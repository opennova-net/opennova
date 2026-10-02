#pragma once

#include <cstdint>
#include <string>

#include <editor/project_build/build_run.h>
#include <editor/run/launch_plan.h>
#include <editor/run/play_lease.h>
#include <editor/run/play_session.h>

namespace opennova::editor {

class SessionCore;
struct SessionView;

// Play in the project session (ADR 0046 d8, S13 A2): the one game the editor runs (PlaySession
// over the embedder's process seam), what it launches, asked of the embedder's launcher source
// when the game is spawned (after its build lands, so the port of the game's MCP endpoint is
// allocated then, not when Play was asked for), the game's log tailed into Output, its boot
// report (the files it did not find), how it ended (its exit code: a crash told from a quit), its
// lease on the build it runs from (run/play_lease.h), which keeps a build from pruning that
// directory while the game may still run, and the run directory it runs in (run/run_directory.h,
// S13 A8: its working directory and its log, the build directory left as the build wrote it).
class PlayController {
public:
	explicit PlayController(SessionCore &core);
	~PlayController();
	PlayController(const PlayController &) = delete;
	PlayController &operator=(const PlayController &) = delete;

	// What Play launches, from the embedder: asked once now for what the view shows (the runtime,
	// whether the run drives the source checkout), and again each time the game is spawned, with a
	// port unless the game install runs; the game is launched from that one answer, and the view's
	// runtime follows it.
	void set_launcher_source(PlayLauncherSource source);
	// The runtime Play launches, resolved (the source run's binary, the project's local setting,
	// the editor's, else the one packaged beside the editor; "" when none).
	std::string resolve_runtime_executable() const;

	// Play refused before any build (no spawn on this platform, a game running): true, said why.
	bool refused();
	// The game started on the last build (its lease written, the view's run block filled).
	void start();
	void stop();
	// The poll's: the child's state, the game's log tail, how it ended once it has; the validation
	// its report left due.
	void poll();
	// The project closes: its game's boot report and exit are forgotten.
	void forget_project();

	// The directories a build must not prune, asked when it publishes: this editor's game's, and
	// every one whose lease names a process that may still run.
	ProtectedDirs protected_dirs(const std::string &output_root);
	// The directory the running game uses ("" when none).
	std::string running_build_dir() const { return play_.running_build_dir(); }

private:
	void follow_launcher(const PlayLauncher &launcher);
	void tail_game_log();
	void absorb_boot_report(const std::string &line);
	void absorb_exit();

	SessionCore &core_;
	SessionView &view_;
	PlaySession play_;
	PlayLease play_lease_; // the running game's lease, as written (pid -1: none)
	std::string run_dir_;  // the running game's run directory, its record written ("" for none)
	PlayLauncherSource source_;
	PlayLauncher launcher_; // what the source said without a port: the view's runtime
	std::string game_log_file_;
	uint64_t game_log_offset_ = 0;
	std::string game_log_partial_;
	std::string boot_project_; // the project the game was started in: its boot report is that project's
};

} // namespace opennova::editor
