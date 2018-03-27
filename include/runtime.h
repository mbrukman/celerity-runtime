#pragma once

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <mpi.h>

#include "buffer_state.h"
#include "buffer_storage.h"
#include "buffer_transfer_manager.h"
#include "distr_queue.h"
#include "graph.h"
#include "types.h"
#include "worker_job.h"

namespace celerity {

class runtime {
  public:
	static void init(int* argc, char** argv[]);
	static runtime& get_instance();

	~runtime();

	void TEST_do_work();
	void register_queue(distr_queue* queue);

	template <typename DataT, int Dims>
	buffer_id register_buffer(cl::sycl::range<Dims> size, cl::sycl::buffer<DataT, Dims>& buf) {
		const buffer_id bid = buffer_count++;
		valid_buffer_regions[bid] = std::make_unique<detail::buffer_state<Dims>>(size, num_nodes);
		buffer_ptrs[bid] = std::make_unique<detail::buffer_storage<DataT, Dims>>(buf);
		return bid;
	}

	void unregister_buffer(buffer_id bid) {
		buffer_ptrs.erase(bid);
		valid_buffer_regions.erase(bid);
	}

	detail::raw_data_range get_buffer_data(buffer_id bid, const cl::sycl::range<3>& offset, const cl::sycl::range<3>& range) {
		assert(buffer_ptrs.at(bid) != nullptr);
		return buffer_ptrs[bid]->get_data_range(offset, range);
	}

	void set_buffer_data(buffer_id bid, const detail::raw_data_range& dr) {
		assert(buffer_ptrs.at(bid) != nullptr);
		buffer_ptrs[bid]->set_data_range(dr);
	}

	void schedule_buffer_send(node_id recipient, const command_pkg& pkg);

  private:
	static std::unique_ptr<runtime> instance;

	distr_queue* queue = nullptr;
	size_t num_nodes;
	bool is_master;

	size_t buffer_count = 0;
	std::unordered_map<buffer_id, std::unique_ptr<detail::buffer_storage_base>> buffer_ptrs;

	// This is a data structe view which encodes where (= on which node) valid
	// regions of a buffer can be found. A valid region is any region that has not
	// been written to on another node.
	// NOTE: This represents the buffer regions after all commands in the current
	// command graph have been completed.
	std::unordered_map<buffer_id, std::unique_ptr<detail::buffer_state_base>> valid_buffer_regions;

	command_dag command_graph;

	buffer_transfer_manager btm;
	job_set jobs;

	runtime(int* argc, char** argv[]);
	runtime(const runtime&) = delete;
	runtime(runtime&&) = delete;

	void build_command_graph();
};

} // namespace celerity