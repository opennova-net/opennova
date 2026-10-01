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
#include <editor/model/diagnostic.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/problems_service.h>
#include <editor/session/session_core.h>

namespace fs = std::filesystem;

namespace opennova::editor {

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

// Play refused before any build: where nothing can be spawned, and while a game runs (a second
// one would fight it for its files). True when refused, said why.
bool PlayController::refused() {
	if (!core_.platform().can_spawn()) {
		core_.report(make_finding(CoreFinding::PlayUnsupported, DiagnosticSeverity::Error,
		                          "Play is Windows-only for now: the editor cannot start the game on this system. "
		                          "Build works here."));
		return true;
	}
	if (play_.state() != PlayState::Stopped) {
		core_.report(make_finding(CoreFinding::PlayAlreadyRunning, DiagnosticSeverity::Error,
		                          "The game is already running; stop it before starting it again."));
		return true;
	}
	return false;
}

void PlayController::start() {
	const std::string &build_dir = view_.activity.last_build->build_dir;
	LaunchPlan plan;
	Diagnostic error;
	std::error_code ec;
	// The last run's boot report and exit go, their rows with them (the next validation would
	// make none); the game started now reports on this project.
	view_.activity.boot_missing.clear();
	core_.problems().set_play_findings({});
	const size_t rows = view_.findings.diagnostics.size();
	view_.findings.diagnostics.erase(
			std::remove_if(view_.findings.diagnostics.begin(), view_.findings.diagnostics.end(),
					[](const Diagnostic &d) {
						return d.row() == &finding_code(CoreFinding::PlayBootMissing) ||
						       d.row() == &finding_code(CoreFinding::PlayCrashed);
					}),
			view_.findings.diagnostics.end());
	core_.touch(ViewConcern::Run);
	if (view_.findings.diagnostics.size() != rows) core_.touch(ViewConcern::Findings);
	boot_project_ = view_.project.root;
	const bool in_install = core_.preferences().values().play_in_install;
	// What Play launches, asked of its source now that the build has landed: one answer, which the
	// plan takes whole (the executable, whether the run drives the source checkout, the Godot
	// options) with the port of the game's MCP endpoint allocated now (none for the game install,
	// which has no endpoint); the view's runtime follows it.
	const PlayLauncher launcher = source_ ? source_(!in_install) : launcher_;
	follow_launcher(launcher);
	if (in_install) {
		if (!prepare_retail_launch_plan(core_.game_install(), build_dir, plan, error)) {
			core_.report(error);
			view_.activity.status = "The game install could not be prepared; see Problems.";
			core_.touch(ViewConcern::Output);
			return;
		}
	} else {
		const std::string executable = resolve_runtime_executable();
		if (executable.empty() || !fs::is_regular_file(executable, ec)) {
			core_.report(make_finding(CoreFinding::PlayRuntimeMissing, DiagnosticSeverity::Error,
			                          executable.empty()
			                                  ? "No game runtime is set; choose opennova.exe in File > Project settings..."
			                                  : "The game runtime was not found: " + executable));
			view_.activity.status = "The game runtime was not found.";
			core_.touch(ViewConcern::Output);
			return;
		}
		plan = launcher.source_run
				? make_source_launch_plan(executable, launcher.godot_project_dir, build_dir,
						  view_.project.document->target_game, launcher.mcp_port, std::string(),
						  launcher.engine_args)
				: make_play_launch_plan(executable, build_dir, view_.project.document->target_game,
						  launcher.mcp_port, std::string(), launcher.engine_args);
	}
	// The game rewrites its log; drop the previous run's so the tail starts clean.
	fs::remove(plan.log_file, ec);
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
	view_.activity.play_state = play_.state();
	view_.activity.play_pid = play_.pid();
	view_.activity.play_mcp_port = plan.mcp_port;
	view_.activity.play_command_line = launch_plan_command_line(plan);
	view_.activity.play_exited_on_its_own = false;
	view_.activity.play_exit_code = -1;
	core_.note("Running: " + view_.activity.play_command_line);
	view_.activity.status = in_install ? "Game install running." : "Game running.";
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
	std::ifstream in(game_log_file_, std::ios::binary);
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

// How the game ended, on the poll that saw it end: stopped, quit (exit code 0, or one the
// platform could not read), or any other code: a crash or an error exit, said in Output with
// its code and a Problems row (play.crashed) that stays, like the boot report, until Play
// starts again or the project closes (a game of a project closed since reports nothing).
void PlayController::absorb_exit() {
	// The game is gone: its lease goes, its own and no other (another game may run from the same
	// build), and its directory is a build like any.
	if (play_lease_.pid >= 0) remove_play_lease(play_lease_.build_dir, play_lease_.pid);
	play_lease_ = PlayLease();
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
			core_.problems().set_play_findings({make_finding(CoreFinding::PlayCrashed, DiagnosticSeverity::Error,
			                                                 "The game ended with exit code " + code +
			                                                         ": it crashed or stopped on an error. Its log is in Output.")});
			core_.problems().validate_later();
		}
	}
	core_.note(line);
	view_.activity.status = line;
}

} // namespace opennova::editor
