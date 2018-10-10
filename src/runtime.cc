#include "runtime.h"

#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <mpi.h>

#include "buffer.h"
#include "buffer_storage.h"
#include "command.h"
#include "executor.h"
#include "graph_generator.h"
#include "graph_utils.h"
#include "logger.h"
#include "scheduler.h"
#include "task_manager.h"

namespace celerity {

std::unique_ptr<runtime> runtime::instance = nullptr;
bool runtime::test_skip_mpi_lifecycle = false;

void runtime::init(int* argc, char** argv[]) {
	instance = std::unique_ptr<runtime>(new runtime(argc, argv));
}

runtime& runtime::get_instance() {
	if(instance == nullptr) { throw std::runtime_error("Runtime has not been initialized"); }
	return *instance;
}

runtime::runtime(int* argc, char** argv[]) {
	if(!test_skip_mpi_lifecycle) {
		int provided;
		MPI_Init_thread(argc, argv, MPI_THREAD_MULTIPLE, &provided);
		assert(provided == MPI_THREAD_MULTIPLE);
	}

	int world_size;
	MPI_Comm_size(MPI_COMM_WORLD, &world_size);
	num_nodes = world_size;

	int world_rank;
	MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
	is_master = world_rank == 0;

	default_logger = logger("default").create_context({{"rank", std::to_string(world_rank)}});
	graph_logger = logger("graph").create_context({{"rank", std::to_string(world_rank)}});
}

runtime::~runtime() {
	// Allow BTM to clean up MPI data types before we finalize
	btm.reset();
	if(!test_skip_mpi_lifecycle) { MPI_Finalize(); }
}

void runtime::startup(distr_queue* queue) {
	if(this->queue != nullptr) { throw std::runtime_error("Only one celerity::distr_queue can be created per process"); }
	this->queue = queue;

	task_mngr = std::make_shared<detail::task_manager>();
	queue->set_task_manager(task_mngr);

	btm = std::make_unique<buffer_transfer_manager>(default_logger);
	executor = std::make_unique<detail::executor>(*queue, *task_mngr, *btm, default_logger);

	if(is_master) {
		ggen = std::make_shared<detail::graph_generator>(num_nodes, *task_mngr, [this](node_id target, const command_pkg& pkg) { flush_command(target, pkg); });
		scheduler = std::make_unique<detail::scheduler>(ggen, num_nodes);
		scheduler->startup();

		task_mngr->register_task_callback([this]() { scheduler->notify_task_created(); });
	}

	executor->startup();
}

void runtime::shutdown() {
	assert(queue != nullptr);
	if(is_master) {
		scheduler->shutdown();

		// Send shutdown commands to all worker nodes.
		// FIXME: This is a bit of hack, since we don't know the last command id issued. Maybe we should generate actual commands in the graph for this.
		command_id base_cmd_id = std::numeric_limits<command_id>::max() - num_nodes;
		for(auto n = 0u; n < num_nodes; ++n) {
			command_pkg pkg{0, base_cmd_id + n, command::SHUTDOWN, command_data{}};
			flush_command(n, pkg);
		}
	}

	executor->shutdown();

	if(is_master) {
		task_mngr->print_graph(graph_logger);
		ggen->print_graph(graph_logger);
	}
}

detail::task_manager& runtime::get_task_manager() {
	return *task_mngr;
}

buffer_id runtime::register_buffer(cl::sycl::range<3> range, std::shared_ptr<detail::buffer_storage_base> buf_storage) {
	buf_storage->set_type(is_master ? detail::buffer_type::HOST_BUFFER : detail::buffer_type::DEVICE_BUFFER);
	const buffer_id bid = buffer_count++;
	buffer_ptrs[bid] = buf_storage;
	if(is_master) { ggen->add_buffer(bid, range); }
	return bid;
}

void runtime::free_buffers() {
	buffer_ptrs.clear();
}

void runtime::flush_command(node_id target, const command_pkg& pkg) {
	// Even though command packages are small enough to use a blocking send we want to be able to send to the master node as well,
	// which is why we have to use Isend after all. We also have to make sure that the buffer stays around until the send is complete.
	active_flushes.emplace_back(pkg, MPI_REQUEST_NULL);
	MPI_Isend(&active_flushes.rbegin()->first, sizeof(command_pkg), MPI_BYTE, static_cast<int>(target), CELERITY_MPI_TAG_CMD, MPI_COMM_WORLD,
	    &active_flushes.rbegin()->second);

	// Just check the oldest flush. Since commands are small this will stay in equilibrium fairly quickly.
	int done;
	MPI_Test(&active_flushes.begin()->second, &done, MPI_STATUS_IGNORE);
	if(done) { active_flushes.pop_front(); }
}

} // namespace celerity
