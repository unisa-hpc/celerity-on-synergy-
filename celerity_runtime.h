#ifndef CELERITY_RUNTIME
#define CELERITY_RUNTIME

#define CELERITY_NUM_WORKER_NODES 2

#include <cassert>
#include <functional>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>

#include <allscale/api/user/data/grid.h>
#include <SYCL/sycl.hpp>
#include <boost/format.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <boost/type_index.hpp>
#include <boost/variant.hpp>

using namespace allscale::api::user::data;

namespace celerity {

using task_id = size_t;
using buffer_id = size_t;
using node_id = size_t;

// Graphs

using vertex = size_t;

struct tdag_vertex_properties {
  std::string label;

  // Whether this task has been processed into the command dag
  bool processed = false;

  // The number of unsatisfied (= unprocessed) dependencies this task has
  size_t num_unsatisfied = 0;
};

struct tdag_graph_properties {
  std::string name;
};

using task_dag =
    boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS,
                          tdag_vertex_properties, boost::no_property,
                          tdag_graph_properties>;

enum class cdag_command { NOP, COMPUTE, PULL, AWAIT_PULL };

struct cdag_vertex_properties {
  std::string label;
  cdag_command cmd = cdag_command::NOP;
  node_id nid = 0;
};

struct cdag_graph_properties {
  std::string name;
  std::unordered_map<task_id, vertex> task_complete_vertices;
};

using command_dag =
    boost::adjacency_list<boost::vecS, boost::vecS, boost::bidirectionalS,
                          cdag_vertex_properties, boost::no_property,
                          cdag_graph_properties>;

// FIXME: Naming; could be clearer
template <int Dims>
struct subrange {
  // TODO: Should "start" be a cl::sycl::id instead? (What's the difference?)
  // We'll leave it a range for now so we don't have to provide conversion
  // overloads below
  cl::sycl::range<Dims> start;
  cl::sycl::range<Dims> range;
  cl::sycl::range<Dims> global_size;
};

namespace detail {
// FIXME: The input dimensions must match the kernel size, not the buffer
template <int Dims>
using range_mapper_fn = std::function<subrange<Dims>(subrange<Dims> range)>;

class range_mapper_base {
 public:
  range_mapper_base(cl::sycl::access::mode am) : access_mode(am) {}
  cl::sycl::access::mode get_access_mode() const { return access_mode; }

  virtual size_t get_dimensions() const = 0;
  virtual subrange<1> operator()(subrange<1> range) { return subrange<1>(); }
  virtual subrange<2> operator()(subrange<2> range) { return subrange<2>(); }
  virtual subrange<3> operator()(subrange<3> range) { return subrange<3>(); }
  virtual ~range_mapper_base() {}

 private:
  cl::sycl::access::mode access_mode;
};

template <int Dims>
class range_mapper : public range_mapper_base {
 public:
  range_mapper(range_mapper_fn<Dims> fn, cl::sycl::access::mode am)
      : range_mapper_base(am), rmfn(fn) {}
  size_t get_dimensions() const override { return Dims; }
  subrange<Dims> operator()(subrange<Dims> range) override {
    return rmfn(range);
  }

 private:
  range_mapper_fn<Dims> rmfn;
};
}  // namespace detail

// Convenience range mappers
namespace access {
template <int Dims>
struct one_to_one {
  subrange<Dims> operator()(subrange<Dims> range) const { return range; }
};
}  // namespace access

class distr_queue;

// FIXME: Type, dimensions
template <cl::sycl::access::mode Mode>
using accessor =
    cl::sycl::accessor<float, 1, Mode, cl::sycl::access::target::global_buffer>;

// TODO: Looks like we will have to provide the full accessor API
template <cl::sycl::access::mode Mode>
class prepass_accessor {
 public:
  float& operator[](cl::sycl::id<1> index) const { return value; }

 private:
  mutable float value = 0.f;
};

enum class is_prepass { true_t, false_t };

template <is_prepass IsPrepass>
class handler {};

template <>
class handler<is_prepass::true_t> {
 public:
  template <typename Name, typename Functor, int Dims>
  void parallel_for(cl::sycl::range<Dims> global_size, const Functor& kernel) {
    this->global_size = global_size;
    // DEBUG: Find nice name for kernel (regex is probably not super portable)
    auto qualified_name = boost::typeindex::type_id<Name*>().pretty_name();
    std::regex name_regex(R"(.*?(?:::)?([\w_]+)\s?\*.*)");
    std::smatch matches;
    std::regex_search(qualified_name, matches, name_regex);
    debug_name = matches.size() > 0 ? matches[1] : qualified_name;
  }

  template <cl::sycl::access::mode Mode>
  void require(prepass_accessor<Mode> a, size_t buffer_id,
               std::unique_ptr<detail::range_mapper_base> rm);

  ~handler();

 private:
  friend class distr_queue;
  distr_queue& queue;
  size_t task_id;
  std::string debug_name;
  boost::variant<cl::sycl::range<1>, cl::sycl::range<2>, cl::sycl::range<3>>
      global_size;

  handler(distr_queue& q, size_t task_id) : queue(q), task_id(task_id) {
    debug_name = (boost::format("task%d") % task_id).str();
  }
};

template <>
class handler<is_prepass::false_t> {
 public:
  template <typename Name, typename Functor, int Dims>
  void parallel_for(cl::sycl::range<Dims> range, const Functor& kernel) {
    sycl_handler->parallel_for<Name>(range, kernel);
  }

  template <cl::sycl::access::mode Mode>
  void require(accessor<Mode> a, size_t buffer_id) {
    // TODO: Is there any need for this?
  }

  cl::sycl::handler& get_sycl_handler() { return *sycl_handler; }

 private:
  friend class distr_queue;
  distr_queue& queue;
  cl::sycl::handler* sycl_handler;
  size_t task_id;

  // The handler does not take ownership of the sycl_handler, but expects it to
  // exist for the duration of it's lifetime.
  handler(distr_queue& q, size_t task_id, cl::sycl::handler* sycl_handler)
      : queue(q), task_id(task_id), sycl_handler(sycl_handler) {
    this->sycl_handler = sycl_handler;
  }
};

template <typename DataT, int Dims>
class buffer {
 public:
  template <cl::sycl::access::mode Mode>
  prepass_accessor<Mode> get_access(handler<is_prepass::true_t> handler,
                                    detail::range_mapper_fn<Dims> rmfn) {
    prepass_accessor<Mode> a;
    handler.require(a, id,
                    std::make_unique<detail::range_mapper<Dims>>(rmfn, Mode));
    return a;
  }

  template <cl::sycl::access::mode Mode>
  accessor<Mode> get_access(handler<is_prepass::false_t> handler,
                            detail::range_mapper_fn<Dims> rmfn) {
    auto a = accessor<Mode>(sycl_buffer, handler.get_sycl_handler());
    handler.require(a, id);
    return a;
  }

  size_t get_id() { return id; }

  // FIXME Host-size access should block
  DataT operator[](size_t idx) { return 1.f; }

 private:
  friend distr_queue;
  buffer_id id;
  cl::sycl::range<Dims> size;
  cl::sycl::buffer<float, 1> sycl_buffer;

  buffer(DataT* host_ptr, cl::sycl::range<Dims> size, buffer_id bid)
      : id(bid), size(size), sycl_buffer(host_ptr, size){};
};

class branch_handle {
 public:
  template <typename DataT, int Dims>
  void get(buffer<DataT, Dims>, cl::sycl::range<Dims>){};
};

namespace detail {
// This is a workaround that let's us store a command group functor with auto&
// parameter, which we require in order to be able to pass different
// celerity::handlers (celerity::is_prepass::true_t/false_t) for prepass and
// live invocations.
struct cgf_storage_base {
  virtual void operator()(handler<is_prepass::true_t>) = 0;
  virtual void operator()(handler<is_prepass::false_t>) = 0;
  virtual ~cgf_storage_base(){};
};

template <typename CGF>
struct cgf_storage : cgf_storage_base {
  CGF cgf;

  cgf_storage(CGF cgf) : cgf(cgf) {}

  void operator()(handler<is_prepass::true_t> cgh) override { cgf(cgh); }
  void operator()(handler<is_prepass::false_t> cgh) override { cgf(cgh); }
};

inline GridPoint<1> sycl_range_to_grid_point(cl::sycl::range<1> range) {
  return GridPoint<1>(range[0]);
}

inline GridPoint<2> sycl_range_to_grid_point(cl::sycl::range<2> range) {
  return GridPoint<2>(range[0], range[1]);
}

inline GridPoint<3> sycl_range_to_grid_point(cl::sycl::range<3> range) {
  return GridPoint<3>(range[0], range[1], range[2]);
}

template <int Dims>
void clamp_range(cl::sycl::range<Dims>& range,
                 const cl::sycl::range<Dims>& max) {
  if (range[0] > max[0]) {
    range[0] = max[0];
  }
  if (range[1] > max[1]) {
    range[1] = max[1];
  }
  if (range[2] > max[2]) {
    range[2] = max[2];
  }
}

inline GridRegion<1> subrange_to_grid_region(const subrange<1>& sr) {
  auto end = sr.start + sr.range;
  clamp_range(end, sr.global_size);
  return GridRegion<1>(sycl_range_to_grid_point(sr.start),
                       sycl_range_to_grid_point(end));
}

inline GridRegion<2> subrange_to_grid_region(const subrange<2>& sr) {
  auto end = sr.start + sr.range;
  clamp_range(end, sr.global_size);
  return GridRegion<2>(sycl_range_to_grid_point(sr.start),
                       sycl_range_to_grid_point(end));
}

inline GridRegion<3> subrange_to_grid_region(const subrange<3>& sr) {
  auto end = sr.start + sr.range;
  clamp_range(end, sr.global_size);
  return GridRegion<3>(sycl_range_to_grid_point(sr.start),
                       sycl_range_to_grid_point(end));
}

class buffer_state_base {
 public:
  virtual size_t get_dimensions() const = 0;
  virtual ~buffer_state_base(){};
};

template <int Dims>
class buffer_state : public buffer_state_base {
  static_assert(Dims >= 1 && Dims <= 3, "Unsupported dimensionality");

 public:
  buffer_state(cl::sycl::range<Dims> size, size_t num_nodes) {
    std::unordered_set<node_id> all_nodes(num_nodes);
    for (auto i = 0u; i < num_nodes; ++i) all_nodes.insert(i);
    region_nodes.push_back(std::make_pair(
        GridRegion<Dims>(sycl_range_to_grid_point(size)), all_nodes));
  }

  size_t get_dimensions() const override { return Dims; }

  std::vector<std::pair<GridBox<Dims>, std::unordered_set<node_id>>>
  get_source_nodes(GridRegion<Dims> request) const {
    std::vector<std::pair<GridBox<Dims>, std::unordered_set<node_id>>> result;

    // Locate entire region by iteratively removing the largest overlaps
    GridRegion<Dims> remaining = request;
    while (remaining.area() > 0) {
      size_t largest_overlap = 0;
      size_t largest_overlap_i = -1;
      for (auto i = 0u; i < region_nodes.size(); ++i) {
        auto r = GridRegion<Dims>::intersect(region_nodes[i].first, remaining);
        auto area = r.area();
        if (area > largest_overlap) {
          largest_overlap = area;
          largest_overlap_i = i;
        }
      }

      assert(largest_overlap > 0);
      auto r = GridRegion<Dims>::intersect(
          region_nodes[largest_overlap_i].first, remaining);
      remaining = GridRegion<Dims>::difference(remaining, r);
      r.scanByBoxes([this, &result, largest_overlap_i](const GridBox<Dims>& b) {
        result.push_back(
            std::make_pair(b, region_nodes[largest_overlap_i].second));
      });
    }

    return result;
  }

  void update_region(const GridRegion<Dims>& region,
                     const std::unordered_set<node_id>& nodes) {
    auto num_regions = region_nodes.size();
    for (auto i = 0u; i < num_regions; ++i) {
      const size_t overlap =
          GridRegion<Dims>::intersect(region_nodes[i].first, region).area();
      if (overlap == 0) continue;
      const auto diff =
          GridRegion<Dims>::difference(region_nodes[i].first, region);
      if (diff.area() == 0) {
        // New region is larger / equal to stored region - update it
        region_nodes[i].first = region;
        region_nodes[i].second = nodes;
      } else {
        // Stored region needs to be updated as well
        region_nodes[i].first = diff;
        region_nodes.push_back(std::make_pair(region, nodes));
      }
    }

    collapse_regions();
  }

 private:
  // TODO: Look into using a different data structure for this.
  // Maybe order descending by area?
  std::vector<std::pair<GridRegion<Dims>, std::unordered_set<node_id>>>
      region_nodes;

  void collapse_regions() {
    std::set<size_t> erase_indices;
    for (auto i = 0u; i < region_nodes.size(); ++i) {
      const auto& nodes_i = region_nodes[i].second;
      for (auto j = i + 1; j < region_nodes.size(); ++j) {
        const auto& nodes_j = region_nodes[j].second;
        std::vector<node_id> intersection;
        std::set_intersection(nodes_i.cbegin(), nodes_i.cend(),
                              nodes_j.cbegin(), nodes_j.cend(),
                              std::back_inserter(intersection));
        if (intersection.size() == nodes_i.size()) {
          region_nodes[i].first = GridRegion<Dims>::merge(
              region_nodes[i].first, region_nodes[j].first);
          erase_indices.insert(j);
        }
      }
    }

    for (auto it = erase_indices.rbegin(); it != erase_indices.rend(); ++it) {
      region_nodes.erase(region_nodes.begin() + *it);
    }
  }
};
}  // namespace detail

class distr_queue {
 public:
  // TODO: Device should be selected transparently
  distr_queue(cl::sycl::device device);

  template <typename CGF>
  void submit(CGF cgf) {
    const task_id tid = task_count++;
    boost::add_vertex(task_graph);
    handler<is_prepass::true_t> h(*this, tid);
    cgf(h);
    task_command_groups[tid] = std::make_unique<detail::cgf_storage<CGF>>(cgf);
  }

  template <typename DataT, int Dims>
  buffer<DataT, Dims> create_buffer(DataT* host_ptr,
                                    cl::sycl::range<Dims> size) {
    const buffer_id bid = buffer_count++;
    valid_buffer_regions[bid] =
        std::make_unique<detail::buffer_state<Dims>>(size, num_nodes);
    return buffer<DataT, Dims>(host_ptr, size, bid);
  }

  // experimental
  // TODO: Can we derive 2nd lambdas args from requested values in 1st?
  void branch(std::function<void(branch_handle& bh)>,
              std::function<void(float)>){};

  void debug_print_task_graph();
  void TEST_execute_deferred();
  void build_command_graph();

 private:
  friend handler<is_prepass::true_t>;
  // TODO: We may want to move all these task maps into a dedicated struct
  std::unordered_map<task_id, std::unique_ptr<detail::cgf_storage_base>>
      task_command_groups;
  std::unordered_map<task_id,
                     boost::variant<cl::sycl::range<1>, cl::sycl::range<2>,
                                    cl::sycl::range<3>>>
      task_global_sizes;
  std::unordered_map<
      task_id,
      std::unordered_map<
          buffer_id, std::vector<std::unique_ptr<detail::range_mapper_base>>>>
      task_range_mappers;

  // This is a high-level view on buffer writers, for creating the task graph
  // NOTE: This represents the state after the latest performed pre-pass, i.e.
  // it corresponds to the leaf nodes of the current task graph.
  std::unordered_map<buffer_id, task_id> buffer_last_writer;

  // This is a more granular view which encodes where (= on which node) valid
  // regions of a buffer can be found. A valid region is any region that has not
  // been written to on another node.
  // NOTE: This represents the buffer regions after all commands in the current
  // command graph have been completed.
  std::unordered_map<buffer_id, std::unique_ptr<detail::buffer_state_base>>
      valid_buffer_regions;

  size_t task_count = 0;
  size_t buffer_count = 0;
  task_dag task_graph;
  command_dag command_graph;

  // For now we don't store any additional data on nodes
  const size_t num_nodes;

  cl::sycl::queue sycl_queue;

  void add_requirement(task_id tid, buffer_id bid, cl::sycl::access::mode mode,
                       std::unique_ptr<detail::range_mapper_base> rm);

  template <int Dims>
  void set_task_data(task_id tid, cl::sycl::range<Dims> global_size,
                     std::string debug_name) {
    task_global_sizes[tid] = global_size;
    task_graph[tid].label =
        (boost::format("Task %d (%s)") % tid % debug_name).str();
  }
};

}  // namespace celerity

#endif
