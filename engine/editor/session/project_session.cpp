#include <editor/session/project_session.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include <editor/blank/create_missing.h>
#include <editor/blank/blank_factory.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/project/project_files.h>
#include <editor/documents/catalog_validation.h>
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
	view_.retail_directory = settings_.retail_directory;
	view_.play_retail = settings_.play_retail;
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
	if (guard_unsaved(request) || handle_document(request)) return true;
	switch (request.kind) {
	case EditorRequestKind::NewProject: new_project(request.path, request.text); return true;
	case EditorRequestKind::OpenProject: open_project(request.path); return true;
	case EditorRequestKind::CloseProject: close_project(); return true;
	case EditorRequestKind::ForgetRecent:
		forget_recent_project(settings_, request.path);
		save_editor_settings();
		return true;
	case EditorRequestKind::Rescan:
		if (view_.project_open) {
            for (auto &document : documents_) if (!document->dirty()) {
                auto loaded = std::make_shared<EditableDocument>(); Diagnostic error;
                if (loaded->load((fs::path(paths_.root) / document->path()).generic_string(),
                    document->path(), document->kind(), view_.document.target_game, error)) document = loaded;
            }
            view_.selection = {}; update_document_view(); refresh();
        }
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
	case EditorRequestKind::PreviewImport:
		if (view_.project_open) {
			std::vector<Diagnostic> diagnostics;
			view_.import_sources = list_import_sources(request.paths, diagnostics);
			view_.import_open = !view_.import_sources.empty();
			for (const auto &d : diagnostics) report(d);
			touch();
		}
		return true;
	case EditorRequestKind::CancelImport:
		view_.import_open = false;
		view_.import_sources.clear();
		touch();
		return true;
	case EditorRequestKind::ImportFiles:
		view_.import_open = false;
		view_.import_sources.clear();
		if (!view_.project_open) return true;
		if (documents_dirty()) {
			report(make_diagnostic(DiagnosticSeverity::Error, "import.unsaved",
			                       "Save edited catalog files before importing."));
			return true;
		}
		{
			const ImportResult imported = import_assets(request.imports, paths_, view_.document, request.flag);
			handle(make_request(EditorRequestKind::Rescan));
			for (const auto &path : imported.imported) note("Imported " + path);
			for (const auto &d : imported.diagnostics) report(d);
			view_.status = std::to_string(imported.imported.size()) + " file(s) imported.";
			touch();
		}
		return true;
	case EditorRequestKind::SetRetailDirectory:
		settings_.retail_directory = request.path;
		save_editor_settings();
		return true;
	case EditorRequestKind::SetPlayRetail:
		settings_.play_retail = request.flag;
		save_editor_settings();
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
	case EditorRequestKind::RevealPath: return false;
	case EditorRequestKind::Quit: view_.quit_requested = true; touch(); return true;
	default: return false;
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
	documents_.clear(); view_.active_document.clear(); view_.selection = {};
	update_document_view();
	view_.project_open = false;
	view_.import_open = false;
	view_.import_sources.clear();
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
	validate_documents();
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
	if (documents_dirty()) {
		view_.has_build = false; view_.last_build = BuildReport(); play_after_build_ = false;
		report(make_diagnostic(DiagnosticSeverity::Error, "build.unsaved", "Save the edited catalog files before Build or Play."));
		return;
	}
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
	const std::string &build_dir = view_.last_build.build_dir;
	LaunchPlan plan;
	Diagnostic error;
	std::error_code ec;
	if (settings_.play_retail) {
		if (!prepare_retail_launch_plan(settings_.retail_directory, build_dir, plan, error)) {
			report(error);
			view_.status = "Retail could not be prepared; see Problems.";
			touch();
			return;
		}
	} else {
		const std::string executable = resolve_runtime_executable();
		if (executable.empty() || !fs::is_regular_file(executable, ec)) {
			report(make_diagnostic(DiagnosticSeverity::Error, "play.runtime_missing",
			                       executable.empty()
			                               ? "No game runtime is set; choose opennova.exe under Project."
			                               : "The game runtime was not found: " + executable));
			view_.status = "The game runtime was not found.";
			touch();
			return;
		}
		plan = launcher_.source_run
		               ? make_source_launch_plan(executable, launcher_.godot_project_dir, build_dir,
		                                         view_.document.target_game, launcher_.mcp_port, std::string(),
		                                         launcher_.engine_args)
		               : make_play_launch_plan(executable, build_dir, view_.document.target_game,
		                                       launcher_.mcp_port, std::string(), launcher_.engine_args);
	}
	// The game rewrites its log; drop the previous run's so the tail starts clean.
	fs::remove(plan.log_file, ec);
	game_log_file_ = plan.log_file;
	game_log_offset_ = 0;
	game_log_partial_.clear();
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
	view_.status = settings_.play_retail ? "Retail running." : "Game running.";
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
	view_.retail_directory = settings_.retail_directory;
	view_.play_retail = settings_.play_retail;
	touch();
}


EditableDocument *ProjectSession::document_for(const std::string &path) {
	const std::string &wanted = path.empty() ? view_.active_document : path;
	for (auto &document : documents_)
		if (document->path() == wanted || normalized_logical_name(fs::path(document->path()).filename().string()) == normalized_logical_name(wanted))
			return document.get();
	return nullptr;
}

bool ProjectSession::documents_dirty() const {
	for (const auto &document : documents_) if (document->dirty()) return true;
	return false;
}

void ProjectSession::update_document_view() {
	view_.documents.clear();
	for (const auto &document : documents_) view_.documents.push_back(document);
	touch();
}

void ProjectSession::validate_documents() {
	view_.diagnostics = view_.scan.diagnostics;
	for (const auto &d : view_.requirements.diagnostics) view_.diagnostics.push_back(d);
	for (const auto &d : validate_catalogs(paths_, view_.document, view_.scan, view_.documents))
		view_.diagnostics.push_back(d);
	touch();
}

bool ProjectSession::save_documents(bool all) {
	if (build_) {
		report(make_diagnostic(DiagnosticSeverity::Error, "document.build_running", "Wait for the build to finish before saving."));
		return false;
	}
	for (const auto &document : documents_) {
		if ((!all && document->path() != view_.active_document) || !document->dirty()) continue;
		Diagnostic error;
		if (!document->save(error)) { report(error); return false; }
	}
	update_document_view();
	refresh();
	view_.status = "Saved.";
	return true;
}

bool ProjectSession::handle_document(const EditorRequest &request) {
    switch (request.kind) {
    case EditorRequestKind::CreateCatalog: {
        if (!view_.project_open || build_) return true;
        const std::string name = normalized_logical_name(request.path);
        if (name != "ITEMS.DEF" && name != "WEAPON.DEF" && name != "AMMO.DEF") return true;
        const AssetKind kind = classify_asset(request.path, nullptr);
        const auto *existing = view_.scan.find(request.path);
        if (!existing) {
            const auto target = fs::path(paths_.root) / blank_placement_dir(kind) / request.path;
            std::error_code ec;
            if (fs::exists(target, ec) || ec) {
                report(make_diagnostic(DiagnosticSeverity::Error, "document.conflict", "Rescan before creating this catalog.", request.path));
                return true;
            }
            std::vector<uint8_t> bytes; Diagnostic error;
            BlankRequest blank; blank.logical_name = request.path; blank.project_title = view_.document.title;
            if (!make_blank(blank, kind, bytes, error)) { report(error); return true; }
            std::string message;
            if (!ensure_directory(target.parent_path().generic_string(), message) ||
                !write_file_atomic(target.generic_string(), bytes.data(), bytes.size(), message)) {
                report(make_diagnostic(DiagnosticSeverity::Error, "document.write", message, request.path));
                return true;
            }
            refresh();
        }
        handle(make_request(EditorRequestKind::OpenDocument, request.path));
        return true;
    }
	case EditorRequestKind::OpenDocument:
	case EditorRequestKind::ReloadDocument: {
		if (!view_.project_open) return true;
		const std::string path = request.path.empty() ? view_.active_document : request.path;
		if (request.kind == EditorRequestKind::OpenDocument && document_for(path)) {
			view_.active_document = document_for(path)->path(); view_.selection = request.catalog_edit.address; touch(); return true;
		}
		for (const auto &asset : view_.scan.entries) {
			if (asset.relative_path != path && normalized_logical_name(asset.logical_name) != normalized_logical_name(path)) continue;
			auto document = std::make_shared<EditableDocument>(); Diagnostic error;
			if (!document->load((fs::path(paths_.root) / asset.relative_path).generic_string(), asset.relative_path,
				asset.kind, view_.document.target_game, error)) { report(error); return true; }
			for (auto it = documents_.begin(); it != documents_.end(); ++it)
				if ((*it)->path() == asset.relative_path) { documents_.erase(it); break; }
			documents_.push_back(document); view_.active_document = document->path();
			view_.selection = request.catalog_edit.address;
			update_document_view(); validate_documents(); return true;
		}
		report(make_diagnostic(DiagnosticSeverity::Error, "document.missing", "The catalog file was not found.", path));
		return true;
	}
	case EditorRequestKind::CloseDocument: {
		const std::string path = request.path.empty() ? view_.active_document : request.path;
		for (auto it = documents_.begin(); it != documents_.end(); ++it)
			if ((*it)->path() == path) { documents_.erase(it); break; }
		if (view_.active_document == path) view_.active_document = documents_.empty() ? "" : documents_.back()->path();
		view_.selection = {}; update_document_view(); validate_documents(); return true;
	}
	case EditorRequestKind::SelectRecord:
		if (!request.path.empty()) view_.active_document = request.path;
		view_.selection = request.catalog_edit.address; touch(); return true;
	case EditorRequestKind::EditRecord: {
		if (auto *document = document_for(request.path)) {
			Diagnostic error;
			if (!document->apply(request.catalog_edit, error)) report(error);
			else {
				if (request.catalog_edit.operation == CatalogOperation::Add || request.catalog_edit.operation == CatalogOperation::Duplicate) {
					const auto kind = request.catalog_edit.address.kind;
					const bool top = kind == document->record_kind() || kind == def::DefRecordKind::Carry;
					view_.selection = {top ? document->last_added() : request.catalog_edit.address.row, kind,
						top ? 0 : document->last_added()};
				}
				update_document_view(); validate_documents();
				view_.status = "Edited " + document->path() + ".";
			}
		}
		return true;
	}
	case EditorRequestKind::Undo:
	case EditorRequestKind::Redo:
		if (auto *document = document_for(request.path)) {
			if (request.kind == EditorRequestKind::Undo) document->undo(); else document->redo();
			update_document_view(); validate_documents();
		}
		return true;
	case EditorRequestKind::EndEdit:
		if (auto *document = document_for(request.path)) document->end_edit_group();
		return true;
	case EditorRequestKind::Save: save_documents(false); return true;
	case EditorRequestKind::SaveAll: save_documents(true); return true;
	default: return false;
	}
}

bool ProjectSession::guard_unsaved(const EditorRequest &request) {
	if (request.kind == EditorRequestKind::ResolveUnsaved) {
		if (!pending_request_) return true;
		if (request.unsaved_choice == UnsavedChoice::Cancel) {
			pending_request_.reset(); view_.unsaved_prompt = false; touch(); return true;
		}
		if (request.unsaved_choice == UnsavedChoice::SaveAll && !save_documents(true)) return true;
		EditorRequest pending = *pending_request_;
		pending_request_.reset(); view_.unsaved_prompt = false;
		if (request.unsaved_choice == UnsavedChoice::Discard) {
			if (pending.kind == EditorRequestKind::CloseDocument || pending.kind == EditorRequestKind::ReloadDocument) {
				for (auto it = documents_.begin(); it != documents_.end(); ++it)
					if ((*it)->path() == pending.path) { documents_.erase(it); break; }
			} else documents_.clear();
			update_document_view();
		}
		handle(pending); return true;
	}
	bool guard = false;
	if (request.kind == EditorRequestKind::CloseDocument || request.kind == EditorRequestKind::ReloadDocument) {
		if (const auto *document = document_for(request.path)) guard = document->dirty();
	} else if (request.kind == EditorRequestKind::NewProject || request.kind == EditorRequestKind::OpenProject ||
		request.kind == EditorRequestKind::CloseProject || request.kind == EditorRequestKind::Quit) guard = documents_dirty();
	if (!guard) return false;
	pending_request_ = request;
	if (request.kind == EditorRequestKind::CloseDocument || request.kind == EditorRequestKind::ReloadDocument)
		pending_request_->path = document_for(request.path)->path();
	view_.unsaved_prompt = true; touch(); return true;
}

} // namespace opennova::editor
