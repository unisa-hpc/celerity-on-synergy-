#include "buffer_transfer_manager.h"

#include <cassert>
#include <climits>

#include "buffer_manager.h"
#include "log.h"
#include "mpi_support.h"
#include "reduction_manager.h"
#include "runtime.h"

namespace celerity {
namespace detail {

	std::shared_ptr<const buffer_transfer_manager::transfer_handle> buffer_transfer_manager::push(const command_pkg& pkg) {
		assert(pkg.get_command_type() == command_type::PUSH);
		auto t_handle = std::make_shared<transfer_handle>();
		// We are blocking the caller until the buffer has been copied and submitted to MPI
		// TODO: Investigate doing this in worker thread
		// --> This probably needs some kind of heuristic, as for small (e.g. ghost cell) transfers the overhead of threading is way too big
		const push_data& data = std::get<push_data>(pkg.data);

		const auto raw_data =
		    runtime::get_instance().get_buffer_manager().get_buffer_data(data.bid, cl::sycl::range<3>(data.sr.offset[0], data.sr.offset[1], data.sr.offset[2]),
		        cl::sycl::range<3>(data.sr.range[0], data.sr.range[1], data.sr.range[2]));

		unique_frame_ptr<data_frame> frame(from_payload_size, raw_data.get_size());
		frame->sr = data.sr;
		frame->bid = data.bid;
		frame->rid = data.rid;
		frame->push_cid = pkg.cid;
		memcpy(frame->data, raw_data.get_pointer(), raw_data.get_size());

		CELERITY_TRACE("Ready to send {} of buffer {} ({} B) to {}", data.sr, data.bid, frame.get_payload_size(), data.target);

		// Start transmitting data
		MPI_Request req;
		MPI_Isend(frame.get_pointer(), static_cast<int>(frame.get_frame_size_bytes()), MPI_BYTE, static_cast<int>(data.target), mpi_support::TAG_DATA_TRANSFER,
		    MPI_COMM_WORLD, &req);

		auto transfer = std::make_unique<transfer_out>();
		transfer->handle = t_handle;
		transfer->request = req;
		transfer->frame = std::move(frame);
		outgoing_transfers.push_back(std::move(transfer));

		return t_handle;
	}

	std::shared_ptr<const buffer_transfer_manager::transfer_handle> buffer_transfer_manager::await_push(const command_pkg& pkg) {
		assert(pkg.get_command_type() == command_type::AWAIT_PUSH);
		const auto& data = std::get<await_push_data>(pkg.data);

		std::shared_ptr<incoming_transfer_handle> t_handle;
		// Check to see if we have (fully) received the push already
		if(push_blackboard.count(data.source_cid) != 0) {
			t_handle = push_blackboard[data.source_cid];
			push_blackboard.erase(data.source_cid);
			assert(t_handle->transfer != nullptr);
			assert(t_handle->transfer->frame->bid == data.bid);
			assert(t_handle->transfer->frame->rid == data.rid);
			assert(t_handle->transfer->frame->sr == data.sr);
			assert(t_handle->complete);
			commit_transfer(*t_handle->transfer);
		} else {
			t_handle = std::make_shared<incoming_transfer_handle>();
			// Store new handle so we can mark it as complete when the push is received
			push_blackboard[data.source_cid] = t_handle;
		}

		return t_handle;
	}

	void buffer_transfer_manager::poll() {
		poll_incoming_transfers();
		update_incoming_transfers();
		update_outgoing_transfers();
	}

	void buffer_transfer_manager::poll_incoming_transfers() {
		MPI_Status status;
		int flag;
		MPI_Message msg;
		MPI_Improbe(MPI_ANY_SOURCE, mpi_support::TAG_DATA_TRANSFER, MPI_COMM_WORLD, &flag, &msg, &status);
		if(flag == 0) {
			// No incoming transfers at the moment
			return;
		}
		int frame_bytes;
		MPI_Get_count(&status, MPI_BYTE, &frame_bytes);

		auto transfer = std::make_unique<transfer_in>();
		transfer->source_nid = static_cast<node_id>(status.MPI_SOURCE);
		transfer->frame = unique_frame_ptr<data_frame>(from_frame_bytes, static_cast<size_t>(frame_bytes));

		// Start receiving data
		MPI_Imrecv(transfer->frame.get_pointer(), frame_bytes, MPI_BYTE, &msg, &transfer->request);
		incoming_transfers.push_back(std::move(transfer));

		CELERITY_TRACE("Receiving incoming data of size {} B from {}", frame_bytes, status.MPI_SOURCE);
	}

	void buffer_transfer_manager::update_incoming_transfers() {
		for(auto it = incoming_transfers.begin(); it != incoming_transfers.end();) {
			auto& transfer = *it;
			int flag;
			MPI_Test(&transfer->request, &flag, MPI_STATUS_IGNORE);
			if(flag == 0) {
				++it;
				continue;
			}

			// Check whether we already have an await push request
			std::shared_ptr<incoming_transfer_handle> t_handle = nullptr;
			if(push_blackboard.count(transfer->frame->push_cid) != 0) {
				t_handle = push_blackboard[transfer->frame->push_cid];
				push_blackboard.erase(transfer->frame->push_cid);
				assert(t_handle.use_count() > 1 && "Dangling await push request");
				t_handle->transfer = std::move(*it);
				commit_transfer(*t_handle->transfer);
				t_handle->complete = true;
			} else {
				t_handle = std::make_shared<incoming_transfer_handle>();
				push_blackboard[transfer->frame->push_cid] = t_handle;
				t_handle->transfer = std::move(*it);
				t_handle->complete = true;
			}
			it = incoming_transfers.erase(it);
		}
	}

	void buffer_transfer_manager::update_outgoing_transfers() {
		for(auto it = outgoing_transfers.begin(); it != outgoing_transfers.end();) {
			auto& t = *it;
			int flag;
			MPI_Test(&t->request, &flag, MPI_STATUS_IGNORE);
			if(flag == 0) {
				++it;
				continue;
			}
			t->handle->complete = true;
			it = outgoing_transfers.erase(it);
		}
	}

	void buffer_transfer_manager::commit_transfer(transfer_in& transfer) {
		const auto& frame = *transfer.frame;
		const size_t elem_size = transfer.frame.get_payload_size() / (frame.sr.range[0] * frame.sr.range[1] * frame.sr.range[2]);
		raw_buffer_data raw_data{elem_size, frame.sr.range};
		memcpy(raw_data.get_pointer(), frame.data, raw_data.get_size());
		if(frame.rid) {
			auto& rm = runtime::get_instance().get_reduction_manager();
			// In some rare situations the local runtime might not yet know about this reduction. Busy wait until it does.
			while(!rm.has_reduction(frame.rid)) {}
			rm.push_overlapping_reduction_data(frame.rid, transfer.source_nid, std::move(raw_data));
		} else {
			auto& bm = runtime::get_instance().get_buffer_manager();
			// In some rare situations the local runtime might not yet know about this buffer. Busy wait until it does.
			while(!bm.has_buffer(frame.bid)) {}
			bm.set_buffer_data(frame.bid, frame.sr.offset, std::move(raw_data));
		}
		transfer.frame = {};
	}

} // namespace detail
} // namespace celerity
