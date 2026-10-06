#include <editor/session/play_controller.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <base/gameprofile/resource_missing.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/model/field_text.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/run/run_directory.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/problems_service.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <runtime/mission/mission_sidecars.h>

namespace fs = std::filesystem;

namespace opennova::editor {

std::string play_mission_at(const SessionView &view, const std::string &path) {
	if (!view.project.open || !view.project.scan) return std::string();
	const std::string name = basename_of(path);
	if (!strutil::ends_with_icase(name, ".bms")) return std::string();
	const AssetEntry *entry = view.project.scan->find(name);
	return entry && entry->kind == AssetKind::Mission ? entry->logical_name : std::string();
}

std::string play_mission_for(const SessionView &view) {
	if (!view.project.open || view.documents.active.empty()) return std::string();
	const AssetScan &scan = *view.project.scan;
	const std::string name = basename_of(view.documents.active);
	if (strutil::ends_with_icase(name, ".bms")) return play_mission_at(view, name);
	// A file the game finds by a mission's name: that mission, when the project holds it. The readers
	// cut a mission's name at its first dot ("op.v2.bms" opens "op.wac"), so the mission is any of the
	// project's whose base name the file's is (the first in the scan's order), and a row read only
	// beside another's file (the dialog's sounds, beside its .dbf) only when the project has that one.
	const std::string wanted = normalized_logical_name(name);
	const std::string base = normalized_logical_name(mission::mission_base_name(name));
	const auto names_it = [&](const std::string &mission) {
		for (const mission::Sidecar &sidecar : mission::sidecars()) {
			if (const mission::Sidecar *needed = sidecar.needs ? mission::sidecar_for_role(sidecar.needs) : nullptr)
				if (!scan.find(mission::sidecar_name(mission, *needed))) continue;
			const std::string alternate = mission::sidecar_alternate_name(mission, sidecar);
			if (normalized_logical_name(mission::sidecar_name(mission, sidecar)) == wanted ||
			    (!alternate.empty() && normalized_logical_name(alternate) == wanted))
				return true;
		}
		return false;
	};
	if (!names_it(base + ".bms")) return std::string();
	for (const AssetEntry &entry : scan.entries)
		if (entry.kind == AssetKind::Mission && strutil::ends_with_icase(entry.logical_name, ".bms") &&
		    normalized_logical_name(mission::mission_base_name(entry.logical_name)) == base && names_it(entry.logical_name))
			return entry.logical_name;
	return std::string();
}

PlayController::PlayController(SessionCore &core) : core_(core), view_(core.view()), play_(core.platform()) {}

PlayController::~PlayController() {
	// A game left running outlives the editor on purpose (a modder may keep playing);
	// the handle is released, never the process.
	if (play_.state() != PlayState::Stopped) core_.platform().release(play_.pid());
}

void PlayController::set_launcher_source(PlayLauncherSource source) {
	source_ = std::move(source);
	launcher_ = source_ ? source_(false) : PlayLauncher();
	view_.activity.source_run = launcher_.source_run;
	view_.activity.runtime_executable = resolve_runtime_executable();
	core_.touch(ViewConcern::Preferences);
}

// The launcher the view shows, the port a spawn asked for left out, and the runtime it resolves to:
// Preferences moves when either changed.
void PlayController::follow_launcher(const PlayLauncher &launcher) {
	launcher_ = launcher;
	launcher_.mcp_port = 0;
	const std::string runtime = resolve_runtime_executable();
	if (view_.activity.source_run == launcher_.source_run &&
			view_.activity.runtime_executable == runtime)
		return;
	view_.activity.source_run = launcher_.source_run;
	view_.activity.runtime_executable = runtime;
	core_.touch(ViewConcern::Preferences);
}

std::string PlayController::resolve_runtime_executable() const {
	if (launcher_.source_run) return launcher_.executable;
	if (!core_.local().runtime_executable.empty()) return core_.local().runtime_executable;
	const Preferences &settings = core_.preferences().values();
	if (!settings.runtime_executable.empty()) return settings.runtime_executable;
	return launcher_.executable;
}

std::string PlayController::mission_file(const std::string &mission) const {
	const AssetEntry *entry = core_.project_file(mission);
	return entry && entry->kind == AssetKind::Mission && strutil::ends_with_icase(entry->logical_name, ".bms")
	               ? entry->logical_name
	               : std::string();
}

// Play refused before any build: where nothing can be spawned, while a game runs (a second
// one would fight it for its files), and for a mission the project does not hold (the game
// would start, find no such mission in what it mounts and fall back to its menu). True when
// refused, said why.
bool PlayController::refused(const std::string &mission, const PlayStart &start) {
	if (!core_.platform().can_spawn()) {
		// The platform says why (an OS the editor cannot spawn on yet, a session with no process seam).
		core_.report(make_finding(CoreFinding::PlayUnsupported, DiagnosticSeverity::Error,
		                          core_.platform().no_spawn_reason() + " Build works here."));
		view_.activity.status = "Play is not available here: see Problems.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	if (play_.state() != PlayState::Stopped || start_again_pending_) {
		core_.report(make_finding(CoreFinding::PlayAlreadyRunning, DiagnosticSeverity::Error,
		                          "The game is already running; stop it before starting it again."));
		return true;
	}
	// Play from here starts the player in a mission: a start with none has no mission to place it in.
	if (start.set && mission.empty()) {
		core_.report(make_finding(CoreFinding::PlayStart, DiagnosticSeverity::Error,
		                          "Play from here starts the player in a mission: name the mission (a .bms of the "
		                          "project) the start is a point of."));
		view_.activity.status = "Play from here needs a mission.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	if (!mission.empty() && mission_file(mission).empty()) {
		core_.report(make_finding(CoreFinding::PlayMissionUnknown, DiagnosticSeverity::Error,
		                          mission + " is no mission of the project: Play starts the game in a .bms the project "
		                                    "holds. Import it, or make it, first."));
		view_.activity.status = "Play mission needs a mission of the project.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	// An expansion plays over the game install, with /exp (ADR 0046 S16): none set, nothing is built.
	if (view_.project.open && !view_.project.document->expansion.standalone() && core_.game_install().empty()) {
		core_.report(make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                          "The project builds as an expansion, which plays over the game install: choose its "
		                          "folder in File > Project settings... first."));
		view_.activity.status = "Play needs the game install; see Problems.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	const Preferences &settings = core_.preferences().values();
	if (!settings.play_in_install) return false;
	// Strict Play stages the build alone, and an expansion's build plays over its base game, which the
	// project cannot name yet: refused rather than played over the install's own archives.
	if (settings.play_in_install_strict && view_.project.open && !view_.project.document->expansion.standalone()) {
		core_.report(strict_expansion_refusal(view_.project.document->expansion.name));
		view_.activity.status = "Strict Play of an expansion is not supported yet; see Problems.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	// The game install's game runs one at a time: one started while another runs (a player's, one an
	// earlier editor left) quits at once (kInstallInstanceSemaphore).
	if (core_.platform().semaphore_held(kInstallInstanceSemaphore) == ProcessLiveness::Alive) {
		core_.report(make_finding(CoreFinding::PlayInstallRunning, DiagnosticSeverity::Error,
		                          "The game install's game is already running on this machine. It runs one at a "
		                          "time: a second one quits at once, before it loads its data. Close it, then Play."));
		view_.activity.status = "The game install's game is already running; see Problems.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	return false;
}

std::vector<Diagnostic> PlayController::play_rows() const {
	std::vector<Diagnostic> rows = findings_;
	for (const char *mode : {kRunModeRuntime, kRunModeInstall, kRunModeStrict})
		if (const auto found = log_findings_.find(mode); found != log_findings_.end())
			rows.insert(rows.end(), found->second.begin(), found->second.end());
	return rows;
}

void PlayController::publish_findings() {
	core_.problems().set_play_findings(play_rows());
	core_.problems().validate_later();
}

PlayGame PlayController::play_game() const {
	if (mode_ == kRunModeStrict) return {"the game install (strict Play)"};
	if (mode_ == kRunModeInstall) return {"the game install"};
	return {"OpenNova"};
}

void PlayController::start(const std::string &mission, bool behind, bool fresh, const PlayStart &start) {
	const std::string &build_dir = view_.activity.last_build->build_dir;
	LaunchPlan plan;
	Diagnostic error;
	std::error_code ec;
	// The mode this Play runs in (its run directory's, run/run_directory.h): the game install's lenient or
	// strict, else OpenNova.
	const bool in_install = core_.preferences().values().play_in_install;
	// Strict Play (Preferences::play_in_install_strict): the build and the install's program alone, no /d.
	const bool strict = in_install && core_.preferences().values().play_in_install_strict;
	const std::string mode = !in_install ? kRunModeRuntime : strict ? kRunModeStrict : kRunModeInstall;
	// The last run's boot report, its mission's and its exit go, their rows with them (the next
	// validation would make none), and so do the rows the logs of this mode's last Play made (DI-27: each
	// mode's stay until the next Play of that mode); the game started now reports on this project.
	view_.activity.boot_missing.clear();
	view_.activity.play_mission.clear();
	view_.activity.play_start = PlayStart();
	view_.activity.play_start_placed = PlayStartPlaced();
	start_again_pending_ = false;
	findings_.clear();
	std::vector<Diagnostic> dropped;
	if (const auto found = log_findings_.find(mode); found != log_findings_.end()) {
		dropped = std::move(found->second);
		log_findings_.erase(found);
	}
	mode_ = mode;
	core_.problems().set_play_findings(play_rows());
	const size_t rows = view_.findings.diagnostics.size();
	view_.findings.diagnostics.erase(
			std::remove_if(view_.findings.diagnostics.begin(), view_.findings.diagnostics.end(),
					[&dropped](const Diagnostic &d) {
						return d.row() == &finding_code(CoreFinding::PlayBootMissing) ||
						       d.row() == &finding_code(CoreFinding::PlayMissionFailed) ||
						       d.row() == &finding_code(CoreFinding::PlayCrashed) ||
						       std::find(dropped.begin(), dropped.end(), d) != dropped.end();
					}),
			view_.findings.diagnostics.end());
	core_.touch(ViewConcern::Run);
	if (view_.findings.diagnostics.size() != rows) {
		core_.problems().mark_rows();
		core_.touch(ViewConcern::Findings);
	}
	boot_project_ = view_.project.root;
	// The mission as the project spells its file now (the build read the project again): one gone
	// since Play was asked for starts nothing.
	const std::string in_mission = mission.empty() ? std::string() : mission_file(mission);
	if (!mission.empty() && in_mission.empty()) {
		core_.report(make_finding(CoreFinding::PlayMissionUnknown, DiagnosticSeverity::Error,
		                          mission + " is no longer a mission of the project: the game was not started."));
		view_.activity.status = "Play mission needs a mission of the project.";
		core_.touch(ViewConcern::Output);
		return;
	}
	// What Play launches, asked of its source now that the build has landed: one answer, which the
	// plan takes whole (the executable, whether the run drives the source checkout, the Godot
	// options) with the port of the game's MCP endpoint allocated now (none for the game install,
	// which has no endpoint); the view's runtime follows it.
	const PlayLauncher launcher = source_ ? source_(!in_install) : launcher_;
	follow_launcher(launcher);
	const std::string executable = in_install ? std::string() : resolve_runtime_executable();
	if (!in_install && (executable.empty() || !fs::is_regular_file(system_path(executable), ec))) {
		core_.report(make_finding(CoreFinding::PlayRuntimeMissing, DiagnosticSeverity::Error,
		                          executable.empty()
		                                  ? "No game runtime is set; choose opennova.exe in File > Project settings..."
		                                  : "The game runtime was not found: " + executable));
		view_.activity.status = "The game runtime was not found.";
		core_.touch(ViewConcern::Output);
		return;
	}
	// The run directory the game runs in (run/run_directory.h, S13 A8): its working directory and its
	// log, never the build directory it runs from, which stays as the build wrote it. Each mode takes its
	// own (`.opennova/run/<mode>/<n>`), so a Play of another mode between two never touches it. It keeps
	// what the game wrote there in the runs of this mode before (its game.cfg, which spares it the device
	// dialog, its saves), what the Play before staged and the logs gone; a fresh Play empties it. One whose
	// game may still run (one an earlier editor left running) is passed over. It lives under the cache,
	// which comes with its self-ignore file.
	std::string run_dir, run_error, cache_error;
	std::vector<std::string> kept;
	ensure_project_cache_dir(core_.paths(), cache_error);
	const LeaseLiveness liveness = [this](int64_t pid, const std::string &created) {
		return core_.platform().process_liveness(pid, created);
	};
	if (!take_run_directory(core_.paths().run_dir, liveness, RunTake{mode, fresh}, run_dir, kept, run_error)) {
		core_.report(make_finding(CoreFinding::PlayRunDirectory, DiagnosticSeverity::Error,
		                          "The game's run directory could not be made: " + run_error));
		view_.activity.status = "The game's run directory could not be made; see Problems.";
		core_.touch(ViewConcern::Output);
		return;
	}
	view_.activity.play_fresh = fresh;
	view_.activity.play_kept = kept;
	if (fresh)
		core_.note("A fresh run: the run directory was emptied of what the runs before wrote there.");
	else if (!kept.empty())
		core_.note_folded("The run directory kept " + counted(kept.size(), "file") +
		                          " the runs before wrote there (Play fresh starts without them); open this line for "
		                          "the names.",
		                  kept);
	// An expansion's build plays over the game install, with /exp (ADR 0046 S16): its run directory
	// holds the install's base game and the build's expansion folder (prepare_expansion_run), for the
	// stock game and the runtime alike.
	const std::string &expansion = view_.activity.last_build->expansion;
	const std::string &copy_cache = core_.paths().install_copy_dir;
	// What the staging put in the run directory recorded there, staged whole or not, so the next Play
	// removes those files alone (and empties the directory when the record cannot be written).
	const auto record_staging = [&](const std::vector<std::string> &staged) {
		std::string record_error;
		if (!record_run_staging(run_dir, RunStaging{mode, staged}, record_error))
			core_.note("The run directory's staging record could not be written (" + record_error +
			           "): the next Play starts from an empty run directory.");
	};
	if (in_install) {
		const bool staged =
		        strict ? prepare_strict_install_launch_plan(core_.game_install(), build_dir, run_dir, expansion, plan, error)
		               : prepare_retail_launch_plan(core_.game_install(), build_dir, run_dir, plan, error, expansion,
		                                            copy_cache);
		record_staging(plan.staged);
		if (!staged) {
			core_.report(error);
			view_.activity.status = "The game install could not be prepared; see Problems.";
			core_.touch(ViewConcern::Output);
			return;
		}
		// The stock game takes no mission on its command line: it starts at its menu, where the
		// build's mission is listed.
		if (!in_mission.empty())
			core_.note("The game install starts at its menu: choose " + in_mission + " there" +
			           (start.set ? std::string(": its player starts at ") + play_start_words(start) + "." : std::string(".")));
	} else {
		// Play from here's runtime mounts the run directory, where the build is staged for it (an expansion's
		// is there already), so the run directory's copy of the mission is the one it loads.
		const bool on_run_dir = start.set && expansion.empty();
		std::vector<std::string> staged;
		const bool prepared = expansion.empty()
		        ? !on_run_dir || prepare_runtime_run(build_dir, run_dir, error, &staged)
		        : prepare_expansion_run(core_.game_install(), build_dir, expansion, run_dir, copy_cache, error, link_file,
		                                &staged);
		record_staging(staged);
		if (!prepared) {
			core_.report(error);
			view_.activity.status = "The expansion's run directory could not be prepared; see Problems.";
			core_.touch(ViewConcern::Output);
			return;
		}
		plan = launcher.source_run
				? make_source_launch_plan(executable, launcher.godot_project_dir, build_dir, run_dir,
						  view_.project.document->target_game, launcher.mcp_port, in_mission,
						  launcher.engine_args, expansion, on_run_dir)
				: make_play_launch_plan(executable, build_dir, run_dir, view_.project.document->target_game,
						  launcher.mcp_port, in_mission, launcher.engine_args, expansion, on_run_dir);
		plan.staged = std::move(staged);
	}
	// Play from here: the start placed in the staged mission, the game started on nothing else.
	if (start.set && !stage_start(run_dir, expansion, in_mission, start)) return;
	// Behind the others when the request asked (the MCP gaps lane): how the platform starts it.
	plan.behind = behind;
	install_run_ = in_install;
	strict_run_ = strict;
	// Strict Play's first run: whether the run directory (the build's files and the install's program in
	// it, and what the runs before wrote there) holds a game.cfg before the game starts, the build's own or
	// one a run before wrote.
	had_config_ = strict && fs::is_regular_file(system_path(join_path(plan.working_dir, "game.cfg")), ec);
	started_again_ = false;
	view_.activity.play_strict = strict;
	view_.activity.play_started_again = false;
	view_.activity.play_mission = in_install ? std::string() : in_mission;
	if (!launch(plan, error)) {
		view_.activity.play_mission.clear();
		core_.report(error);
		view_.activity.status = "The game could not be started.";
		core_.touch(ViewConcern::Output);
	}
}

bool PlayController::launch(const LaunchPlan &plan, Diagnostic &error) {
	// The run directory holds no log (its take removed the logs a run before left, whatever else it kept),
	// or it is the first run's whose log was read as it exited (the game deletes the log a run before left
	// as it writes its first line): the tail starts clean.
	game_log_file_ = plan.log_file;
	game_log_offset_ = 0;
	game_log_partial_.clear();
	file_log_ = FileAccessLog();
	view_.activity.play_file_log_read = false;
	view_.activity.play_file_log = FileAccessLog();
	// The graphics log a run before left in the run directory (the game install's: made anew by the first
	// line a run writes), as it stands: one the game leaves as it was is not this run's (DI-27).
	graphics_log_size_ = -1;
	graphics_log_time_ = 0;
	{
		std::error_code ec;
		const fs::path graphics = system_path(join_path(plan.working_dir, kInstallGraphicsLogName));
		if (fs::is_regular_file(graphics, ec)) {
			graphics_log_size_ = static_cast<int64_t>(fs::file_size(graphics, ec));
			graphics_log_time_ = static_cast<int64_t>(fs::last_write_time(graphics, ec).time_since_epoch().count());
		}
	}
	if (!play_.start(plan, error)) return false;
	plan_ = plan;
	started_ms_ = core_.platform().now_ms();
	// The game's lease on the directory it runs from: a build leaves it alone while the game
	// runs, this editor's and one started after the editor restarts (run/play_lease.h).
	// The game as the OS knows it (the image it runs, when it was created): what tells it from
	// another process that later takes its pid.
	std::string lease_error;
	ProcessIdentity identity;
	core_.platform().process_identity(play_.pid(), identity);
	PlayLease lease{plan.build_dir, play_.pid(), identity.image, identity.created};
	if (write_play_lease(lease, lease_error))
		play_lease_ = std::move(lease);
	else
		core_.note("The game's lease could not be written (" + lease_error +
		           "): a build after the editor restarts may remove its files while it runs.");
	// The run directory records its game the same way, so a later Play passes it over while the game
	// may run, after the editor restarts too.
	std::string claim_error;
	if (claim_run_directory(plan.working_dir, play_.pid(), identity, claim_error))
		run_dir_ = plan.working_dir;
	else
		core_.note("The game's run directory could not record it (" + claim_error +
		           "): a later Play may take that directory while the game runs.");
	view_.activity.play_run_dir = plan.working_dir;
	view_.activity.play_log_file = plan.log_file;
	view_.activity.play_state = play_.state();
	view_.activity.play_pid = play_.pid();
	view_.activity.play_mcp_port = plan.mcp_port;
	view_.activity.play_behind = plan.behind;
	view_.activity.play_command_line = launch_plan_command_line(plan);
	view_.activity.play_exited_on_its_own = false;
	view_.activity.play_exit_code = -1;
	// One line for the game, its command line and its whole log folded under it; what matters of the log
	// (an error, a warning, a file it lacks) shown below it as it comes (the UX round's problems lane). The
	// game install's log is folded under it once the game has exited.
	game_name_ = !install_run_ ? "OpenNova" : strict_run_ ? "the game install (strict)" : "the game install";
	game_lines_ = 0;
	game_shown_ = 0;
	game_line_ = core_.note_folded(game_words(), {"Command line: " + view_.activity.play_command_line});
	view_.activity.status = install_run_                        ? "Game install running."
	                        : view_.activity.play_mission.empty() ? "Game running."
	                                                              : "Game running: " + view_.activity.play_mission + ".";
	core_.touch(ViewConcern::Run);
	return true;
}

void PlayController::stop() {
	// A start again waiting for the game before to let its gate go is not made.
	if (start_again_pending_) {
		start_again_pending_ = false;
		view_.activity.status = "The game was not started again.";
		core_.note("Stop requested: the game is not started again.");
		core_.touch(ViewConcern::Run);
	}
	if (play_.state() != PlayState::Running) return;
	play_.stop();
	view_.activity.play_state = play_.state();
	view_.activity.status = "Stopping the game...";
	core_.note("Stop requested.");
	core_.touch(ViewConcern::Run);
}

void PlayController::poll() {
	const PlayState before = play_.state();
	const PlayState now = play_.poll();
	// The game install's log is read once its game has exited, never while it runs: the game appends to it
	// through an exclusive open and makes it anew, empty, when that open fails, so a read while it runs cuts
	// it (kInstallFileLogName, run/launch_plan.h). OpenNova's own log is tailed as it comes.
	if (now != PlayState::Stopped && !install_run_) tail_game_log();
	if (before != now) {
		if (now == PlayState::Stopped) {
			const bool log_read = tail_game_log();
			absorb_exit();
			if (install_run_) {
				report_file_log(log_read);
				absorb_install_logs(log_read);
			}
			if (strict_run_) start_again_if_first_run();
		}
		view_.activity.play_state = play_.state();
		view_.activity.play_pid = play_.pid();
		if (play_.state() == PlayState::Stopped) view_.activity.play_mcp_port = 0;
		core_.touch(ViewConcern::Run);
	}
	if (start_again_pending_ && play_.state() == PlayState::Stopped) step_start_again();
}

// What the game install's file log says its game loaded, as it exited: the run section's file_log and a
// line in Output, every name folded under it. A file the game did not find is never in the log (only an
// open that succeeded is logged), so nothing is said of what the game lacked.
void PlayController::report_file_log(bool read) {
	view_.activity.play_file_log_read = read;
	view_.activity.play_file_log = file_log_;
	core_.touch(ViewConcern::Run);
	if (!read) {
		std::string line = std::string("The game left no file log (") + kInstallFileLogName +
		                   "): it opened no archive and no file.";
		// A game that quit at once on its own: the game runs one at a time (kInstallInstanceSemaphore).
		if (view_.activity.play_exited_on_its_own && view_.activity.play_exit_code == 0)
			line += " The game runs one at a time: one started while another runs quits at once, before it loads its data.";
		core_.note(line);
		return;
	}
	const FileAccessLog &log = file_log_;
	std::vector<std::string> folded;
	for (const std::string &name : log.archives) folded.push_back("archive: " + name);
	for (const std::string &name : log.from_archives) folded.push_back("from the archives: " + name);
	for (const std::string &name : log.from_disk) folded.push_back("from disk: " + name);
	core_.note_folded("What the game loaded, from its file log: " + counted(log.archives.size(), "archive") + ", " +
	                          counted(log.from_archives.size(), "file") + " from the archives, " +
	                          (log.from_disk.empty() ? std::string("none") : counted(log.from_disk.size(), "file")) +
	                          " from disk (a file the game did not find is never logged); open this line for the names.",
	                  std::move(folded));
}

// Play from here (DI-26): neither game takes a place on its command line, so the start is a start marker of
// the mission, in the run directory's copy alone (run/play_start.h); the game places its player there by its
// own spawn selection. Said in Output and the run section; one that cannot be placed is reported and the
// game is not started.
bool PlayController::stage_start(const std::string &run_dir, const std::string &expansion, const std::string &mission,
                                 const PlayStart &start) {
	PlayStartPlaced placed;
	Diagnostic error;
	if (!stage_play_start(run_dir, expansion, mission, start, placed, error)) {
		core_.report(error);
		view_.activity.status = "Play from here could not place the start; see Problems.";
		core_.touch(ViewConcern::Output);
		return false;
	}
	view_.activity.play_start = start;
	view_.activity.play_start.set = true;
	view_.activity.play_start_placed = placed;
	const std::string markers = placed.added ? std::string("a start marker added (type ") + std::to_string(placed.type) + ")"
	                                         : counted(placed.moved, "start marker") + " of type " +
	                                                   std::to_string(placed.type) + " moved";
	core_.note("Play from here: the player starts at " + play_start_words(start) + ", " + markers + " in " + mission +
	           " of the run directory's " + placed.archive + "; the project's file and the build are as they were.");
	core_.touch(ViewConcern::Run);
	return true;
}

// Strict Play's first run, gone: with no game.cfg beside it, the game may write its own and quit before a
// player sees its menu. Started once more, in the same run directory (the game.cfg it wrote kept), when
// it exited on its own with code 0 soon after it started and wrote that game.cfg
// (strict_first_run_starts_again); never for a game of a project closed since.
void PlayController::start_again_if_first_run() {
	std::error_code ec;
	const bool has_config = fs::is_regular_file(system_path(join_path(plan_.working_dir, "game.cfg")), ec);
	const int64_t ran_ms = core_.platform().now_ms() - started_ms_;
	if (!strict_first_run_starts_again(had_config_, has_config, view_.activity.play_exited_on_its_own,
	                                   view_.activity.play_exit_code, ran_ms, started_again_))
		return;
	if (!view_.project.open || view_.project.root != boot_project_) return;
	started_again_ = true;
	view_.activity.play_started_again = true;
	core_.note("First run: the game wrote its game.cfg and quit; started again.");
	start_again_pending_ = true;
	start_again_since_ = core_.platform().now_ms();
	step_start_again();
}

// The start again, once the game before has let the game's one-at-a-time gate go: a game started while
// the gate is held quits at once (kInstallInstanceSemaphore). Waited for on the polls, up to the stop
// deadline; still held then (another game runs), said and not made.
void PlayController::step_start_again() {
	if (core_.platform().semaphore_held(kInstallInstanceSemaphore) == ProcessLiveness::Alive) {
		if (core_.platform().now_ms() - start_again_since_ < kPlayStopDeadlineMs) return;
		start_again_pending_ = false;
		core_.report(make_finding(CoreFinding::PlayInstallRunning, DiagnosticSeverity::Error,
		                          "The game was not started again: another game of the install is running on this "
		                          "machine, and the game runs one at a time. Close it, then Play."));
		view_.activity.status = "The game was not started again; see Problems.";
		core_.touch(ViewConcern::Output);
		return;
	}
	start_again_pending_ = false;
	Diagnostic error;
	const LaunchPlan first = plan_; // the first run's plan, in the same run directory
	if (!launch(first, error)) {
		core_.report(error);
		view_.activity.status = "The game could not be started again.";
		core_.touch(ViewConcern::Output);
	}
}

void PlayController::forget_project() {
	start_again_pending_ = false;
	view_.activity.boot_missing.clear();
	boot_project_.clear();
	findings_.clear();
	log_findings_.clear();
	core_.problems().set_play_findings({});
}

ProtectedDirs PlayController::protected_dirs(const std::string &output_root) {
	return [this, output_root] {
		const int64_t own = play_.state() != PlayState::Stopped ? play_.pid() : -1;
		std::vector<std::string> dirs =
		        leased_build_dirs(output_root, [this, own](int64_t pid, const std::string &created) {
			        return pid == own ? ProcessLiveness::Alive : core_.platform().process_liveness(pid, created);
		        });
		if (!play_.running_build_dir().empty()) dirs.push_back(play_.running_build_dir());
		return dirs;
	};
}

// Append whatever the game wrote to its log since the last poll, line by line (the game install's, once
// its game has exited: what it says the game loaded kept too).
bool PlayController::tail_game_log() {
	if (game_log_file_.empty()) return false;
	std::ifstream in(system_path(game_log_file_), std::ios::binary);
	if (!in) return false;
	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	if (size < 0 || static_cast<uint64_t>(size) <= game_log_offset_) return true;
	in.seekg(static_cast<std::streamoff>(game_log_offset_));
	std::string chunk(static_cast<size_t>(static_cast<uint64_t>(size) - game_log_offset_), '\0');
	in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
	game_log_offset_ = static_cast<uint64_t>(size);
	game_log_partial_ += chunk;
	size_t start = 0;
	std::vector<std::string> folded;
	for (;;) {
		const size_t nl = game_log_partial_.find('\n', start);
		if (nl == std::string::npos) break;
		std::string line = game_log_partial_.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		// Every line folded under the game's line; one that matters shown as well.
		++game_lines_;
		if (game_line_matters(line)) {
			++game_shown_;
			core_.note("game: " + line);
		}
		if (install_run_) add_file_access_line(file_log_, line);
		folded.push_back(line);
		absorb_report(line);
		start = nl + 1;
	}
	game_log_partial_.erase(0, start);
	// The game's line says how much its log holds; one Output no longer holds starts again below.
	if (!folded.empty() && !core_.fold_into_note(game_line_, game_words(), folded))
		game_line_ = core_.note_folded(game_words(), std::move(folded));
	// The names the lines reported become their rows in the validation they left due
	// (absorb_boot_report), which the polls step (S13 A3).
	return true;
}

// "Running: OpenNova on the build. Its log: 412 lines, 3 shown below; open this line for all of it."
std::string PlayController::game_words() const {
	std::string out = "Running: " + game_name_ + " on the build.";
	if (game_lines_ == 0) return out + " Its log follows, folded under this line.";
	return out + " Its log: " + counted(game_lines_, "line") + ", " + std::to_string(game_shown_) +
	       " shown below; open this line for all of it.";
}

void PlayController::absorb_report(const std::string &line) {
	absorb_boot_report(line);
	absorb_mission_report(line);
	absorb_resource_miss(line);
}

bool PlayController::add_log_findings(std::vector<Diagnostic> rows) {
	std::vector<Diagnostic> &kept = log_findings_[mode_];
	bool added = false;
	for (Diagnostic &d : rows)
		if (std::find(kept.begin(), kept.end(), d) == kept.end()) {
			kept.push_back(std::move(d));
			added = true;
		}
	return added;
}

// The runtime names each file or name it looked up as it loaded and did not find, once, on a line of its
// log (`ResourceRoot: <kResourceMissingMarker><kind> "<name>"[ named by "<file>"][: <words>]`,
// ResourceRoot::report_missing): rows on the files of the project that name it (resource_miss_findings),
// among this mode's until its next Play. Like the boot report it belongs to the project the game was
// started in: a line read after that project closed, or while another is open, is ignored.
void PlayController::absorb_resource_miss(const std::string &line) {
	gameprofile::ResourceMiss miss;
	if (!gameprofile::parse_resource_missing(line, miss)) return;
	if (!view_.project.open || view_.project.root != boot_project_) return;
	if (!add_log_findings(resource_miss_findings(miss, play_game(), view_))) return;
	core_.touch(ViewConcern::Run);
	publish_findings();
}

// The game install's game names nothing it did not find: its file log names each open that succeeded, its
// graphics log the mission it began loading and whether it finished. Read once it exited (the file log
// never while it runs, kInstallFileLogName), they make rows of what they show it lacked
// (install_log_findings) among this mode's until its next Play; never for a project closed since.
void PlayController::absorb_install_logs(bool read) {
	if (!view_.project.open || view_.project.root != boot_project_) return;
	InstallLogs logs;
	logs.file_log = read ? &file_log_ : nullptr;
	logs.exited_on_its_own = view_.activity.play_exited_on_its_own;
	// The graphics log this run wrote: one there as the game was started, as it was, is a run before's.
	std::error_code ec;
	const fs::path graphics = system_path(join_path(plan_.working_dir, kInstallGraphicsLogName));
	if (fs::is_regular_file(graphics, ec)) {
		const int64_t size = static_cast<int64_t>(fs::file_size(graphics, ec));
		const int64_t time = static_cast<int64_t>(fs::last_write_time(graphics, ec).time_since_epoch().count());
		if (size != graphics_log_size_ || time != graphics_log_time_) {
			std::ifstream in(graphics, std::ios::binary);
			logs.graphics_log.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
	}
	if (!add_log_findings(install_log_findings(logs, play_game(), view_))) return;
	core_.touch(ViewConcern::Run);
	publish_findings();
}

// The runtime names each boot-required file it could not find, one line per file
// (`BootRootMount: <kBootResourceMissingMarker><name> ...`, godot/game/boot_root_mount.gd
// over the witnessed manifest). Each name is recorded once, and every
// validation makes its Problems row (ProblemsService) until Play starts again or the
// project closes. The report belongs to the project the game was started in: a line read
// after that project closed, or while another is open, is ignored.
void PlayController::absorb_boot_report(const std::string &line) {
	const std::string marker = gameprofile::kBootResourceMissingMarker;
	const size_t at = line.find(marker);
	if (at == std::string::npos) return;
	const size_t start = at + marker.size();
	size_t end = start;
	while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
	const std::string name = line.substr(start, end - start);
	if (name.empty()) return;
	if (!view_.project.open || view_.project.root != boot_project_) return;
	if (view_.activity.missing_at_boot(name)) return;
	view_.activity.boot_missing.push_back(name);
	core_.touch(ViewConcern::Run);
	core_.problems().validate_later();
}

// The runtime says when the mission it was launched in did not load, on one line
// (`MainGame: <kLaunchMissionFailedMarker><mission> <reason>`, godot/game/main_game.gd): a Problems
// row (play.mission.failed) on the mission's file, once per Play, kept by every validation until
// Play starts again or the project closes. Like the boot report it belongs to the project the game
// was started in: a line read after that project closed, or while another is open, is ignored.
void PlayController::absorb_mission_report(const std::string &line) {
	const std::string marker = gameprofile::kLaunchMissionFailedMarker;
	const size_t at = line.find(marker);
	if (at == std::string::npos) return;
	const size_t start = at + marker.size();
	// The mission's name, which may hold a space ("my map.bms"): the one Play started the game in,
	// where the line names it (case aside), else the text to the first space.
	const std::string &launched = view_.activity.play_mission;
	size_t end = start;
	if (!launched.empty() && line.size() >= start + launched.size() &&
	    strutil::iequals(line.substr(start, launched.size()), launched) &&
	    (line.size() == start + launched.size() || std::isspace(static_cast<unsigned char>(line[start + launched.size()]))))
		end = start + launched.size();
	else
		while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
	const std::string name = line.substr(start, end - start);
	if (name.empty()) return;
	if (!view_.project.open || view_.project.root != boot_project_) return;
	for (const Diagnostic &d : findings_)
		if (d.row() == &finding_code(CoreFinding::PlayMissionFailed)) return;
	while (end < line.size() && std::isspace(static_cast<unsigned char>(line[end]))) ++end;
	std::string reason = line.substr(end);
	while (!reason.empty() && (reason.back() == '.' || std::isspace(static_cast<unsigned char>(reason.back())))) reason.pop_back();
	const AssetEntry *entry = core_.project_file(name);
	findings_.push_back(make_finding(CoreFinding::PlayMissionFailed, DiagnosticSeverity::Error,
	                                 "The game could not load " + name + (reason.empty() ? std::string() : ": " + reason) +
	                                         ". It went back to its menu; its log is in Output.",
	                                 entry ? entry->relative_path : std::string()));
	core_.touch(ViewConcern::Run);
	publish_findings();
}

// How the game ended, on the poll that saw it end: stopped, quit (exit code 0, or one the
// platform could not read), or any other code: a crash or an error exit, said in Output with
// its code and a Problems row (play.crashed) that stays, like the boot report, until Play
// starts again or the project closes (a game of a project closed since reports nothing).
void PlayController::absorb_exit() {
	// The game is gone: its lease goes, its own and no other (another game may run from the same
	// build), and its directory is a build like any.
	if (play_lease_.pid >= 0) remove_play_lease(play_lease_.build_dir, play_lease_.pid);
	play_lease_ = PlayLease();
	release_run_directory(run_dir_); // its log stays until a Play takes the directory again
	run_dir_.clear();
	view_.activity.play_exited_on_its_own = play_.exited_on_its_own();
	view_.activity.play_exit_code = play_.exit_code();
	std::string line =
			view_.activity.play_exited_on_its_own ? "The game exited." : "The game was stopped.";
	if (view_.activity.play_exited_on_its_own && view_.activity.play_exit_code > 0) {
		std::string code = std::to_string(view_.activity.play_exit_code);
		if (view_.activity.play_exit_code > 0xFFFF) {
			char hex[16];
			std::snprintf(hex, sizeof(hex), "0x%08llX", static_cast<unsigned long long>(view_.activity.play_exit_code));
			code += std::string(" (") + hex + ")";
		}
		line = "The game exited with code " + code + ".";
		if (view_.project.open && view_.project.root == boot_project_) {
			findings_.push_back(make_finding(CoreFinding::PlayCrashed, DiagnosticSeverity::Error,
			                                 "The game ended with exit code " + code +
			                                         ": it crashed or stopped on an error. Its log is in Output."));
			publish_findings();
		}
	}
	core_.note(line);
	view_.activity.status = line;
}

} // namespace opennova::editor
