#include <editor/session/project_session.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include <editor/blank/create_missing.h>
#include <editor/project_build/build_plan.h>
#include <editor/run/launch_plan.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

constexpr size_t kOutputLinesMax = 2000;

} // namespace

ProjectSession::ProjectSession(ProcessPlatform &platform, std::string editor_settings_path)
		: platform_(platform), settings_path_(std::move(editor_settings_path)), play_(platform) {
	Diagnostic error;
	if (!load_editor_settings(settings_path_, settings_, error)) {
		settings_ = EditorSettings();
		report(error);
	}
	view_.recent_projects = settings_.recent_projects;
	view_.status = "No project open.";
	touch();
}

ProjectSession::~ProjectSession() {
	// A game left running outlives the editor on purpose (a modder may keep playing);
	// the handle is released, never the process.
	if (play_.state() != PlayState::Stopped) platform_.release(play_.pid());
}

void ProjectSession::set_launcher(PlayLauncher launcher) {
	launcher_ = std::move(launcher);
	view_.source_run = launcher_.source_run;
	view_.runtime_executable = resolve_runtime_executable();
	touch();
}

bool ProjectSession::handle(const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::NewProject: new_project(request.path, request.text); return true;
	case EditorRequestKind::OpenProject: open_project(request.path); return true;
	case EditorRequestKind::CloseProject: close_project(); return true;
	case EditorRequestKind::ForgetRecent:
		forget_recent_project(settings_, request.path);
		save_editor_settings();
		return true;
	case EditorRequestKind::Rescan:
		if (view_.project_open) refresh();
		return true;
	case EditorRequestKind::SetTitle:
		if (view_.project_open && !request.text.empty()) {
			view_.document.title = request.text;
			save_document();
		}
		return true;
	case EditorRequestKind::SetFeature:
		if (view_.project_open) {
			if (request.text == "mission") view_.document.features.mission = request.flag;
			else if (request.text == "multiplayer") view_.document.features.multiplayer = request.flag;
			else return true;
			save_document();
			refresh();
		}
		return true;
	case EditorRequestKind::SetRuntimeExecutable:
		settings_.runtime_executable = request.path;
		save_editor_settings();
		view_.runtime_executable = resolve_runtime_executable();
		touch();
		return true;
	case EditorRequestKind::CreateMissing:
		if (view_.project_open) create_missing(request.text);
		return true;
	case EditorRequestKind::Build:
		if (view_.project_open) start_build(false);
		return true;
	case EditorRequestKind::Play:
		if (view_.project_open) start_build(true);
		return true;
	case EditorRequestKind::StopPlay: stop_play(); return true;
	case EditorRequestKind::PickDirectory:
	case EditorRequestKind::PickFile:
	case EditorRequestKind::RevealPath:
	case EditorRequestKind::Quit: return false;
	}
	return false;
}

void ProjectSession::poll() {
	if (build_) {
		build_->step();
		view_.build_done = build_->steps_done();
		view_.build_total = build_->steps_total();
		view_.build_step = build_->last_step();
		touch();
		if (build_->done()) absorb_build();
	}
	const PlayState before = play_.state();
	const PlayState now = play_.poll();
	if (now != PlayState::Stopped) tail_game_log();
	if (before != now) {
		if (now == PlayState::Stopped) {
			tail_game_log();
			view_.play_exited_on_its_own = play_.exited_on_its_own();
			note(view_.play_exited_on_its_own ? "The game exited." : "The game was stopped.");
			view_.status = view_.play_exited_on_its_own ? "The game exited." : "The game was stopped.";
		}
		view_.play_state = now;
		view_.play_pid = play_.pid();
		touch();
	}
}

void ProjectSession::finish_build() {
	while (build_) poll();
}

bool ProjectSession::new_project(const std::string &dir, const std::string &title) {
	if (dir.empty()) return false;
	if (view_.project_open) close_project();
	ProjectDocument doc;
	Diagnostic error;
	if (!create_project(dir, title.empty() ? std::string("New Game") : title, kDefaultTargetGame, doc, error)) {
		report(error);
		view_.status = "The project could not be created.";
		touch();
		return false;
	}
	note("Created " + doc.title + " at " + dir);
	return open_project(dir);
}

bool ProjectSession::open_project(const std::string &dir) {
	if (dir.empty()) return false;
	if (view_.project_open) close_project();
	ProjectDocument doc;
	Diagnostic error;
	if (!::opennova::editor::open_project(dir, doc, error)) {
		report(error);
		forget_recent_project(settings_, dir);
		save_editor_settings();
		view_.status = "The project could not be opened.";
		touch();
		return false;
	}
	paths_ = ProjectPaths::for_root(dir);
	Diagnostic local_error;
	if (!load_local_settings(paths_.local_settings_file, local_, local_error)) {
		local_ = LocalSettings();
		report(local_error);
	}
	view_.project_open = true;
	view_.project_root = paths_.root;
	view_.document = doc;
	view_.has_build = false;
	view_.last_build = BuildReport();
	remember_recent_project(settings_, paths_.root);
	save_editor_settings();
	view_.runtime_executable = resolve_runtime_executable();
	refresh();
	note("Opened " + doc.title + " (" + paths_.root + ")");
	view_.status = "Opened " + doc.title + ".";
	touch();
	return true;
}

void ProjectSession::close_project() {
	if (!view_.project_open) return;
	if (build_) {
		finish_build();
	}
	const std::string title = view_.document.title;
	view_.project_open = false;
	view_.project_root.clear();
	view_.document = ProjectDocument();
	view_.scan = AssetScan();
	view_.requirements = RequirementReport();
	view_.diagnostics.clear();
	view_.has_build = false;
	view_.last_build = BuildReport();
	paths_ = ProjectPaths();
	local_ = LocalSettings();
	view_.runtime_executable = resolve_runtime_executable();
	note("Closed " + title);
	view_.status = "No project open.";
	touch();
}

// Re-read the project's files and re-evaluate the checklist; the project findings
// replace the last action's.
void ProjectSession::refresh() {
	view_.scan = scan_project_assets(paths_, view_.document);
	view_.requirements = evaluate_requirements(view_.document, view_.scan);
	view_.diagnostics = view_.scan.diagnostics;
	for (const Diagnostic &d : view_.requirements.diagnostics) view_.diagnostics.push_back(d);
	touch();
}

void ProjectSession::save_document() {
	Diagnostic error;
	if (!save_project_document(paths_.project_file, view_.document, error)) {
		report(error);
		return;
	}
	view_.status = "Saved " + view_.document.title + ".";
	touch();
}

void ProjectSession::create_missing(const std::string &role) {
	const CreateMissingResult result =
	        create_missing_requirements(paths_, view_.document, view_.requirements, role);
	for (const std::string &path : result.created) note("Created " + path);
	for (const std::string &name : result.unavailable) {
		note("The editor cannot create " + name + " yet: no writer exists for this kind of file.");
	}
	refresh();
	for (const Diagnostic &d : result.diagnostics) report(d);
	if (result.created.empty() && result.unavailable.empty() && result.diagnostics.empty()) {
		view_.status = "Nothing to create.";
	} else {
		view_.status = std::to_string(result.created.size()) + " file(s) created" +
		               (result.unavailable.empty()
		                        ? "."
		                        : ", " + std::to_string(result.unavailable.size()) + " not yet possible.");
	}
	touch();
}

void ProjectSession::start_build(bool then_play) {
	if (then_play && play_.state() != PlayState::Stopped) {
		report(make_diagnostic(DiagnosticSeverity::Error, "play.already_running",
		                       "The game is already running; stop it before starting it again."));
		return;
	}
	play_after_build_ = play_after_build_ || then_play;
	if (build_) return; // the running build serves this request too
	refresh();
	const BuildPlan plan = plan_build(paths_, view_.document, view_.scan, view_.requirements);
	std::vector<std::string> protected_dirs;
	if (!play_.running_build_dir().empty()) protected_dirs.push_back(play_.running_build_dir());
	build_ = std::make_unique<BuildRun>(plan, view_.document, paths_.build_dir + "/play", protected_dirs);
	view_.build_running = true;
	view_.build_done = 0;
	view_.build_total = build_->steps_total();
	view_.build_step.clear();
	view_.status = "Building...";
	note("Build started.");
	touch();
}

void ProjectSession::absorb_build() {
	const BuildReport result = build_->report();
	build_.reset();
	view_.build_running = false;
	view_.has_build = true;
	view_.last_build = result;
	for (const Diagnostic &d : result.diagnostics) report(d);
	if (result.ok) {
		if (result.reused_existing) {
			note("Build unchanged: " + result.build_dir);
			view_.status = "Build unchanged.";
		} else {
			note("Built " + result.build_dir + " (" + std::to_string(result.archives_written.size()) +
			     " archive(s) written, " + std::to_string(result.archives_reused.size()) + " reused, " +
			     std::to_string(result.loose_written.size()) + " loose file(s))");
			view_.status = "Build finished.";
		}
	} else {
		note("Build failed.");
		view_.status = "Build failed; see Problems.";
	}
	const bool play = play_after_build_;
	play_after_build_ = false;
	if (result.ok && play) start_play();
	touch();
}

std::string ProjectSession::resolve_runtime_executable() const {
	if (launcher_.source_run) return launcher_.executable;
	if (!local_.runtime_executable.empty()) return local_.runtime_executable;
	if (!settings_.runtime_executable.empty()) return settings_.runtime_executable;
	return launcher_.executable;
}

void ProjectSession::start_play() {
	const std::string executable = resolve_runtime_executable();
	std::error_code ec;
	if (executable.empty() || !fs::is_regular_file(executable, ec)) {
		report(make_diagnostic(DiagnosticSeverity::Error, "play.runtime_missing",
		                       executable.empty()
		                               ? "No game runtime is set; choose opennova.exe under Project."
		                               : "The game runtime was not found: " + executable));
		view_.status = "The game runtime was not found.";
		touch();
		return;
	}
	const std::string &build_dir = view_.last_build.build_dir;
	const LaunchPlan plan =
	        launcher_.source_run
	                ? make_source_launch_plan(executable, launcher_.godot_project_dir, build_dir,
	                                          view_.document.target_game, launcher_.mcp_port, std::string(),
	                                          launcher_.engine_args)
	                : make_play_launch_plan(executable, build_dir, view_.document.target_game,
	                                        launcher_.mcp_port, std::string(), launcher_.engine_args);
	// The game rewrites its log; drop the previous run's so the tail starts clean.
	fs::remove(plan.log_file, ec);
	game_log_file_ = plan.log_file;
	game_log_offset_ = 0;
	game_log_partial_.clear();
	Diagnostic error;
	if (!play_.start(plan, error)) {
		report(error);
		view_.status = "The game could not be started.";
		touch();
		return;
	}
	view_.play_state = play_.state();
	view_.play_pid = play_.pid();
	view_.play_command_line = launch_plan_command_line(plan);
	view_.play_exited_on_its_own = false;
	note("Running: " + view_.play_command_line);
	view_.status = "Game running.";
	touch();
}

void ProjectSession::stop_play() {
	if (play_.state() != PlayState::Running) return;
	play_.stop();
	view_.play_state = play_.state();
	view_.status = "Stopping the game...";
	note("Stop requested.");
	touch();
}

void ProjectSession::note(std::string line) {
	view_.output.push_back(std::move(line));
	if (view_.output.size() > kOutputLinesMax) {
		view_.output.erase(view_.output.begin(),
		                   view_.output.begin() +
		                           static_cast<std::ptrdiff_t>(view_.output.size() - kOutputLinesMax));
	}
	touch();
}

void ProjectSession::report(const Diagnostic &d) {
	view_.diagnostics.push_back(d);
	note(std::string(diagnostic_severity_label(d.severity)) + ": " + d.message);
}

// Append whatever the game wrote to its log since the last poll, line by line.
void ProjectSession::tail_game_log() {
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
		note("game: " + line);
		start = nl + 1;
	}
	game_log_partial_.erase(0, start);
}

void ProjectSession::save_editor_settings() {
	Diagnostic error;
	if (!::opennova::editor::save_editor_settings(settings_path_, settings_, error)) report(error);
	view_.recent_projects = settings_.recent_projects;
	touch();
}

} // namespace opennova::editor
