#include <cstdio>

#include <celerity.h>

const size_t MAT_SIZE = 1024;

template <typename T>
void set_identity(celerity::distr_queue queue, celerity::buffer<T, 2> mat) {
	queue.submit([=](celerity::handler& cgh) {
		celerity::accessor dw{mat, cgh, celerity::access::one_to_one{}, celerity::write_only, celerity::no_init};
		cgh.parallel_for<class set_identity_kernel>(mat.get_range(), [=](celerity::item<2> item) { dw[item] = item[0] == item[1]; });
	});
}

template <typename T>
void multiply(celerity::distr_queue queue, celerity::buffer<T, 2> mat_a, celerity::buffer<T, 2> mat_b, celerity::buffer<T, 2> mat_c) {
	queue.submit([=](celerity::handler& cgh) {
		celerity::accessor a{mat_a, cgh, celerity::access::slice<2>(1), celerity::read_only};
		celerity::accessor b{mat_b, cgh, celerity::access::slice<2>(0), celerity::read_only};
		celerity::accessor c{mat_c, cgh, celerity::access::one_to_one{}, celerity::write_only, celerity::no_init};

		// Use local-memory tiling to avoid waiting on global memory too often
		// Note: We assume a local range size of 64 here, this should be supported by most devices.
		const size_t group_size = 8;
		celerity::local_accessor<T, 2> scratch_a{{group_size, group_size}, cgh};
		celerity::local_accessor<T, 2> scratch_b{{group_size, group_size}, cgh};

		cgh.parallel_for<class mat_mul>(celerity::nd_range<2>{{MAT_SIZE, MAT_SIZE}, {group_size, group_size}}, [=](celerity::nd_item<2> item) {
			T sum{};
			const auto lid = item.get_local_id();
			for(size_t j = 0; j < MAT_SIZE; j += group_size) {
				scratch_a[lid] = a[item.get_group(0) * group_size + lid[0]][j + lid[1]];
				scratch_b[lid] = b[j + lid[0]][item.get_group(1) * group_size + lid[1]];
				celerity::group_barrier(item.get_group());

				for(size_t k = 0; k < group_size; ++k) {
					const auto a_ik = scratch_a[lid[0]][k];
					const auto b_kj = scratch_b[k][lid[1]];
					sum += a_ik * b_kj;
				}
				celerity::group_barrier(item.get_group());
			}
			c[item.get_global_id()] = sum;
		});
	});
}

// TODO this should really reduce into a buffer<bool> on the device, but not all backends currently support reductions
template <typename T>
void verify(celerity::distr_queue& queue, celerity::buffer<T, 2> mat_c, celerity::buffer<bool> passed_buf) {
	queue.submit([=](celerity::handler& cgh) {
		celerity::accessor c{mat_c, cgh, celerity::access::one_to_one{}, celerity::read_only_host_task};
		// omitting no_init here to force verification failure if the kernel does not execute for some reason:
		celerity::accessor passed{passed_buf, cgh, celerity::access::all{}, celerity::write_only_host_task};

		cgh.host_task(mat_c.get_range(), [=](celerity::partition<2> part) {
			passed[0] = true;
			const auto& sr = part.get_subrange();
			for(size_t i = sr.offset[0]; i < sr.offset[0] + sr.range[0]; ++i) {
				for(size_t j = sr.offset[1]; j < sr.offset[1] + sr.range[1]; ++j) {
					const float received = c[i][j];
					const float expected = i == j;
					if(expected != received) {
						CELERITY_ERROR("Verification failed for element {},{}: {} (received) != {} (expected)", i, j, received, expected);
						passed[0] = false;
					}
				}
			}
			if(passed[0]) { CELERITY_INFO("Verification passed for {}", part.get_subrange()); }
		});
	});
}

int main() {
	celerity::distr_queue queue;

	const auto range = celerity::range<2>(MAT_SIZE, MAT_SIZE);
	celerity::buffer<float, 2> mat_a_buf(range);
	celerity::buffer<float, 2> mat_b_buf(range);
	celerity::buffer<float, 2> mat_c_buf(range);

	celerity::debug::set_buffer_name(mat_a_buf, "mat_a");
	celerity::debug::set_buffer_name(mat_b_buf, "mat_b");

	set_identity(queue, mat_a_buf);
	set_identity(queue, mat_b_buf);

	multiply(queue, mat_a_buf, mat_b_buf, mat_c_buf);
	multiply(queue, mat_b_buf, mat_c_buf, mat_a_buf);

	const bool passed_init = false;
	celerity::buffer<bool> passed_buf(&passed_init, 1);
	verify(queue, mat_c_buf, passed_buf);

	// The value of `passed` can differ between hosts if only part of the verification failed.
	const auto passed = celerity::experimental::fence(queue, passed_buf).get()[0];
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
