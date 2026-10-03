#include <editor/session/play_controller.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
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

std::string play_mission_for(const SessionView &view) {
	if (!view.project.open || view.documents.active.empty()) return std::string();
	const AssetScan &scan = *view.project.scan;
	const auto mission_named = [&scan](const std::string &file) {
		const AssetEntry *entry = scan.find(file);
		return entry && entry->kind == AssetKind::Mission ? entry->logical_name : std::string();
	};
	const std::string name = basename_of(view.documents.active);
	if (strutil::ends_with_icase(name, ".bms")) return mission_named(name);
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
bool PlayController::refused(const std::string &mission) {
	if (!core_.platform().can_spawn()) {
		// The platform says why (an OS the editor cannot spawn on yet, a session with no process seam).
		core_.report(make_finding(CoreFinding::PlayUnsupported, DiagnosticSeverity::Error,
		                          core_.platform().no_spawn_reason() + " Build works here."));
		view_.activity.status = "Play is not available here: see Problems.";
		core_.touch(ViewConcern::Output);
		return true;
	}
	if (play_.state() != PlayState::Stopped) {
		core_.report(make_finding(CoreFinding::PlayAlreadyRunning, DiagnosticSeverity::Error,
		                          "The game is already running; stop it before starting it again."));
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
	return false;
}

void PlayController::publish_findings() {
	core_.problems().set_play_findings(findings_);
	core_.problems().validate_later();
}

void PlayController::start(const std::string &mission) {
	const std::string &build_dir = view_.activity.last_build->build_dir;
	LaunchPlan plan;
	Diagnostic error;
	std::error_code ec;
	// The last run's boot report, its mission's and its exit go, their rows with them (the next
	// validation would make none); the game started now reports on this project.
	view_.activity.boot_missing.clear();
	view_.activity.play_mission.clear();
	findings_.clear();
	core_.problems().set_play_findings({});
	const size_t rows = view_.findings.diagnostics.size();
	view_.findings.diagnostics.erase(
			std::remove_if(view_.findings.diagnostics.begin(), view_.findings.diagnostics.end(),
					[](const Diagnostic &d) {
						return d.row() == &finding_code(CoreFinding::PlayBootMissing) ||
						       d.row() == &finding_code(CoreFinding::PlayMissionFailed) ||
						       d.row() == &finding_code(CoreFinding::PlayCrashed);
					}),
			view_.findings.diagnostics.end());
	core_.touch(ViewConcern::Run);
	if (view_.findings.diagnostics.size() != rows) core_.touch(ViewConcern::Findings);
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
	const bool in_install = core_.preferences().values().play_in_install;
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
	// log, emptied, never the build directory it runs from, which stays as the build wrote it. One
	// whose game may still run (one an earlier editor left running) is passed over. It lives under
	// the cache, which comes with its self-ignore file.
	std::string run_dir, run_error, cache_error;
	ensure_project_cache_dir(core_.paths(), cache_error);
	const LeaseLiveness liveness = [this](int64_t pid, const std::string &created) {
		return core_.platform().process_liveness(pid, created);
	};
	if (!take_run_directory(core_.paths().run_dir, liveness, run_dir, run_error)) {
		core_.report(make_finding(CoreFinding::PlayRunDirectory, DiagnosticSeverity::Error,
		                          "The game's run directory could not be made: " + run_error));
		view_.activity.status = "The game's run directory could not be made; see Problems.";
		core_.touch(ViewConcern::Output);
		return;
	}
	if (in_install) {
		if (!prepare_retail_launch_plan(core_.game_install(), build_dir, run_dir, plan, error)) {
			core_.report(error);
			view_.activity.status = "The game install could not be prepared; see Problems.";
			core_.touch(ViewConcern::Output);
			return;
		}
		// The stock game takes no mission on its command line: it starts at its menu, where the
		// build's mission is listed.
		if (!in_mission.empty()) core_.note("The game install starts at its menu: choose " + in_mission + " there.");
	} else {
		plan = launcher.source_run
				? make_source_launch_plan(executable, launcher.godot_project_dir, build_dir, run_dir,
						  view_.project.document->target_game, launcher.mcp_port, in_mission,
						  launcher.engine_args)
				: make_play_launch_plan(executable, build_dir, run_dir, view_.project.document->target_game,
						  launcher.mcp_port, in_mission, launcher.engine_args);
	}
	// The run directory is new (emptied), so the tail starts clean.
	game_log_file_ = plan.log_file;
	game_log_offset_ = 0;
	game_log_partial_.clear();
	if (!play_.start(plan, error)) {
		core_.report(error);
		view_.activity.status = "The game could not be started.";
		core_.touch(ViewConcern::Output);
		return;
	}
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
	view_.activity.play_mission = in_install ? std::string() : in_mission;
	view_.activity.play_command_line = launch_plan_command_line(plan);
	view_.activity.play_exited_on_its_own = false;
	view_.activity.play_exit_code = -1;
	core_.note("Running: " + view_.activity.play_command_line);
	view_.activity.status = in_install                          ? "Game install running."
	                        : view_.activity.play_mission.empty() ? "Game running."
	                                                              : "Game running: " + view_.activity.play_mission + ".";
	core_.touch(ViewConcern::Run);
}

void PlayController::stop() {
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
	if (now != PlayState::Stopped) tail_game_log();
	if (before != now) {
		if (now == PlayState::Stopped) {
			tail_game_log();
			absorb_exit();
		}
		view_.activity.play_state = now;
		view_.activity.play_pid = play_.pid();
		if (now == PlayState::Stopped) view_.activity.play_mcp_port = 0;
		core_.touch(ViewConcern::Run);
	}
}

void PlayController::forget_project() {
	view_.activity.boot_missing.clear();
	boot_project_.clear();
	findings_.clear();
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

// Append whatever the game wrote to its log since the last poll, line by line.
void PlayController::tail_game_log() {
	if (game_log_file_.empty()) return;
	std::ifstream in(system_path(game_log_file_), std::ios::binary);
	if (!in) return;
	in.seekg(0, std::ios::end);
	const std::streamoff size = in.tellg();
	if (size < 0 || static_cast<uint64_t>(size) <= game_log_offset_) return;
	in.seekg(static_cast<std::streamoff>(game_log_offset_));
	std::string chunk(static_cast<size_t>(static_cast<uint64_t>(size) - game_log_offset_), '\0');
	in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
	game_log_offset_ = static_cast<uint64_t>(size);
	game_log_partial_ += chunk;
	size_t start = 0;
	for (;;) {
		const size_t nl = game_log_partial_.find('\n', start);
		if (nl == std::string::npos) break;
		std::string line = game_log_partial_.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		core_.note("game: " + line);
		absorb_boot_report(line);
		absorb_mission_report(line);
		start = nl + 1;
	}
	game_log_partial_.erase(0, start);
	// The names the lines reported become their rows in the validation they left due
	// (absorb_boot_report), which the polls step (S13 A3).
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
