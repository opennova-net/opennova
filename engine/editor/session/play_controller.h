#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project_build/build_run.h>
#include <editor/run/launch_plan.h>
#include <editor/run/play_lease.h>
#include <editor/run/play_session.h>
#include <editor/run/play_start.h>
#include <editor/session/build_operation.h>
#include <editor/session/play_log.h>

namespace opennova::editor {

class SessionCore;
struct EditorRequest;
struct SessionView;

// The mission Play mission starts the game in for the active document (ADR 0046 S14): the
// document's own file when it is a mission (a .bms), else the mission whose name the game finds
// the document's file by (runtime/mission/mission_sidecars.h: its text, its script, its tiles,
// its loading image, its dialog bank and that bank's sounds) when the project holds that .bms;
// its logical name as the project spells it, "" for any other document and for none. The one
// rule the windows, the editor MCP and the tests ask.
std::string play_mission_for(const SessionView &view);
// The mission Play starts the game in for the file at `path`: its logical name when it is a .bms the
// project holds (a mission view's, a Files row's), else "".
std::string play_mission_at(const SessionView &view, const std::string &path);

// Play in the project session (ADR 0046 d8, S13 A2): the one game the editor runs (PlaySession
// over the embedder's process seam), what it launches, asked of the embedder's launcher source
// when the game is spawned (after its build lands, so the port of the game's MCP endpoint is
// allocated then, not when Play was asked for), the mission it starts the game in (S14: a .bms of
// the project, the runtime's --mission; the game install starts at its menu whatever was asked),
// the game's log tailed into Output (the game install's file log read once its game exited, never
// while it runs, and what it says the game loaded reported; Strict Play's first run started again
// when it wrote its game.cfg and quit), its boot report (the files it did not find) and its launch
// mission's (the mission that did not load, and why), how it ended (its exit code: a crash told
// from a quit), its lease on the build it runs from (run/play_lease.h), which keeps a build from
// pruning that directory while the game may still run, and the run directory it runs in
// (run/run_directory.h, S13 A8: its working directory and its log, the build directory left as the
// build wrote it). What the game's logs say it looked for and did not find are Problems rows on the
// files that name it (session/play_log.h, ADR 0046 DI-27): OpenNova's as its log comes, the game
// install's once its game exited; each mode's kept until the next Play of that mode, or until the
// project closes.
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

	// The Play `request` asks for (a play's; the one an edit_in_viewport's command plans): its mission,
	// behind, fresh and start, and its mode resolved now, the request's play_mode for this Play alone, else
	// the open project's own (LocalSettings::play_mode: the OpenNova runtime for a project never set).
	PlayIntent intent_of(const EditorRequest &request) const;
	// Play refused before any build (no spawn on this platform, a game running, a `mission` that is
	// no .bms of the project, a `start` with no mission to start in, the game install's game running
	// already when Play runs there, Strict Play of an expansion): true, said why.
	bool refused(const PlayIntent &intent);
	// The game started on the last build in the intent's mode (its lease written, the view's run block
	// filled), in `mission` ("" its menu): the project's .bms of that name as the scan spells it, looked up
	// again now the build landed (one gone since starts nothing, play.mission.unknown). The game
	// install starts at its menu, a note saying so. `behind`: its window starts behind every other,
	// never taking the foreground (LaunchPlan::behind; the run section says so). The run directory keeps
	// what the game wrote there in the runs of this mode before (run/run_directory.h: its game.cfg, its
	// saves), Output and the run section's kept naming them; `fresh` empties it first (a first run: the
	// game install's device dialog, a default profile). `start` (Play from here, DI-26): the run
	// directory's copy of the mission has its player's start markers at the point (run/play_start.h), the
	// runtime mounting the run directory, where the build is staged for it; one that cannot be staged
	// starts nothing (play.start).
	void start(const PlayIntent &intent);
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
	// The game spawned on `plan` in its run directory, staged already (its lease written, the run
	// directory claimed, the view's run block filled, its Output line begun); false with `error`.
	bool launch(const LaunchPlan &plan, Diagnostic &error);
	// Whatever the game wrote to its log since the last read, line by line; false when there is no log
	// to read.
	bool tail_game_log();
	// What the game install's file log said its game loaded, read as it exited (`read`: the log was
	// there): the run section's file_log, and a line in Output.
	void report_file_log(bool read);
	// Strict Play's first run, gone: started once more when it wrote its game.cfg and quit
	// (strict_first_run_starts_again), in the same run directory, once the game's one-at-a-time gate is
	// let go (step_start_again, from the polls).
	void start_again_if_first_run();
	void step_start_again();
	// Play from here's start placed in the run directory's copy of `mission` (staged already in
	// `run_dir`); false, reported, when it could not be.
	bool stage_start(const std::string &run_dir, const std::string &expansion, const std::string &mission,
	                 const PlayStart &start);
	// What a line of the game's log reports, each marker's to its absorber (the boot report, the launch
	// mission's): the one place a new report the game logs is read (DI-27's misses).
	void absorb_report(const std::string &line);
	void absorb_boot_report(const std::string &line);
	// A line saying what the runtime looked for and did not find (gameprofile::kResourceMissingMarker):
	// its rows (resource_miss_findings) among this mode's, each once, for the project the game was
	// started in.
	void absorb_resource_miss(const std::string &line);
	// What the game install's logs, read once its game exited, say it did not find (install_log_findings):
	// its file log (`read`: there was one) and the graphics log this run wrote, its rows among this mode's.
	void absorb_install_logs(bool read);
	// This mode's log rows gaining `rows`, each once; true when one was new.
	bool add_log_findings(std::vector<Diagnostic> rows);
	// The game this Play's mode runs, as its rows name it.
	PlayGame play_game() const;
	// The launch mission's report (gameprofile::kLaunchMissionFailedMarker): the mission that did
	// not load and why, a Problems row (play.mission.failed) of the project the game was started in.
	void absorb_mission_report(const std::string &line);
	void absorb_exit();
	// The project's .bms `mission` names, as the scan spells it; "" for none.
	std::string mission_file(const std::string &mission) const;
	// The Play's own findings (a mission that did not load, a nonzero exit) and every mode's log rows
	// given to Problems, and the validation that makes their rows left due.
	void publish_findings();
	// The rows Problems is given: the last Play's own, then each mode's log rows (OpenNova's, the game
	// install's, strict Play's).
	std::vector<Diagnostic> play_rows() const;

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
	// The running (or last) game: its plan, whether it is the game install's (its log read once it
	// exited) and strictly so (its mode's), whether its run directory held a game.cfg before its first start, whether
	// it was started again, when it started, and what its file log said it loaded.
	LaunchPlan plan_;
	bool install_run_ = false;
	bool strict_run_ = false;
	bool had_config_ = false;
	bool started_again_ = false;
	bool start_again_pending_ = false; // waiting for the game before to let its gate go
	int64_t start_again_since_ = 0;
	int64_t started_ms_ = 0;
	FileAccessLog file_log_;
	// The game's one Output line (its log folded under it): its index, what runs ("OpenNova", "the game
	// install"), the lines its log held and how many of them were shown; game_words is its text.
	std::string game_words() const;
	uint64_t game_line_ = 0;
	std::string game_name_;
	size_t game_lines_ = 0, game_shown_ = 0;
	std::string boot_project_; // the project the game was started in: its boot report is that project's
	std::vector<Diagnostic> findings_; // this Play's own rows: its mission's failure, its crash
	// The mode of the running (or last) Play (kRunMode*), and the rows each mode's last Play's logs made
	// (DI-27), kept until the next Play of that mode.
	std::string mode_;
	std::map<std::string, std::vector<Diagnostic>> log_findings_;
	// The graphics log in the run directory as the game was started (its size, -1 for none, and its last
	// write): one the game install's game left as it was is a run before's, never read.
	int64_t graphics_log_size_ = -1;
	int64_t graphics_log_time_ = 0;
};

} // namespace opennova::editor
