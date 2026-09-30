#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/run/play_state.h>
#include <editor/session/output_log.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

struct BuildReport;

// What the editor is doing and has said, as the view shows it (ADR 0046 S13 V4; the Operation,
// Run and Output concerns): the running operation and what the last one came to, the last
// build's report, the game Play started, the Output lines and the status line. The build's
// report is shared and never null (an ActivityView made empty holds an empty one), so a header
// naming the view pulls none of the build's headers.
struct ActivityView {
	ActivityView(); // the build's report made, empty

	// The operation that runs (a build: its progress, what it works on, whether it can be
	// cancelled, what it reads and writes; id 0 when none), and what the last one came to (id 0
	// before the first ends).
	OperationStatus operation;
	OperationOutcome last_operation;
	bool has_build = false;
	std::shared_ptr<const BuildReport> last_build;

	PlayState play_state = PlayState::Stopped;
	int64_t play_pid = -1;
	// The port the running game's MCP endpoint answers on (0: no game, or none).
	int play_mcp_port = 0;
	std::string play_command_line;
	bool play_exited_on_its_own = false;
	// The code the last game exited with on its own (PlaySession::exit_code; -1: none, or it
	// was stopped). Nonzero, it is a Problems row (play.crashed) until Play starts again or
	// the project closes.
	int64_t play_exit_code = -1;
	std::string runtime_executable; // what Play launches (resolved; "" = none found)
	bool source_run = false;        // OpenNova Play drives the Godot binary at the source project
	// The files the running (or last) game of this project reported missing when it
	// booted, from its log: each is a Problems row (play.boot_missing) until Play starts
	// again or the project closes; a report from a game of another project is ignored.
	std::vector<std::string> boot_missing;
	// True when the last boot report named `name`, compared as the game compares names.
	bool missing_at_boot(const std::string &name) const;

	OutputLog output;   // what the editor said and the running game's log, oldest first
	std::string status; // the last thing that happened, one line
};

} // namespace opennova::editor
