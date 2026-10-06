#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/value.h>
#include <editor/run/launch_plan.h>
#include <editor/run/play_state.h>
#include <editor/session/output_log.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

struct BuildReport;
struct ExportReport;

// The project's validation as the poll steps it (ProblemsService, ADR 0046 S13 A3): whether one is
// due or under way, and the files it has asked for their own findings of those it asks (0 of 0
// before its first step, the graph's update).
struct ValidationStatus {
	bool running = false;
	uint64_t done = 0;
	uint64_t total = 0;
	// A validation has ended since the project opened: the graph has read the project's references
	// (the pairings a preview looks up), whatever validation runs again after an edit.
	bool read = false;
	// The graph has not read the project's files as the scan lists them now (an import's, a rescan's):
	// what names a file is known as far as the files read before.
	bool files_unread = false;
	bool operator==(const ValidationStatus &o) const {
		return running == o.running && done == o.done && total == o.total && read == o.read &&
		       files_unread == o.files_unread;
	}
	bool operator!=(const ValidationStatus &o) const { return !(*this == o); }
};

// What the editor is doing and has said, as the view shows it (ADR 0046 S13 V4; the Operation,
// Run and Output concerns): the running operation and what the last one came to, the last
// build's report, the game Play started, the Output lines and the status line. The build's
// report is shared and never null (an ActivityView made empty holds an empty one), so a header
// naming the view pulls none of the build's headers.
struct ActivityView {
	ActivityView(); // the build's and the export's reports made, empty

	// The operation that runs (a build: its progress, what it works on, whether it can be
	// cancelled, what it reads and writes; id 0 when none), and what the last one came to (id 0
	// before the first ends).
	OperationStatus operation;
	OperationOutcome last_operation;
	// The validation the poll steps a file at a time while no request waits on it (the first one
	// after a project opens, above all): the Problems rows are the last ones composed until it ends.
	ValidationStatus validation;
	bool has_build = false;
	std::shared_ptr<const BuildReport> last_build;
	// The last Export's copy of a build (ADR 0046 S16; never null, as the build's report).
	bool has_export = false;
	std::shared_ptr<const ExportReport> last_export;

	PlayState play_state = PlayState::Stopped;
	int64_t play_pid = -1;
	// The port the running game's MCP endpoint answers on (0: no game, or none).
	int play_mcp_port = 0;
	std::string play_command_line;
	// The mission the running (or last) game was started in, as the project spells its file
	// ("" at its menu: a plain Play, or the game install, which starts there whatever was asked).
	// One that did not load is a Problems row (play.mission.failed) until Play starts again or the
	// project closes.
	std::string play_mission;
	// Whether the running (or last) game was started behind every other window (play's behind, the MCP gaps
	// lane).
	bool play_behind = false;
	// Where the running (or last) game runs, its run directory (run/run_directory.h), and the log
	// Play tails there ("" before the first Play): never the build directory it runs from.
	std::string play_run_dir;
	std::string play_log_file;
	// Whether the running (or last) game's run directory was emptied first (play's fresh), and what it kept
	// of what the runs before wrote there ('/'-separated paths under it: the game's game.cfg, its saves),
	// run/run_directory.h.
	bool play_fresh = false;
	std::vector<std::string> play_kept;
	// The running (or last) game is the game install's under Strict Play (the build and the install's
	// program alone, no /d), and its first run, which wrote its game.cfg and quit, was started again.
	bool play_strict = false;
	bool play_started_again = false;
	// What the last game install's game loaded, read from its file log (_filelog.txt) once it had exited
	// (never while it runs: the game's exclusive appends would cut a log a reader holds open): whether the
	// log was there to read, and what it names (FileAccessLog: the archives, what they served, what was
	// opened from disk). Cleared as a game starts; a game that loaded nothing leaves no log.
	bool play_file_log_read = false;
	FileAccessLog play_file_log;
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

	// The last rename that finished in this project (the UX round's problems lane): Undo does not take
	// a rename back (it rewrote files, not a step of a document's history), so the Edit menu offers the
	// rename back (RenameController's rename_back: only the sites it rewrote). A name's (`symbol`: its
	// defining file, the record's locator and the field, its kind and scope) or a file's (`path` its new
	// path); `from` the old name, `to` the new.
	struct LastRename {
		bool made = false;
		bool symbol = false;
		std::string path, locator, field, from, to;
		ReferenceKind kind = ReferenceKind::None;
		std::string scope;
	};
	LastRename last_rename;
};

} // namespace opennova::editor
