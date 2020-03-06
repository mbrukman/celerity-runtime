#include "print_graph.h"

// As of Boost 1.70, this includes a header which contains a __noinline__ attribute
// for __GNUC__ == 4 (which Clang (8) apparently also identifies as).
// This breaks CUDA compilation with Clang, as the CUDA (10) headers define __noinline__
// in an incompatible manner. As a workaround we thus simply undefine it altogether.
// Potentially related to https://svn.boost.org/trac10/ticket/9392
#if defined(__clang__) && defined(__CUDA__)
#undef __noinline__
#endif
#include <boost/graph/graphviz.hpp>

#include <boost/algorithm/string.hpp>
#include <spdlog/fmt/fmt.h>

#include "command.h"
#include "command_graph.h"
#include "grid.h"
#include "logger.h"

namespace celerity {
namespace detail {

	std::string get_task_label(const task* tsk) {
		switch(tsk->get_type()) {
		case task_type::COMPUTE: return fmt::format("Task {} ({})", tsk->get_id(), static_cast<const compute_task*>(tsk)->get_debug_name());
		case task_type::MASTER_ACCESS: return fmt::format("Task {} (master-access)", tsk->get_id());
		default: return fmt::format("Task {} (UNKNOWN)");
		}
	}

	void print_graph(const std::unordered_map<task_id, std::shared_ptr<task>>& tdag, logger& graph_logger) {
		std::stringstream ss;
		ss << "digraph G { ";

		for(auto& it : tdag) {
			if(it.first == 0) continue; // Skip INIT task
			const auto tsk = it.second.get();

			std::unordered_map<std::string, std::string> props;
			props["label"] = boost::escape_dot_string(get_task_label(tsk));

			ss << tsk->get_id();
			ss << "[";
			for(const auto& it : props) {
				ss << " " << it.first << "=" << it.second;
			}
			ss << "];";

			for(auto d : tsk->get_dependencies()) {
				if(d.node->get_id() == 0) continue; // Skip INIT task
				ss << fmt::format("{} -> {}{};", d.node->get_id(), tsk->get_id(), d.is_anti ? " [color=limegreen]" : "");
			}
		}

		ss << "}";
		auto str = ss.str();
		boost::replace_all(str, "\n", "\\n");
		boost::replace_all(str, "\"", "\\\"");
		graph_logger.info(logger_map({{"name", "TaskGraph"}, {"data", str}}));
	}

	std::string get_command_label(const abstract_command* cmd) {
		const std::string label = fmt::format("[{}] Node {}:\\n", cmd->get_cid(), cmd->get_nid());
		if(isa<compute_command>(cmd)) {
			return label
			       + fmt::format("COMPUTE {}\\n{}", subrange_to_grid_region(static_cast<const compute_command*>(cmd)->get_execution_range()), cmd->debug_label);
		}
		if(isa<master_access_command>(cmd)) { return label + "MASTER ACCESS\\n" + cmd->debug_label; }
		if(isa<push_command>(cmd)) {
			const auto push = static_cast<const push_command*>(cmd);
			return label + fmt::format("PUSH {} to {}\\n {}", push->get_bid(), push->get_target(), subrange_to_grid_region(push->get_range()));
		}
		if(isa<await_push_command>(cmd)) {
			const auto await_push = static_cast<const await_push_command*>(cmd);
			return label
			       + fmt::format("AWAIT PUSH {} from {}\\n {}", await_push->get_source()->get_bid(), await_push->get_source()->get_nid(),
			           subrange_to_grid_region(await_push->get_source()->get_range()));
		}
		if(isa<horizon_command>(cmd)) { return label + "HORIZON"; }
		return fmt::format("[{}] UNKNOWN\\n{}", cmd->get_cid(), cmd->debug_label);
	}

	void print_graph(const command_graph& cdag, logger& graph_logger) {
		std::stringstream main_ss;
		std::unordered_map<task_id, std::stringstream> task_subgraph_ss;

		const auto write_vertex = [&](std::ostream& out, abstract_command* cmd) {
			const char* colors[] = {"black", "crimson", "dodgerblue4", "goldenrod", "maroon4", "springgreen2", "tan1", "chartreuse2"};

			std::unordered_map<std::string, std::string> props;
			props["label"] = boost::escape_dot_string(get_command_label(cmd));
			props["fontcolor"] = colors[cmd->get_nid() % (sizeof(colors) / sizeof(char*))];
			if(isa<compute_command>(cmd)) { props["shape"] = "box"; }

			out << cmd->get_cid();
			out << "[";
			for(const auto& it : props) {
				out << " " << it.first << "=" << it.second;
			}
			out << "];";
		};

		const auto write_command = [&](auto* cmd) {
			if(isa<nop_command>(cmd)) return;

			if(const auto tcmd = dynamic_cast<task_command*>(cmd)) {
				// Add to subgraph as well
				if(task_subgraph_ss.find(tcmd->get_tid()) == task_subgraph_ss.end()) {
					// TODO: Can we print the task debug label here as well?
					task_subgraph_ss[tcmd->get_tid()] << fmt::format("subgraph cluster_{} {{ label=\"Task {}\"; color=gray;", tcmd->get_tid(), tcmd->get_tid());
				}
				write_vertex(task_subgraph_ss[tcmd->get_tid()], cmd);
			} else {
				write_vertex(main_ss, cmd);
			}

			for(auto d : cmd->get_dependencies()) {
				if(isa<nop_command>(d.node)) continue;
				main_ss << fmt::format("{} -> {}{};", d.node->get_cid(), cmd->get_cid(), d.is_anti ? " [color=limegreen]" : "");
			}

			// Add a dashed line to the corresponding PUSH
			if(isa<await_push_command>(cmd)) {
				auto await_push = static_cast<await_push_command*>(cmd);
				main_ss << fmt::format("{} -> {} [style=dashed color=gray40];", await_push->get_source()->get_cid(), cmd->get_cid());
			}
		};

		for(auto cmd : cdag.all_commands()) {
			write_command(cmd);
		}

		// Close all subgraphs
		for(auto& sg : task_subgraph_ss) {
			sg.second << "}";
		}

		std::stringstream ss;
		ss << "digraph G { ";
		for(auto& sg : task_subgraph_ss) {
			ss << sg.second.str();
		}
		ss << main_ss.str();
		ss << "}";

		auto str = ss.str();
		boost::replace_all(str, "\n", "\\n");
		boost::replace_all(str, "\"", "\\\"");
		graph_logger.info(logger_map({{"name", "CommandGraph"}, {"data", str}}));
	}

} // namespace detail
} // namespace celerity
