#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project_build/build_run.h>
#include <editor/run/launch_plan.h>
#include <editor/run/play_lease.h>
#include <editor/run/play_session.h>

namespace opennova::editor {

class SessionCore;
struct SessionView;

// The mission Play mission starts the game in for the active document (ADR 0046 S14): the
// document's own file when it is a mission (a .bms), else the mission whose name the game finds
// the document's file by (runtime/mission/mission_sidecars.h: its text, its script, its tiles,
// its loading image, its dialog bank and that bank's sounds) when the project holds that .bms;
// its logical name as the project spells it, "" for any other document and for none. The one
// rule the windows, the editor MCP and the tests ask.
std::string play_mission_for(const SessionView &view);

// Play in the project session (ADR 0046 d8, S13 A2): the one game the editor runs (PlaySession
// over the embedder's process seam), what it launches, asked of the embedder's launcher source
// when the game is spawned (after its build lands, so the port of the game's MCP endpoint is
// allocated then, not when Play was asked for), the mission it starts the game in (S14: a .bms of
// the project, the runtime's --mission; the game install starts at its menu whatever was asked),
// the game's log tailed into Output, its boot report (the files it did not find) and its launch
// mission's (the mission that did not load, and why), how it ended (its exit code: a crash told
// from a quit), its lease on the build it runs from (run/play_lease.h), which keeps a build from
// pruning that directory while the game may still run, and the run directory it runs in
// (run/run_directory.h, S13 A8: its working directory and its log, the build directory left as the
// build wrote it).
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

	// Play refused before any build (no spawn on this platform, a game running, a `mission` that is
	// no .bms of the project): true, said why.
	bool refused(const std::string &mission = std::string());
	// The game started on the last build (its lease written, the view's run block filled), in
	// `mission` ("" its menu): the project's .bms of that name as the scan spells it, looked up
	// again now the build landed (one gone since starts nothing, play.mission.unknown). The game
	// install starts at its menu, a note saying so.
	void start(const std::string &mission = std::string());
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
	// The launch mission's report (gameprofile::kLaunchMissionFailedMarker): the mission that did
	// not load and why, a Problems row (play.mission.failed) of the project the game was started in.
	void absorb_mission_report(const std::string &line);
	void absorb_exit();
	// The project's .bms `mission` names, as the scan spells it; "" for none.
	std::string mission_file(const std::string &mission) const;
	// The Play's own findings (a mission that did not load, a nonzero exit) given to Problems, and
	// the validation that makes their rows left due.
	void publish_findings();

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
	std::vector<Diagnostic> findings_; // this Play's own rows: its mission's failure, its crash
};

} // namespace opennova::editor
