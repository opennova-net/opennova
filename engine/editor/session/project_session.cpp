#include <editor/session/project_session.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include <base/gameprofile/required_resources.h>
#include <editor/blank/create_missing.h>
#include <editor/blank/blank_factory.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/project/project_files.h>
#include <editor/documents/document_types.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/project/project_findings.h>
#include <editor/project/project_state.h>
#include <editor/project_build/build_plan.h>
#include <editor/run/launch_plan.h>
#include <editor/run/play_lease.h>
#include <editor/session/build_operation.h>
#include <editor/session/request_kinds.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

bool same_finding(const Diagnostic &a, const Diagnostic &b) {
	return a.severity == b.severity && a.code == b.code && a.message == b.message && a.asset == b.asset &&
	       a.field == b.field && a.record == b.record && a.line == b.line;
}

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
	view_.runtime_setting = settings_.runtime_executable;
	view_.import_dependencies = settings_.import_dependencies;
	view_.graph = graph_;
	view_.assets = assets_;
	view_.render_check = render_check_;
	view_.status = "No project open.";
	touch(ViewConcern::Preferences);
	touch(ViewConcern::Graph);
	touch(ViewConcern::Output);
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
	touch(ViewConcern::Preferences);
}

bool ProjectSession::handle(const EditorRequest &request) {
	if (handling_ == 0) outcome_ = ActionOutcome();
	bool served = false;
	{
		struct Depth {
			int &depth;
			explicit Depth(int &d) : depth(d) { ++depth; }
			~Depth() { --depth; }
		} depth(handling_);
		served = dispatch(request);
	}
	// A request from outside returns validated, unless a pump holds validation for its poll.
	if (handling_ == 0 && !validation_held_) validate_pending();
	return served;
}

bool ProjectSession::dispatch(const EditorRequest &request) {
	if (request.kind == EditorRequestKind::ResolveUnsaved) {
		resolve_unsaved(request.unsaved_choice);
		return true;
	}
	// Build and Play pack the files as saved: every edit group ends first, as EndEdit ends
	// one, so a keystroke after them is a step of its own.
	if (request.kind == EditorRequestKind::Build || request.kind == EditorRequestKind::Play) end_edit_groups();
	if (gate_busy(request) || guard_unsaved(request) || handle_document(request)) return true;
	switch (request.kind) {
	case EditorRequestKind::NewProject: new_project(request.path, request.text); return true;
	case EditorRequestKind::OpenProject: open_project(request.path); return true;
	case EditorRequestKind::CloseProject: close_project(); return true;
	case EditorRequestKind::ForgetRecent:
		forget_recent_project(settings_, request.path);
		save_editor_settings();
		return true;
	case EditorRequestKind::Rescan:
		// Only what changed outside the editor is read again: an open document whose file
		// holds what it was read from keeps its records, its history and its selection.
		if (view_.project_open) {
			reload_changed_documents();
			refresh();
		}
		return true;
	case EditorRequestKind::ApplyProjectSettings: apply_project_settings(request.settings); return true;
	case EditorRequestKind::PreviewImport:
		if (view_.project_open) {
			std::vector<Diagnostic> diagnostics;
			std::vector<ImportSource> choices, roots;
			// A loose file picked is chosen; an archive's members are listed to choose from.
			for (ImportSource &source : list_import_sources(request.paths, diagnostics))
				(source.entry.empty() ? roots : choices).push_back(std::move(source));
			for (const auto &d : diagnostics) report(d);
			preview_import(std::move(choices), std::move(roots), request.flag);
		}
		return true;
	case EditorRequestKind::PlanImport:
		if (view_.project_open)
			preview_import(view_.import_preview.open ? view_.import_preview.choices : std::vector<ImportSource>(),
			               request.imports, request.flag);
		return true;
	case EditorRequestKind::SetImportDependencies: set_import_dependencies(request.flag); return true;
	case EditorRequestKind::CancelImport:
		view_.import_preview = SessionView::ImportPreview();
		touch(ViewConcern::Dialogs);
		return true;
	case EditorRequestKind::ImportFiles: import_files(request); return true;
	case EditorRequestKind::PreviewRetailImport:
		if (view_.project_open) {
			std::vector<Diagnostic> diagnostics;
			std::vector<ImportSource> sources = list_retail_import_sources(game_install(), view_.document, diagnostics);
			// With names (an Import fix): those files alone, chosen; a name the game data does
			// not have is a finding (unless the install itself is the finding). Without, every
			// file is listed to choose from.
			std::vector<ImportSource> named;
			for (const std::string &name : request.names) {
				const auto found = std::find_if(sources.begin(), sources.end(), [&name](const ImportSource &source) {
					return normalized_logical_name(source.entry) == normalized_logical_name(name);
				});
				if (found == sources.end()) {
					if (diagnostics.empty())
						diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "import.source",
						                                      "The game data has no file named " + name + "."));
					continue;
				}
				named.push_back(*found);
			}
			if (!request.names.empty()) sources.clear();
			for (const auto &d : diagnostics) report(d);
			preview_import(std::move(sources), std::move(named), request.flag);
		}
		return true;
	case EditorRequestKind::Reimport: reimport(request.path, request.flag); return true;
	case EditorRequestKind::CreateMissing:
		if (view_.project_open) create_missing(request.names);
		return true;
	case EditorRequestKind::Build:
		if (view_.project_open) start_build(false);
		return true;
	case EditorRequestKind::Play:
		if (view_.project_open) start_build(true);
		return true;
	case EditorRequestKind::StopPlay: stop_play(); return true;
	case EditorRequestKind::CancelOperation: cancel_operation(true); return true;
	case EditorRequestKind::RenameAsset: rename_asset(request.path, request.text); return true;
	case EditorRequestKind::AssignRequirement: assign_requirement(request.text, request.path); return true;
	case EditorRequestKind::PreviewRename: preview_rename(request); return true;
	case EditorRequestKind::RenameSymbol: rename_symbol(request); return true;
	case EditorRequestKind::ClearOutput:
		view_.output.clear();
		touch(ViewConcern::Output);
		return true;
	case EditorRequestKind::PickDirectory:
	case EditorRequestKind::PickFile:
	case EditorRequestKind::RevealPath: return false;
	case EditorRequestKind::Quit:
		view_.quit_requested = true;
		touch(ViewConcern::Project);
		return true;
	default: return false;
	}
	return false;
}

void ProjectSession::poll() {
	validation_held_ = false;
	validate_pending();
	// The running operation's steps, within the poll's budget: its progress moves Operation alone.
	if (operations_.running() && !operations_.done()) {
		operations_.poll(poll_budget_, steady_clock_ms);
		view_.operation = operations_.status();
		touch(ViewConcern::Operation);
	}
	const PlayState before = play_.state();
	const PlayState now = play_.poll();
	if (now != PlayState::Stopped) tail_game_log();
	if (before != now) {
		if (now == PlayState::Stopped) {
			tail_game_log();
			absorb_play_exit();
		}
		view_.play_state = now;
		view_.play_pid = play_.pid();
		if (now == PlayState::Stopped) view_.play_mcp_port = 0;
		touch(ViewConcern::Run);
		validate_pending();
	}
	// Last, the operation found done finishes: the view learns what it came to (a build lands,
	// and the game a Play waits on starts on it).
	if (operations_.done()) finish_operation();
}

void ProjectSession::run_operations() {
	while (operations_.running()) {
		operations_.run_to_end();
		poll();
	}
}

// --- the busy gate and the operation slot -----------------------------------------------

// The busy gate (request_kinds.h): while an operation runs that holds what a request needs, the
// request's row says what happens. Refused: an operation.busy warning, the outcome not done,
// nothing changed. Joined: the running operation serves it (a Build or a Play onto a build).
// Superseded: the running operation is cancelled for it. CancelRunning: the running operation
// is cancelled and the request goes on (a project switch, Quit: a Play waiting on a build goes
// with it). A request that needs nothing it holds (an edit while a build packs) goes on. One the
// unsaved-changes prompt would hold, and which the row does not refuse, asks first and meets the
// gate once it goes ahead: a Close the prompt then drops cancels nothing, and a Build with
// unsaved edits never joins a build that packs the files without them.
bool ProjectSession::gate_busy(const EditorRequest &request) {
	if (!busy_for(request.kind)) return false;
	const OnBusy on_busy = request_kind_row(request.kind).on_busy;
	if (on_busy != OnBusy::Refuse) {
		std::vector<std::string> unsaved;
		if (unsaved_files(request, unsaved) && !unsaved.empty()) return false;
	}
	switch (on_busy) {
	case OnBusy::CancelRunning:
		cancel_operation(false);
		return false;
	case OnBusy::Join:
		if (join_operation(request)) return true;
		break;
	case OnBusy::Supersede:
		if (operations_.running()->superseded_by(request)) {
			cancel_operation(false);
			return false;
		}
		break;
	case OnBusy::Refuse: break;
	}
	refuse_busy(request.path);
	return true;
}

bool ProjectSession::busy_for(EditorRequestKind kind) const {
	const SessionOperation *running = operations_.running();
	return running && holds_any(request_kind_row(kind).needs, running->holds());
}

void ProjectSession::refuse_busy(const std::string &asset) {
	const SessionOperation *running = operations_.running();
	const std::string noun = running ? operation_kind_row(running->kind()).noun : "the operation";
	refuse_now("operation.busy", "Wait for " + noun + " to finish, or cancel it, first.", asset);
}

// A Build or a Play onto the running build: the build serves it (a Play refused before it could,
// as it would be before any build: no spawn here, or a game running). The outcome names the
// operation joined.
bool ProjectSession::join_operation(const EditorRequest &request) {
	if (request.kind == EditorRequestKind::Play && play_refused()) return true;
	if (!operations_.running()->join(request)) return false;
	outcome_.operation = view_.operation.id;
	if (request.kind == EditorRequestKind::Play) {
		view_.status = "Building, then playing...";
		note("Play starts the game when the build lands.");
	}
	return true;
}

// The running operation stopped between two steps, its work discarded (a build's staging
// directory removed, the Play waiting on it dropped); `asked` is CancelOperation's, which says
// so when nothing runs.
void ProjectSession::cancel_operation(bool asked) {
	const SessionOperation *running = operations_.running();
	if (!running) {
		if (asked) refuse_now("operation.none", "Nothing is running to cancel.");
		return;
	}
	const std::string noun = operation_kind_row(running->kind()).noun;
	if (!operations_.cancel()) {
		if (asked) refuse_now("operation.not_cancellable", "This cannot be cancelled now: wait for " + noun + " to finish.");
		return;
	}
	const std::string line = "Cancelled " + noun + ".";
	if (asked) view_.status = line;
	note(line);
	show_operation();
}

void ProjectSession::finish_operation() {
	operations_.finish(*this);
	show_operation();
}

// The slot as the view shows it: the running operation (none) and what the last one came to.
void ProjectSession::show_operation() {
	view_.operation = operations_.status();
	view_.last_operation = operations_.last();
	touch(ViewConcern::Operation);
}

bool ProjectSession::new_project(const std::string &dir, const std::string &title) {
	if (dir.empty()) return false;
	if (view_.project_open) close_project();
	ProjectDocument doc;
	Diagnostic error;
	if (!create_project(dir, title.empty() ? std::string("New Game") : title, kDefaultTargetGame, doc, error)) {
		report(error);
		view_.status = "The project could not be created.";
		touch(ViewConcern::Output);
		return false;
	}
	note("Created " + doc.title + ".");
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
		touch(ViewConcern::Output);
		return false;
	}
	paths_ = ProjectPaths::for_root(dir);
	// The project's game install is its own (local.json); one that names none starts from the
	// install last chosen in the editor.
	Diagnostic local_error;
	if (!open_local_settings(paths_, settings_.retail_directory, local_, local_error))
		report(local_error);
	view_.project_open = true;
	view_.project_root = paths_.root;
	view_.document = doc;
	view_.has_build = false;
	view_.last_build = BuildReport();
	remember_recent_project(settings_, paths_.root);
	save_editor_settings();
	view_.runtime_executable = resolve_runtime_executable();
	refresh_retail_files();
	refresh();
	// Output names the project; the menu bar's tooltip on what was said names its folder.
	note("Opened " + doc.title + ".");
	view_.status = "Opened " + doc.title + ".";
	touch(ViewConcern::Project);
	touch(ViewConcern::Operation); // no build yet
	touch(ViewConcern::Output);
	return true;
}

void ProjectSession::close_project() {
	graph_->clear();
	assets_->clear();
	render_check_->clear();
	validation_cache_ = ValidationCache();
	document_findings_.clear();
	validation_due_ = false;
	if (!view_.project_open) return;
	// Its operation goes with it (a project switch and Quit cancelled it at the gate already): a
	// build is cancelled, and a Play waiting on it.
	cancel_operation(false);
	const std::string title = view_.document.title;
	// What belongs to the project goes with it: its documents, their selections, a prompt
	// waiting on them (an answer to it afterwards is refused: nothing waits), and the boot
	// report of the game started in it (a later line of that game's log is ignored; the
	// next project opens with none, even when it is this one again).
	documents_.clear();
	remembered_.clear();
	stale_.clear();
	conflicts_.clear();
	if (pending_request_) close_unsaved_prompt();
	view_.boot_missing.clear();
	boot_project_.clear();
	play_findings_.clear();
	activate(std::string());
	view_.clipboard.clear();
	view_.reveal_file.clear();
	view_.reveal_file_rename = false;
	update_document_view();
	view_.project_open = false;
	view_.import_preview = SessionView::ImportPreview();
	view_.imports.clear();
	view_.retail_files.clear();
	view_.project_root.clear();
	view_.document = ProjectDocument();
	view_.scan = AssetScan();
	view_.requirements = RequirementReport();
	view_.diagnostics.clear();
	view_.has_build = false;
	view_.last_build = BuildReport();
	// The last build's findings are this project's and go with it.
	build_findings_.clear();
	paths_ = ProjectPaths();
	local_ = LocalSettings();
	view_.runtime_executable = resolve_runtime_executable();
	view_.retail_directory = game_install();
	note("Closed " + title + ".");
	view_.status = "No project open.";
	// What the project was goes with it: every concern of the view moves.
	for (size_t concern = 0; concern < kViewConcernCount; ++concern)
		touch(static_cast<ViewConcern>(concern));
}

// Re-read the project's files and re-evaluate the checklist through the engine's one
// refresh (project/project_state.h: import, scan, requirements), the same the command
// line runs; the project findings replace the last action's.
ImportRunResult ProjectSession::refresh(bool force_import, const std::string &only) {
	ProjectState state = refresh_project_state(paths_, view_.document, force_import, only);
	view_.imports = state.imports.sources;
	for (const ImportedSource &source : state.imports.sources)
		if (source.reimported) note("Imported " + source.source + " (" + std::to_string(source.outputs.size()) + " file" +
		                            (source.outputs.size() == 1 ? "" : "s") + ")");
	view_.scan = std::move(state.scan);
	assets_->set_scan(paths_.root, view_.scan, view_.document.target_game);
	view_.requirements = std::move(state.requirements);
	touch(ViewConcern::Files);
	validate_documents();
	return std::move(state.imports);
}

// The settings a request names that differ from those in effect, written: the project's
// (its name and features) to project.opennova and the editor's (the game install, the
// runtime, Play in the game install) to its settings file. Each file is written from a
// copy and its values take effect once it is written, so a setting that failed is still
// the one in effect and a retry writes it again, while one that was written is compared
// with from then on. The result (the view's settings_result, under the request's serial)
// lists what could not be written; each is also a finding.
void ProjectSession::apply_project_settings(const ProjectSettingsChange &change) {
	std::vector<Diagnostic> failures;
	ProjectDocument project = view_.document;
	bool project_changed = false, features_changed = false;
	if (change.title && *change.title != project.title) {
		if (change.title->empty()) {
			failures.push_back(make_diagnostic(DiagnosticSeverity::Error, "project.title_empty", "A project needs a name."));
		} else {
			project.title = *change.title;
			project_changed = true;
		}
	}
	if (change.mission && *change.mission != project.features.mission) {
		project.features.mission = *change.mission;
		project_changed = features_changed = true;
	}
	if (change.multiplayer && *change.multiplayer != project.features.multiplayer) {
		project.features.multiplayer = *change.multiplayer;
		project_changed = features_changed = true;
	}
	// The requirements follow the features: their change reads the files again (the import pass,
	// the scan), which an operation holding them (a build packing them) must not see change.
	if (features_changed && busy_for(EditorRequestKind::Rescan)) {
		failures.push_back(make_diagnostic(DiagnosticSeverity::Warning, "operation.busy",
		                                   "Wait for the build to finish, or cancel it, before changing the "
		                                   "project's features."));
		project.features = view_.document.features;
		features_changed = false;
		project_changed = project.title != view_.document.title;
	}
	if (project_changed && !view_.project_open) {
		failures.push_back(make_diagnostic(DiagnosticSeverity::Error, "project.none",
		                                   "Open a project to change its name or its features."));
		project_changed = features_changed = false;
	}
	if (project_changed) {
		Diagnostic error;
		if (save_project_document(paths_.project_file, project, error)) {
			view_.document = project;
			if (features_changed) refresh(); // the requirements follow the features
		} else {
			failures.push_back(error);
			project_changed = false;
		}
	}
	// The game install is the open project's (ADR 0046 d6/d10), written to its local.json,
	// which opennova-project reads too; the editor's machine setting keeps the install last
	// chosen, where a project that names none starts. Both keep it absolute: a relative path
	// is taken from the editor's working directory, not from wherever the command line runs.
	const std::optional<std::string> install =
	        change.retail_directory ? std::optional<std::string>(absolute_install_path(*change.retail_directory))
	                                : std::nullopt;
	bool install_changed = false;
	if (install && view_.project_open && *install != local_.retail_root) {
		LocalSettings local = local_;
		local.retail_root = *install;
		Diagnostic error;
		if (save_local_settings(paths_, local, error)) {
			local_ = std::move(local);
			install_changed = true;
		} else {
			failures.push_back(error);
		}
	}
	EditorSettings editor = settings_;
	if (install) editor.retail_directory = *install;
	if (change.runtime_executable) editor.runtime_executable = *change.runtime_executable;
	if (change.play_retail) editor.play_retail = *change.play_retail;
	bool editor_changed = editor.retail_directory != settings_.retail_directory ||
	                      editor.runtime_executable != settings_.runtime_executable ||
	                      editor.play_retail != settings_.play_retail;
	if (editor_changed) {
		Diagnostic error;
		if (::opennova::editor::save_editor_settings(settings_path_, editor, error)) {
			settings_ = editor;
			view_.play_retail = settings_.play_retail;
			view_.runtime_setting = settings_.runtime_executable;
			view_.runtime_executable = resolve_runtime_executable();
		} else {
			failures.push_back(error);
			editor_changed = false;
		}
	}
	if (game_install() != view_.retail_directory) {
		view_.retail_directory = game_install();
		touch(ViewConcern::Preferences);
		refresh_retail_files();
	}
	for (const Diagnostic &failure : failures) report(failure);
	view_.settings_result = {change.serial, failures};
	view_.status = !failures.empty()                                       ? "A setting could not be saved: see Problems."
	               : project_changed || install_changed || editor_changed ? "Saved the settings."
	                                                                      : "No setting changed.";
	if (project_changed) touch(ViewConcern::Project);
	if (install_changed || editor_changed) touch(ViewConcern::Preferences);
	touch(ViewConcern::Dialogs);
	touch(ViewConcern::Output);
}

// The required files `roles` names, made from their factories. The checklist the request
// was raised from may be older than the tree: it is evaluated again over the files as they
// are now, so a file that has appeared since is refused (create_missing.exists), never
// overwritten.
void ProjectSession::create_missing(const std::vector<std::string> &roles) {
	if (roles.empty()) {
		view_.status = "Nothing to create.";
		touch(ViewConcern::Output);
		return;
	}
	const RequirementReport now = evaluate_requirements(view_.document, scan_project_assets(paths_, view_.document));
	const CreateMissingResult result = create_missing_requirements(paths_, view_.document, now, roles);
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
	touch(ViewConcern::Output);
}

// Play refused before any build: where nothing can be spawned, and while a game runs (a second
// one would fight it for its files). True when refused, said why.
bool ProjectSession::play_refused() {
	if (!platform_.can_spawn()) {
		report(make_diagnostic(DiagnosticSeverity::Error, "play.unsupported",
		                       "Play is Windows-only for now: the editor cannot start the game on this system. "
		                       "Build works here."));
		return true;
	}
	if (play_.state() != PlayState::Stopped) {
		report(make_diagnostic(DiagnosticSeverity::Error, "play.already_running",
		                       "The game is already running; stop it before starting it again."));
		return true;
	}
	return false;
}

// The build as an operation (BuildOperation): planned here from the Problems rows as they are,
// then stepped by the polls and landed by the one that sees it done (absorb_build). Unsaved
// edits never reach here: Build and Play wait on the unsaved prompt first (guard_unsaved), whose
// Save writes them. A build running already served the request at the busy gate (it joined).
void ProjectSession::start_build(bool then_play) {
	if (then_play && play_refused()) return;
	build_findings_.clear(); // the last build's rows go: this one reports anew
	reload_changed_documents();
	refresh();
	// The plan gates on the findings the refresh above just produced (the Problems rows),
	// not on a validation of its own; the build's own findings are those its report adds to
	// these rows (absorb_build), whatever the rows are when it ends.
	const BuildPlan plan = plan_build(paths_, view_.scan, view_.requirements, document_findings_);
	// No directory a game runs from is pruned: this editor's game's, and every one whose lease
	// names a process that still runs (a game left running across an editor restart); a lease
	// whose game is gone is deleted (run/play_lease.h).
	const std::string output_root = paths_.build_dir + "/play";
	std::vector<std::string> protected_dirs =
	        live_leased_dirs(output_root, platform_, play_.state() != PlayState::Stopped ? play_.pid() : -1);
	if (!play_.running_build_dir().empty()) protected_dirs.push_back(play_.running_build_dir());
	// The build lands under the cache, which keeps itself out of the modder's repository;
	// a cache that cannot be made fails the build's own first step, which says why.
	std::string cache_error;
	ensure_project_cache_dir(paths_, cache_error);
	const uint64_t id = operations_.start(std::make_unique<BuildOperation>(
	        plan, output_root, std::move(protected_dirs), view_.diagnostics, then_play));
	if (id == 0) return refuse_busy(std::string()); // another operation runs, holding nothing it needs
	outcome_.operation = id;
	view_.status = then_play ? "Building, then playing..." : "Building...";
	note("Build started.");
	show_operation();
}

OperationOutcome ProjectSession::absorb_build(const BuildReport &result, const std::vector<Diagnostic> &gate,
                                              bool then_play) {
	view_.has_build = true;
	view_.last_build = result;
	// A blocked build's report repeats the findings that blocked it, which were Problems rows
	// when it started: only the ones those rows lacked (the plan's own, the build's) are the
	// build's, whatever an edit made of the rows while it packed, and every validation keeps
	// them until the next build starts or the project closes.
	std::vector<Diagnostic> own;
	for (const Diagnostic &d : result.diagnostics)
		if (std::none_of(gate.begin(), gate.end(), [&d](const Diagnostic &gated) { return same_finding(gated, d); }))
			own.push_back(d);
	for (const Diagnostic &d : own) report(d);
	build_findings_ = own;
	if (result.ok) {
		if (result.reused_existing) {
			note("Build unchanged: " + shown_path(result.build_dir, paths_.root));
			view_.status = "Build unchanged.";
		} else {
			note("Built " + shown_path(result.build_dir, paths_.root) + " (" + std::to_string(result.archives_written.size()) +
			     " archive(s) written, " + std::to_string(result.archives_reused.size()) + " reused, " +
			     std::to_string(result.loose_written.size()) + " loose file(s))");
			view_.status = "Build finished.";
		}
	} else {
		note("Build failed.");
		view_.status = "Build failed; see Problems.";
	}
	if (result.ok && then_play) start_play();
	touch(ViewConcern::Operation);
	OperationOutcome outcome;
	outcome.end = result.ok ? OperationEnd::Done : OperationEnd::Failed;
	outcome.findings = std::move(own);
	return outcome;
}

// The game install the editor imports from and plays in: the open project's (its
// local.json, as opennova-project reads it), else the one the editor last chose.
std::string ProjectSession::game_install() const {
	return view_.project_open ? local_.retail_root : settings_.retail_directory;
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
	// The last run's boot report and exit go, their rows with them (the next validation would
	// make none); the game started now reports on this project.
	view_.boot_missing.clear();
	play_findings_.clear();
	const size_t rows = view_.diagnostics.size();
	view_.diagnostics.erase(std::remove_if(view_.diagnostics.begin(), view_.diagnostics.end(),
	                                       [](const Diagnostic &d) {
		                                       return d.code == "play.boot_missing" || d.code == "play.crashed";
	                                       }),
	                        view_.diagnostics.end());
	touch(ViewConcern::Run);
	if (view_.diagnostics.size() != rows) touch(ViewConcern::Findings);
	boot_project_ = view_.project_root;
	if (settings_.play_retail) {
		if (!prepare_retail_launch_plan(game_install(), build_dir, plan, error)) {
			report(error);
			view_.status = "The game install could not be prepared; see Problems.";
			touch(ViewConcern::Output);
			return;
		}
	} else {
		const std::string executable = resolve_runtime_executable();
		if (executable.empty() || !fs::is_regular_file(executable, ec)) {
			report(make_diagnostic(DiagnosticSeverity::Error, "play.runtime_missing",
			                       executable.empty()
			                               ? "No game runtime is set; choose opennova.exe in File > Project settings..."
			                               : "The game runtime was not found: " + executable));
			view_.status = "The game runtime was not found.";
			touch(ViewConcern::Output);
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
		touch(ViewConcern::Output);
		return;
	}
	// The game's lease on the directory it runs from: a build leaves it alone while the game
	// runs, this editor's and one started after the editor restarts (run/play_lease.h).
	std::string lease_error;
	if (!write_play_lease({plan.build_dir, play_.pid(), plan.executable}, lease_error))
		note("The game's lease could not be written (" + lease_error +
		     "): a build after the editor restarts may remove its files while it runs.");
	view_.play_state = play_.state();
	view_.play_pid = play_.pid();
	view_.play_mcp_port = plan.mcp_port;
	view_.play_command_line = launch_plan_command_line(plan);
	view_.play_exited_on_its_own = false;
	view_.play_exit_code = -1;
	note("Running: " + view_.play_command_line);
	view_.status = settings_.play_retail ? "Game install running." : "Game running.";
	touch(ViewConcern::Run);
}

void ProjectSession::stop_play() {
	if (play_.state() != PlayState::Running) return;
	play_.stop();
	view_.play_state = play_.state();
	view_.status = "Stopping the game...";
	note("Stop requested.");
	touch(ViewConcern::Run);
}

void ProjectSession::note(std::string line) {
	view_.output.append(std::move(line));
	touch(ViewConcern::Output);
}

void ProjectSession::report(const Diagnostic &d) {
	// A validation an edit left due runs first, so it cannot replace this row.
	validate_pending();
	view_.diagnostics.push_back(d);
	touch(ViewConcern::Findings);
	record_outcome(d);
	note(std::string(diagnostic_severity_label(d.severity)) + ": " + d.message);
}

// A finding raised while serving a request is that request's outcome; one raised by a
// poll (the build finishing, the game's log) belongs to no request.
void ProjectSession::record_outcome(const Diagnostic &d) {
	if (handling_ == 0) return;
	outcome_.findings.push_back(d);
	if (d.severity == DiagnosticSeverity::Error) outcome_.refused = true;
}

// A request that cannot run now (a build is packing the project's files, the document it
// names is not open): nothing is wrong with the project, so the row is a warning, but the
// request did nothing and its outcome says so.
void ProjectSession::refuse_now(const char *code, const std::string &message, const std::string &asset) {
	report(make_diagnostic(DiagnosticSeverity::Warning, code, message, asset));
	if (handling_ > 0) outcome_.refused = true;
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
		absorb_boot_report(line);
		start = nl + 1;
	}
	game_log_partial_.erase(0, start);
	// The names the lines reported become their rows in one validation.
	validate_pending();
}

// The runtime names each boot-required file it could not find, one line per file
// (`BootRootMount: <kBootResourceMissingMarker><name> ...`, godot/game/boot_root_mount.gd
// over the witnessed manifest). Each name is recorded once, and every
// validation makes its Problems row (validate_documents) until Play starts again or the
// project closes. The report belongs to the project the game was started in: a line read
// after that project closed, or while another is open, is ignored.
void ProjectSession::absorb_boot_report(const std::string &line) {
	const std::string marker = gameprofile::kBootResourceMissingMarker;
	const size_t at = line.find(marker);
	if (at == std::string::npos) return;
	const size_t start = at + marker.size();
	size_t end = start;
	while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
	const std::string name = line.substr(start, end - start);
	if (name.empty()) return;
	if (!view_.project_open || view_.project_root != boot_project_) return;
	if (view_.missing_at_boot(name)) return;
	view_.boot_missing.push_back(name);
	touch(ViewConcern::Run);
	validate_later();
}

// How the game ended, on the poll that saw it end: stopped, quit (exit code 0, or one the
// platform could not read), or any other code: a crash or an error exit, said in Output with
// its code and a Problems row (play.crashed) that stays, like the boot report, until Play
// starts again or the project closes (a game of a project closed since reports nothing).
void ProjectSession::absorb_play_exit() {
	remove_play_lease(play_.plan().build_dir); // the game is gone: its directory is a build like any
	view_.play_exited_on_its_own = play_.exited_on_its_own();
	view_.play_exit_code = play_.exit_code();
	std::string line = view_.play_exited_on_its_own ? "The game exited." : "The game was stopped.";
	if (view_.play_exited_on_its_own && view_.play_exit_code > 0) {
		std::string code = std::to_string(view_.play_exit_code);
		if (view_.play_exit_code > 0xFFFF) {
			char hex[16];
			std::snprintf(hex, sizeof(hex), "0x%08llX", static_cast<unsigned long long>(view_.play_exit_code));
			code += std::string(" (") + hex + ")";
		}
		line = "The game exited with code " + code + ".";
		if (view_.project_open && view_.project_root == boot_project_) {
			play_findings_ = {make_diagnostic(DiagnosticSeverity::Error, "play.crashed",
			                                  "The game ended with exit code " + code +
			                                          ": it crashed or stopped on an error. Its log is in Output.")};
			validate_later();
		}
	}
	note(line);
	view_.status = line;
}

void ProjectSession::save_editor_settings() {
	Diagnostic error;
	if (!::opennova::editor::save_editor_settings(settings_path_, settings_, error)) report(error);
	view_.recent_projects = settings_.recent_projects;
	view_.retail_directory = game_install();
	view_.play_retail = settings_.play_retail;
	view_.import_dependencies = settings_.import_dependencies;
	touch(ViewConcern::Preferences);
}


Document *ProjectSession::document_for(const std::string &path) {
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
	assets_->set_open(view_.documents);
	touch(ViewConcern::Documents);
	// Which documents are open, each as read and whether it has unsaved edits: an edit that
	// leaves its document as unsaved as it was moves Documents alone.
	std::vector<std::pair<uint64_t, bool>> set;
	for (const auto &document : documents_)
		set.emplace_back(document->identity(), document->dirty());
	if (set != document_set_) {
		document_set_ = std::move(set);
		touch(ViewConcern::DocumentSet);
	}
}

// The project's findings now, composed as `opennova-project validate` composes them
// (project/project_findings): the scan's, the requirements', the files the last Play's game
// reported missing and its nonzero exit, every document type's over the files (the open
// documents standing in for theirs, the closed ones from the cache) with the graph's and the
// open documents' own (a file changed outside the editor that was not read again), the menu
// render check's notes and the last build's own findings (after the gate the build reads: a
// note never blocks a build, nor does the last Play's report, which only the next Play can
// clear, nor the last build's, which the next build replaces). They replace the Problems rows:
// Findings moves only when they differ, and Graph only when the graph's update changed it.
void ProjectSession::validate_documents() {
	validation_due_ = false;
	const uint64_t graph_generation = graph_->generation();
	const std::vector<Diagnostic> open = open_document_findings();
	ProjectFindings findings = compose_project_findings(
	        {paths_, view_.document, view_.scan, view_.requirements, view_.documents, view_.boot_missing, play_findings_,
	         open, build_findings_},
	        *graph_, validation_cache_, *render_check_, *assets_);
	document_findings_ = std::move(findings.documents);
	if (findings.rows != view_.diagnostics) {
		view_.diagnostics = std::move(findings.rows);
		touch(ViewConcern::Findings);
	}
	if (graph_->generation() != graph_generation) touch(ViewConcern::Graph);
}

// An edit's validation, left for validate_pending: the request's return from outside, a
// pump's poll, or the next finding reported. Nothing in the view moves until it runs.
void ProjectSession::validate_later() {
	validation_due_ = true;
}

void ProjectSession::validate_pending() {
	if (validation_due_) validate_documents();
}

// A document of its kind's type read from the project's file; null, with `error`, when
// the file does not load.
std::shared_ptr<Document> ProjectSession::load_document(const std::string &relative, AssetKind kind,
                                                        Diagnostic &error) const {
	const DocumentType *type = document_type_for(kind);
	if (!type) {
		error = make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This kind of file has no editor yet.", relative);
		return nullptr;
	}
	std::shared_ptr<Document> document = type->make();
	if (!document->load((fs::path(paths_.root) / relative).generic_string(), relative, kind, view_.document.target_game,
	                    error))
		return nullptr;
	return document;
}

// The open documents against their files (Rescan, an import, Build and Play: the build
// packs the files on disk, and its gate is the validation in which an open document stands
// in for its file). A document whose file holds what it was read from or last saved keeps
// its records, its history and its selection. A clean one whose file changed outside the
// editor is read again (its selection goes with its old records); one whose file no longer
// reads (or is gone) stays open as it was, and why is its Problems row (document.stale, an
// error: the build would pack the file that does not read). One with unsaved edits whose
// file changed keeps them, and a warning says so (document.conflict: its Save is refused)
// with a Reload fix, which asks about the edits first.
void ProjectSession::reload_changed_documents() {
	bool changed = false;
	for (auto &document : documents_) {
		const std::string path = document->path();
		if (document->matches_file()) {
			forget_file_state(path);
			continue;
		}
		if (document->dirty()) {
			stale_.erase(path);
			conflicts_.insert(path);
			continue;
		}
		Diagnostic error;
		std::shared_ptr<Document> loaded = load_document(path, document->kind(), error);
		if (!loaded) {
			if (!stale_.count(path))
				note("Kept " + path + " as it was: it changed outside the editor and could not be read again.");
			stale_[path] = error;
			continue;
		}
		forget_file_state(path);
		remembered_.erase(path); // read again: its records' identities are gone
		if (path == view_.active_document) view_.select_only({});
		document = loaded;
		changed = true;
		note("Reloaded " + path + ": it changed outside the editor.");
	}
	if (!changed) return;
	select_first_screen(); // the active menu read again shows its first screen
	touch(ViewConcern::Selection);
	update_document_view();
}

// The Problems rows of the open documents whose file changed outside the editor and was not
// read again (stale_, conflicts_).
std::vector<Diagnostic> ProjectSession::open_document_findings() const {
	std::vector<Diagnostic> findings;
	for (const auto &[path, reason] : stale_) {
		Diagnostic d = make_diagnostic(DiagnosticSeverity::Error, "document.stale",
		                               "This file changed outside the editor and could not be read again, so the editor "
		                               "shows it as it was: " + reason.message + " Correct the file and Refresh, or close it.",
		                               path);
		d.line = reason.line;
		findings.push_back(std::move(d));
	}
	for (const std::string &path : conflicts_)
		findings.push_back(make_diagnostic(DiagnosticSeverity::Warning, "document.conflict",
		                                   "This file changed outside the editor while it has unsaved edits: Save is "
		                                   "refused until it is read again, which discards the edits.",
		                                   path));
	return findings;
}

// The document at `path` was read again, saved or closed: whatever its file did outside the
// editor is no longer a row.
void ProjectSession::forget_file_state(const std::string &path) {
	stale_.erase(path);
	conflicts_.erase(path);
}

// A file renamed with every reference rewritten (graph/rename_transaction), or refused
// with the reasons as findings; the rewritten documents that are open reload. A rename that
// would rewrite a file with unsaved edits, or leave them behind on the old name, never
// reaches here with them: it waits on the unsaved prompt first (guard_unsaved), whose Save
// writes them.
void ProjectSession::rename_asset(const std::string &file, const std::string &new_name) {
	if (!view_.project_open) return;
	// The plan reads the graph: an edit a held pump made first reaches it (and the
	// Problems rows), so a reference it added is planned or refused like any other.
	validate_pending();
	const RenamePlan plan = plan_rename(paths_, view_.scan, *graph_, file, new_name);
	if (!plan.ok()) {
		for (const Diagnostic &d : plan.refusals) report(d);
		view_.status = "The rename was refused.";
		touch(ViewConcern::Output);
		return;
	}
	std::vector<Diagnostic> findings;
	const bool ok = apply_rename(paths_, view_.document, view_.scan, *graph_, plan, findings);
	std::vector<std::string> reload;
	for (const RenameSite &site : plan.sites)
		if (document_for(site.file) && std::find(reload.begin(), reload.end(), site.file) == reload.end()) reload.push_back(site.file);
	// The document the modder was in stays active through the closes and reloads below
	// (the renamed file's own document follows it to the new name). A reloaded document
	// holds new records, so only an untouched one keeps its selection.
	std::string active = view_.active_document;
	const NodeAddress selection = view_.selection;
	const std::vector<NodeAddress> selected = view_.selected;
	bool keep_selection = std::find(reload.begin(), reload.end(), active) == reload.end();
	// A refused commit leaves the file where it was: its open document stays.
	Document *renamed = ok ? document_for(plan.path) : nullptr;
	if (renamed) {
		const bool was_active = renamed->path() == active;
		handle(make_request(EditorRequestKind::CloseDocument, renamed->path()));
		refresh();
		handle(make_request(EditorRequestKind::OpenDocument, plan.new_path));
		if (was_active) {
			active = view_.active_document;
			keep_selection = false;
		}
	}
	refresh();
	for (const std::string &path : reload) handle(make_request(EditorRequestKind::ReloadDocument, path));
	bool active_open = active.empty();
	for (const auto &document : documents_) active_open = active_open || document->path() == active;
	if (active_open) {
		activate(active);
		view_.select_only(keep_selection ? selection : NodeAddress());
		if (keep_selection) view_.selected = selected;
		select_first_screen();
	}
	// Reported last: the refresh and the reloads above rebuild the Problems rows.
	for (const Diagnostic &d : findings) report(d);
	if (ok) {
		note("Renamed " + plan.old_name + " to " + plan.new_name + " (" + std::to_string(plan.sites.size()) +
		     " reference" + (plan.sites.size() == 1 ? "" : "s") + " rewritten)");
		view_.status = "Renamed " + plan.old_name + " to " + plan.new_name + ".";
	} else {
		view_.status = "The rename did not finish.";
	}
	touch(ViewConcern::Selection); // the active document and its selection, kept or started over
	touch(ViewConcern::Output);
}

// Whether the project file at `path`, as saved, has a use that reaches exactly the renamed
// definition: the one the graph's lookup returns for it (users_of's rule), never a same-named
// definition another file shadows or another scope holds.
bool ProjectSession::saved_file_uses(const std::string &path, const SymbolRenamePlan &plan) const {
	const GraphSymbol *renamed = graph_->symbol_at(plan.file, plan.locator, plan.field);
	const AssetEntry *asset = project_file(path);
	Extracted saved;
	Diagnostic error;
	if (!renamed || renamed->inert || !asset || !extract_from_asset(paths_, view_.document, *asset, saved, error)) return false;
	for (const GraphEdge &edge : saved.edges)
		if (edge.kind == plan.kind && graph_->resolve_symbol(edge.kind, edge.value, edge.scope) == renamed) return true;
	return false;
}

// The name a rename request gives: a text, or a number typed as one (an item id).
static std::string new_name_of(const EditorRequest &request) {
	if (const auto *text = std::get_if<std::string>(&request.edit.value)) return *text;
	if (const auto *number = std::get_if<int64_t>(&request.edit.value)) return std::to_string(*number);
	return std::string();
}

SymbolRenamePlan ProjectSession::plan_symbol(const EditorRequest &request) {
	// The plan reads the graph: an edit a held pump made first reaches it.
	validate_pending();
	const GraphSymbol *symbol = graph_->symbol_at(request.path, request.text, request.edit.field);
	if (!symbol) {
		SymbolRenamePlan none;
		none.file = request.path;
		none.new_name = new_name_of(request);
		none.refusals.push_back(make_diagnostic(DiagnosticSeverity::Error, "rename.unknown_symbol",
		                                        request.path + " defines no name in field " + request.edit.field +
		                                                " of the record at " + request.text + ".",
		                                        request.path, request.edit.field));
		return none;
	}
	return plan_symbol_rename_project(view_.scan, *graph_, *symbol, new_name_of(request));
}

// What a rename would do, planned into the view (nothing written, nothing reported): a name's
// rename everywhere, or a file's (edit.field "").
void ProjectSession::preview_rename(const EditorRequest &request) {
	if (!view_.project_open) return;
	SessionView::RenamePreview preview;
	preview.serial = view_.rename_preview.serial + 1;
	preview.ask_serial = view_.rename_preview.ask_serial + (request.flag ? 1 : 0);
	preview.requested = new_name_of(request);
	if (request.edit.field.empty()) {
		validate_pending();
		const RenamePlan plan = plan_rename(paths_, view_.scan, *graph_, request.path, new_name_of(request));
		preview.path = plan.path.empty() ? request.path : plan.path;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.sites = plan.sites;
		preview.refusals = plan.refusals;
	} else {
		SymbolRenamePlan plan = plan_symbol(request);
		// What each file's own type makes of it, as the commit would (open documents as they
		// stand): a site its document refuses is a refusal here too.
		if (plan.ok()) {
			std::vector<std::shared_ptr<const Document>> open(documents_.begin(), documents_.end());
			check_symbol_rename(paths_, view_.document, view_.scan, *graph_, plan, open, plan.refusals);
		}
		preview.symbol = true;
		preview.kind = plan.kind;
		preview.path = plan.file;
		preview.locator = request.text;
		preview.field = request.edit.field;
		preview.old_name = plan.old_name;
		preview.new_name = plan.new_name;
		preview.sites = plan.sites;
		preview.refusals = plan.refusals;
	}
	view_.rename_preview = std::move(preview);
	touch(ViewConcern::Dialogs);
}

// A name renamed everywhere (graph/rename_transaction), or refused with the reasons as
// findings; the rewritten documents that are open reload. One that would rewrite a file with
// unsaved edits never reaches here with them: it waits on the unsaved prompt first
// (guard_unsaved), whose Save writes them.
void ProjectSession::rename_symbol(const EditorRequest &request) {
	if (!view_.project_open) return;
	const SymbolRenamePlan plan = plan_symbol(request);
	if (!plan.ok()) {
		for (const Diagnostic &d : plan.refusals) report(d);
		view_.status = "The rename was refused.";
		touch(ViewConcern::Output);
		return;
	}
	std::vector<Diagnostic> findings;
	const bool ok = apply_symbol_rename(paths_, view_.document, view_.scan, *graph_, plan, findings);
	std::vector<std::string> reload;
	for (const RenameSite &site : plan.sites)
		if (document_for(site.file) && std::find(reload.begin(), reload.end(), site.file) == reload.end()) reload.push_back(site.file);
	// The document the modder was in stays active; one read again holds new records, so only an
	// untouched one keeps its selection.
	const std::string active = view_.active_document;
	const NodeAddress selection = view_.selection;
	const std::vector<NodeAddress> selected = view_.selected;
	const bool keep_selection = std::find(reload.begin(), reload.end(), active) == reload.end();
	refresh();
	for (const std::string &path : reload) handle(make_request(EditorRequestKind::ReloadDocument, path));
	if (!active.empty() && document_for(active)) {
		activate(active);
		view_.select_only(keep_selection ? selection : NodeAddress());
		if (keep_selection) view_.selected = selected;
		// The renamed definition selected again where it was, its field shown.
		if (!keep_selection && active == plan.file)
			if (Document *defining = document_for(active)) view_.select_only(defining->address_at(plan.locator));
		select_first_screen();
	}
	for (const Diagnostic &d : findings) report(d);
	if (ok) {
		const size_t uses = plan.sites.size() - 1;
		note("Renamed " + std::string(reference_row(plan.kind).phrase) + " " + plan.old_name + " to " + plan.new_name + " (" +
		     std::to_string(uses) + " use" + (uses == 1 ? "" : "s") + " rewritten)");
		view_.status = "Renamed " + plan.old_name + " to " + plan.new_name + " everywhere.";
	} else {
		view_.status = "The rename did not finish.";
	}
	touch(ViewConcern::Selection); // the active document and its selection, kept or started over
	touch(ViewConcern::Output);
}

// The refresh with the import pass forced over one source (or all): every stale source
// imports too, as on any refresh; `force` imports the named one even when unchanged (a
// changed importer, a wanted rebuild).
void ProjectSession::reimport(const std::string &source, bool force) {
	if (!view_.project_open) return;
	const ImportRunResult imported = refresh(force, source);
	// The pass's findings are Problems rows already (they ride the scan); the ones on the
	// sources asked for are also this request's outcome.
	std::vector<std::string> asked;
	for (const ImportedSource &ran : imported.sources)
		if (import_source_named(source, ran.source)) {
			asked.push_back(ran.source);
			asked.push_back(ran.sidecar);
		}
	for (const Diagnostic &d : imported.diagnostics)
		if (std::find(asked.begin(), asked.end(), d.asset) != asked.end()) record_outcome(d);
	view_.status = std::to_string(imported.reimported) + " source" + (imported.reimported == 1 ? "" : "s") + " imported.";
	touch(ViewConcern::Output);
}

// The import dialog on `roots` chosen among `choices` (each file once), planned with the
// files they need when `with_dependencies`: open while it has something to show, a list to
// choose from or a file chosen.
void ProjectSession::preview_import(std::vector<ImportSource> choices, std::vector<ImportSource> roots,
                                    bool with_dependencies) {
	SessionView::ImportPreview &preview = view_.import_preview;
	preview.choices = std::move(choices);
	preview.roots.clear();
	for (ImportSource &root : roots)
		if (std::find(preview.roots.begin(), preview.roots.end(), root) == preview.roots.end())
			preview.roots.push_back(std::move(root));
	preview.with_dependencies = with_dependencies;
	preview.open = !preview.choices.empty() || !preview.roots.empty();
	preview.changed = false;
	plan_preview();
}

// The open preview's plan, made from its roots as the files are now: the project read again
// (the view's scan may be older than a change made outside the editor; a scan writes
// nothing, unlike a refresh, which runs the import pass), resolved by a copy of the asset
// graph brought up to it (its cache reads again only the files that changed); its findings
// are the dialog's to show.
void ProjectSession::plan_preview() {
	SessionView::ImportPreview &preview = view_.import_preview;
	validate_pending();
	if (preview.open) {
		const AssetScan scan = scan_project_assets(paths_, view_.document);
		AssetGraph graph = *graph_;
		graph.update(paths_, view_.document, scan, view_.documents);
		preview.plan = plan_import(preview.roots, preview.with_dependencies, paths_, view_.document, scan, graph,
		                           game_install());
	} else {
		preview.plan = ImportPlan();
	}
	preview.serial = ++import_serial_;
	if (preview.open) {
		size_t files = 0, found = 0, missing = 0;
		for (const ImportPlanRow &row : preview.plan.rows) {
			if (row.state == ImportPlanRow::State::NotFound) ++missing;
			else if (row.selected) ++files;
			if (row.state == ImportPlanRow::State::Found) ++found;
		}
		view_.status = preview.roots.empty() ? std::string("Choose the files to import.")
		               : "Import preview: " + std::to_string(files) + " file" + (files == 1 ? "" : "s") + " to import" +
		                         (preview.with_dependencies ? " (" + std::to_string(found) + " the chosen ones need), " +
		                                                              std::to_string(missing) + " not found."
		                                                    : std::string("."));
	}
	touch(ViewConcern::Dialogs);
	if (preview.open) touch(ViewConcern::Output);
}

// The import dialog's "Include the files these need": the editor's setting, written from a
// copy (a setting that could not be written stays the one in effect, its failure a finding);
// an open preview is planned again with the flag asked for.
void ProjectSession::set_import_dependencies(bool flag) {
	if (flag != settings_.import_dependencies) {
		EditorSettings editor = settings_;
		editor.import_dependencies = flag;
		Diagnostic error;
		if (::opennova::editor::save_editor_settings(settings_path_, editor, error))
			settings_ = editor;
		else
			report(error);
	}
	view_.import_dependencies = settings_.import_dependencies;
	view_.status = flag ? "Imports bring the files the chosen ones need." : "Imports take the chosen files alone.";
	SessionView::ImportPreview &preview = view_.import_preview;
	if (preview.open && preview.with_dependencies != flag) {
		preview.with_dependencies = flag;
		preview.changed = false;
		plan_preview();
	}
	touch(ViewConcern::Preferences);
	touch(ViewConcern::Output);
}

// The rows kept, written the whole selection or none of it as far as the disk allows
// (import_assets), then one refresh. With a preview open the files are planned again first:
// when that is not the import the preview showed (a dependency new or gone, a file found in
// another place, a file that no longer reads), nothing is written and the dialog shows the
// new plan with a line saying so; a row the plan does not have is refused. An import that
// would write over a file with unsaved edits never reaches here: it waits on the unsaved
// prompt first (guard_unsaved), whose Save writes them; the rescan after it reads again
// the open documents whose files it replaced.
void ProjectSession::import_files(const EditorRequest &request) {
	if (!view_.project_open) return;
	SessionView::ImportPreview &preview = view_.import_preview;
	if (preview.open) {
		const ImportPlan shown = preview.plan;
		plan_preview();
		if (!same_import(shown, preview.plan)) {
			preview.changed = true;
			view_.status = "The files changed since the preview: nothing was imported.";
			touch(ViewConcern::Dialogs);
			touch(ViewConcern::Output);
			return refuse_now("import.changed",
			                  "The files changed since the preview: nothing was imported. Check the import again.");
		}
		for (const ImportSource &import : request.imports) {
			const bool planned = std::any_of(preview.plan.rows.begin(), preview.plan.rows.end(), [&import](const ImportPlanRow &row) {
				return row.state != ImportPlanRow::State::NotFound && row.source == import;
			});
			if (!planned)
				return report(make_diagnostic(DiagnosticSeverity::Error, "import.not_planned",
				                              import.name() + " is not in the import preview: plan it first.", import.name()));
		}
	}
	view_.import_preview = SessionView::ImportPreview();
	if (request.imports.empty()) {
		view_.status = "Nothing to import.";
		touch(ViewConcern::Dialogs);
		touch(ViewConcern::Output);
		return;
	}
	const ImportResult imported = import_assets(request.imports, paths_, view_.document, request.flag);
	if (!imported.imported.empty()) handle(make_request(EditorRequestKind::Rescan));
	for (const auto &path : imported.imported) note("Imported " + path);
	for (const auto &path : imported.not_imported) note("Not imported " + path);
	for (const auto &d : imported.diagnostics) report(d);
	const size_t done = imported.imported.size();
	view_.status = !imported.not_imported.empty()
	                       ? std::to_string(done) + " of " + std::to_string(done + imported.not_imported.size()) +
	                                 " files imported: the import stopped at " + imported.not_imported.front() + "."
	                       : std::to_string(done) + " file(s) imported.";
	touch(ViewConcern::Dialogs); // the preview closed
	touch(ViewConcern::Output);
}

// The game install's file names, for the Import fixes (problem_fixes.h).
void ProjectSession::refresh_retail_files() {
	view_.retail_files = view_.project_open ? list_retail_file_names(game_install(), view_.document)
	                                        : std::vector<std::string>();
	touch(ViewConcern::Files);
}

// The requirement row of `role`, or null.
const RequirementRow *ProjectSession::requirement_row(const std::string &role) const {
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view_.requirements.rows)
		if (candidate.role == role) row = &candidate;
	return row;
}

// The project file `file` names: the one at that project-relative path, else the first of
// that logical name (two files of one name are the scan's asset.name.duplicate); null when
// the project has none.
const AssetEntry *ProjectSession::project_file(const std::string &file) const {
	for (const AssetEntry &candidate : view_.scan.entries)
		if (candidate.relative_path == file) return &candidate;
	const std::string wanted = normalized_logical_name(fs::path(file).filename().string());
	for (const AssetEntry &candidate : view_.scan.entries)
		if (normalized_logical_name(candidate.logical_name) == wanted) return &candidate;
	return nullptr;
}

// A requirement satisfied by renaming a project file of the expected kind to the name
// the engine demands.
void ProjectSession::assign_requirement(const std::string &role, const std::string &file) {
	if (!view_.project_open) return;
	const RequirementRow *row = requirement_row(role);
	if (!row) {
		report(make_diagnostic(DiagnosticSeverity::Error, "requirement.unknown", "No requirement has the role '" + role + "'."));
		return;
	}
	// Nothing is renamed: a refusal, so whoever asked learns the assignment did not happen.
	if (row->state == RequirementState::Present) {
		report(make_diagnostic(DiagnosticSeverity::Error, "requirement.assigned",
		                       row->name + " is already in the project: nothing was assigned.", row->asset_path));
		return;
	}
	const AssetEntry *asset = project_file(file);
	if (!asset) {
		report(make_diagnostic(DiagnosticSeverity::Error, "requirement.unknown_file", "The project has no file named '" + file + "'.", file));
		return;
	}
	if (asset->kind != row->expected_kind) {
		report(make_diagnostic(DiagnosticSeverity::Error, "requirement.kind",
		                       asset->logical_name + " is " + asset_kind_label(asset->kind) + ", and " + row->name +
		                               " must be " + asset_kind_label(row->expected_kind) + ".",
		                       asset->relative_path));
		return;
	}
	rename_asset(asset->relative_path, row->name);
}

// Writes each open document at `paths` that has unsaved edits (`rewrite`: an explicit Save,
// which also writes one with none whose file holds other bytes than it would write, and
// refuses one that does not serialize), past a failure: an Output line per file written, the
// refresh, then one finding per file that could not be, the status counting both. False when
// one could not be. (A save while a build packs never reaches here: the busy gate refused it.)
bool ProjectSession::save_documents(const std::vector<std::string> &paths, bool rewrite) {
	std::vector<Document *> writes;
	for (const std::string &path : paths)
		for (const auto &document : documents_)
			if (document->path() == path &&
			    (document->dirty() || (rewrite && document->rewrite_need() != Document::RewriteNeed::None)))
				writes.push_back(document.get());
	if (writes.empty()) {
		view_.status = paths.size() == 1 ? paths.front() + " has no changes to save." : "No file has unsaved changes.";
		touch(ViewConcern::Output);
		return true;
	}
	size_t saved = 0;
	std::vector<Diagnostic> failures;
	for (Document *document : writes) {
		Diagnostic error;
		if (!document->save(error)) {
			// A file changed outside the editor under unsaved edits: the conflict is a row
			// until the document is read again (its Reload fix).
			if (error.code == "document.conflict" && document->dirty()) conflicts_.insert(document->path());
			failures.push_back(error);
			continue;
		}
		forget_file_state(document->path());
		++saved;
		note("Saved " + document->path());
	}
	gesture_validation_due_ = false;
	update_document_view();
	refresh();
	// Reported after the refresh, which rebuilds the Problems rows.
	for (const Diagnostic &d : failures) report(d);
	view_.status = "Saved " + std::to_string(saved) + " file(s)" +
	               (failures.empty() ? "." : "; " + std::to_string(failures.size()) + " could not be saved: see Problems.");
	touch(ViewConcern::Output);
	return failures.empty();
}

// A Save of a file that is not open: read as its document type, written when it would
// write other bytes than the file holds (the canonical rewrite: the lines the game ignores
// dropped, the line ends fixed, a table regrouped), and left closed; the refresh then reads
// the file as written, so the findings the rewrite fixed leave Problems. One that does not
// serialize is refused with the reason (document.unserializable), the file untouched. Of two
// files of one name, the one project_file picks: the path named, else the first of the name.
void ProjectSession::rewrite_file(const std::string &path) {
	const AssetEntry *asset = project_file(path);
	if (!asset) return report(make_diagnostic(DiagnosticSeverity::Error, "document.missing", "The file was not found.", path));
	const std::string relative = asset->relative_path;
	Diagnostic error;
	const std::shared_ptr<Document> document = load_document(relative, asset->kind, error);
	if (!document) return report(error);
	if (document->rewrite_need() == Document::RewriteNeed::None) {
		view_.status = relative + " has no changes to save.";
		touch(ViewConcern::Output);
		return;
	}
	if (!document->save(error)) { // one that does not serialize says why here
		report(error);
		view_.status = relative + " could not be saved: see Problems.";
		touch(ViewConcern::Output);
		return;
	}
	note("Saved " + relative);
	refresh();
	view_.status = "Saved 1 file(s).";
	touch(ViewConcern::Output);
}

std::vector<std::string> ProjectSession::dirty_files() const {
	std::vector<std::string> files;
	for (const auto &document : documents_)
		if (document->dirty()) files.push_back(document->path());
	return files;
}

// EndEdit on every open document: the coalesced group (typing) and the gesture (a drag) end,
// and the validation a gesture's edits left waiting is due.
void ProjectSession::end_edit_groups() {
	for (const auto &document : documents_) document->end_edit_group();
	if (gesture_validation_due_) validate_later();
	gesture_validation_due_ = false;
}

// The one place the active document changes. The document it replaces keeps its selection
// for when it is active again; `path` takes back the one it kept (none when it never had
// one, or was read again since: a menu then shows its first screen), repaired against its
// records as they are now. A caller with a record to show selects it after.
void ProjectSession::activate(const std::string &path) {
	if (path == view_.active_document) return;
	const auto open = [this](const std::string &wanted) -> const Document * {
		for (const auto &document : documents_)
			if (document->path() == wanted) return document.get();
		return nullptr;
	};
	if (open(view_.active_document)) remembered_[view_.active_document] = {view_.selection, view_.selected};
	view_.active_document = path;
	view_.select_only({});
	const auto kept = remembered_.find(path);
	if (kept != remembered_.end()) {
		view_.selection = kept->second.primary;
		view_.selected = kept->second.selected;
		remembered_.erase(kept);
		if (const Document *document = open(path)) view_.repair_selection(*document, NodeAddress());
	}
	select_first_screen();
	touch(ViewConcern::ActiveDocument); // the caller touches Selection
}

// A menu made the active document, or read again, with nothing selected shows its first
// screen: the menu view lists the selected screen's windows and the preview draws it.
void ProjectSession::select_first_screen() {
	if (view_.selection.row) return;
	const Document *document = document_for();
	if (!document || document->kind() != AssetKind::Menu || document->rows().empty()) return;
	const Node &screen = *document->rows().front();
	view_.select_only({screen.id, screen.kind, 0});
}

bool ProjectSession::handle_document(const EditorRequest &request) {
	switch (request.kind) {
	case EditorRequestKind::CreateFile: {
		if (!view_.project_open) return true;
		// A name alone cannot say what a new `.bin` is; the request's text may name the kind.
		const AssetKind kind = request.text.empty() ? classify_asset(request.path, nullptr) : asset_kind_from_token(request.text);
		// A required name gets its requirement's blank (main.mnu, the STARTUP screen); any
		// other name the kind's free-form one (blank_factory.h). A kind with neither cannot
		// be made; one the editor does not edit (a font) is made and not opened.
		BlankRequest blank;
		blank.logical_name = request.path;
		blank.project_title = view_.document.title;
		for (const RequirementRow &row : view_.requirements.rows)
			if (row.expected_kind == kind && normalized_logical_name(row.name) == normalized_logical_name(request.path))
				blank.role = row.role;
		if (!find_blank_factory_for_role(blank.role) && !find_blank_factory_for_kind(kind)) {
			report(make_diagnostic(DiagnosticSeverity::Error, "document.kind", "The editor cannot create this kind of file.", request.path));
			return true;
		}
		// A plain name the archives can carry, whose extension is the kind's, landing
		// inside the project.
		std::string problem, message;
		if (!check_project_file_name(paths_.root, blank_placement_dir(kind), request.path, kind, problem, message)) {
			report(make_diagnostic(DiagnosticSeverity::Error, "document." + problem, message, request.path));
			return true;
		}
		const auto *existing = view_.scan.find(request.path);
		const std::string relative = (fs::path(blank_placement_dir(kind)) / request.path).generic_string();
		if (!existing) {
			const auto target = fs::path(paths_.root) / relative;
			std::error_code ec;
			if (fs::exists(target, ec) || ec) {
				report(make_diagnostic(DiagnosticSeverity::Error, "document.conflict", "Refresh before creating this file.", request.path));
				return true;
			}
			std::vector<uint8_t> bytes; Diagnostic error;
			if (!make_blank(blank, kind, bytes, error)) { report(error); return true; }
			if (!ensure_directory(target.parent_path().generic_string(), message) ||
				!write_file_atomic(target.generic_string(), bytes.data(), bytes.size(), message)) {
				report(make_diagnostic(DiagnosticSeverity::Error, "document.write", message, request.path));
				return true;
			}
			refresh();
			note("Created " + relative);
		}
		if (is_editable_kind(kind)) {
			handle(make_request(EditorRequestKind::OpenDocument, request.path));
		} else {
			view_.status = existing ? existing->relative_path + " is in the project already." : "Created " + relative + ".";
			touch(ViewConcern::Output);
		}
		return true;
	}
	case EditorRequestKind::OpenDocument:
	case EditorRequestKind::ReloadDocument: {
		if (!view_.project_open) return true;
		const std::string path = request.path.empty() ? view_.active_document : request.path;
		// The record a request names (by its address, or by its locator: a Go to) is selected,
		// and its field (a Problems row's, the defining field a Go to shows) shown.
		const auto select_named = [this, &request](const Document &document) {
			const NodeAddress record =
			        request.text.empty() ? request.edit.address : document.address_at(request.text);
			view_.select_only(record);
			if (!record.row || request.edit.field.empty()) return;
			view_.reveal_field = request.edit.field;
			++view_.reveal_serial;
		};
		if (request.kind == EditorRequestKind::OpenDocument && document_for(path)) {
			// An open document comes back with the selection it had, unless the request names
			// a record (a Problems row, a Go to).
			const Document &document = *document_for(path);
			activate(document.path());
			if (request.edit.address.row || !request.text.empty()) select_named(document);
			touch(ViewConcern::Selection);
			return true;
		}
		for (const auto &asset : view_.scan.entries) {
			if (asset.relative_path != path && normalized_logical_name(asset.logical_name) != normalized_logical_name(path)) continue;
			const DocumentType *type = document_type_for(asset.kind);
			if (!type) {
				report(make_diagnostic(DiagnosticSeverity::Error, "document.kind", "This kind of file has no editor yet.", path));
				return true;
			}
			std::shared_ptr<Document> document = type->make(); Diagnostic error;
			if (!document->load((fs::path(paths_.root) / asset.relative_path).generic_string(), asset.relative_path,
				asset.kind, view_.document.target_game, error)) { report(error); return true; }
			for (auto it = documents_.begin(); it != documents_.end(); ++it)
				if ((*it)->path() == asset.relative_path) { documents_.erase(it); break; }
			remembered_.erase(asset.relative_path); // read again: its records have new identities
			forget_file_state(asset.relative_path);
			documents_.push_back(document);
			activate(document->path());
			select_named(*document);
			select_first_screen(); // no record named: a menu shows its first screen
			touch(ViewConcern::Selection);
			update_document_view(); validate_documents(); return true;
		}
		report(make_diagnostic(DiagnosticSeverity::Error, "document.missing", "The file was not found.", path));
		return true;
	}
	case EditorRequestKind::ShowInFiles: {
		// Files shows the file (and asks its new name when the request says so); every ask is
		// shown again, the same file too.
		if (!view_.project_open) return true;
		const AssetEntry *asset = project_file(request.path);
		if (!asset) {
			report(make_diagnostic(DiagnosticSeverity::Error, "document.missing", "The file was not found.", request.path));
			return true;
		}
		view_.reveal_file = asset->relative_path;
		view_.reveal_file_rename = request.flag;
		++view_.reveal_file_serial;
		touch(ViewConcern::Selection);
		return true;
	}
	case EditorRequestKind::CloseDocument: {
		const std::string path = request.path.empty() ? view_.active_document : request.path;
		for (auto it = documents_.begin(); it != documents_.end(); ++it)
			if ((*it)->path() == path) { documents_.erase(it); break; }
		remembered_.erase(path);
		forget_file_state(path);
		if (view_.active_document == path) {
			activate(documents_.empty() ? "" : documents_.back()->path());
			touch(ViewConcern::Selection);
		}
		update_document_view(); validate_documents(); return true;
	}
	case EditorRequestKind::SelectRecord: {
		// The document's own path, however the request named it (a logical name included),
		// so a selection joined by path stays in one document; a record of another document
		// makes it the active one, the record alone selected.
		const Document *document = document_for(request.path);
		const std::string path = document ? document->path() : request.path.empty() ? view_.active_document : request.path;
		if (path != view_.active_document) {
			activate(path);
			view_.select_only(request.edit.address);
		} else {
			view_.select(path, request.edit.address, request.select_mode);
		}
		touch(ViewConcern::Selection);
		return true;
	}
	case EditorRequestKind::EditRecord: {
		// A fix's edit opens its document first (a Problems row about a file not open).
		if (!document_for(request.path) && request.flag && view_.project_open && !request.path.empty())
			handle(make_request(EditorRequestKind::OpenDocument, request.path));
		auto *document = document_for(request.path);
		if (!document) {
			if (view_.project_open) refuse_now("document.not_open", "Open the file before editing it.", request.path);
			return true;
		}
		apply_edits(*document, request.edits.empty() ? std::vector<Edit>{request.edit} : request.edits);
		return true;
	}
	case EditorRequestKind::RevertToSaved: {
		auto *document = document_for(request.path);
		if (!document) {
			if (view_.project_open) refuse_now("document.not_open", "Open the file before reverting in it.", request.path);
			return true;
		}
		std::vector<Edit> batch;
		for (const Edit &target : request.edits.empty() ? std::vector<Edit>{request.edit} : request.edits)
			for (Edit &edit : document->revert_edits(target.address, target.field)) batch.push_back(std::move(edit));
		if (batch.empty()) {
			last_edit_ok_ = false;
			refuse_now("document.revert_nothing",
			           "Nothing to revert: the field is as the saved file holds it, or the saved file does not have "
			           "it to go back to.",
			           document->path());
			return true;
		}
		apply_edits(*document, batch);
		return true;
	}
	case EditorRequestKind::Copy:
	case EditorRequestKind::Cut: {
		auto *document = document_for(request.path);
		if (!document) {
			if (view_.project_open) refuse_now("document.not_open", "Open the file before copying from it.", request.path);
			return true;
		}
		copy_records(*document, request.kind == EditorRequestKind::Cut);
		return true;
	}
	case EditorRequestKind::Paste: {
		auto *document = document_for(request.path);
		if (!document) {
			if (view_.project_open) refuse_now("document.not_open", "Open the file before pasting into it.", request.path);
			return true;
		}
		paste_records(*document, request.edit);
		return true;
	}
	case EditorRequestKind::Duplicate: {
		auto *document = document_for(request.path);
		if (!document) {
			if (view_.project_open) refuse_now("document.not_open", "Open the file before duplicating in it.", request.path);
			return true;
		}
		duplicate_records(*document);
		return true;
	}
	case EditorRequestKind::Undo:
	case EditorRequestKind::Redo: {
		auto *document = document_for(request.path);
		if (!document) {
			if (view_.project_open) refuse_now("document.not_open", "Open the file before undoing or redoing in it.", request.path);
			return true;
		}
		const uint64_t before = document->revision();
		const NodeAddress primary = view_.selection;
		const std::vector<NodeAddress> selected = view_.selected;
		if (request.kind == EditorRequestKind::Undo) document->undo(); else document->redo();
		view_.repair_selection(*document, NodeAddress());
		if (view_.selection != primary || view_.selected != selected) touch(ViewConcern::Selection);
		update_document_view();
		// An undo or a redo ends a gesture: the validation its edits left waiting runs now.
		if (document->revision() != before || gesture_validation_due_) validate_later();
		gesture_validation_due_ = false;
		return true;
	}
	case EditorRequestKind::EndEdit:
		if (auto *document = document_for(request.path)) document->end_edit_group();
		if (gesture_validation_due_) validate_later();
		gesture_validation_due_ = false;
		return true;
	case EditorRequestKind::Save: {
		if (Document *document = document_for(request.path)) {
			save_documents({document->path()}, true);
		} else if (view_.project_open) {
			// A file that is not open (a Rewrite fix names one) is rewritten closed.
			if (request.path.empty()) refuse_now("document.not_open", "Open a file before saving it.");
			else rewrite_file(request.path);
		}
		return true;
	}
	case EditorRequestKind::SaveAll: save_documents(dirty_files(), false); return true;
	default: return false;
	}
}

// --- the selection, the edits, the clipboard -------------------------------------------

// One EditRecord: a single edit or a batch on one row, then the selection follows (a
// new record selected, a removed one's owner) and the validation is left due (or, for a
// gesture, until it ends).
bool ProjectSession::apply_edits(Document &document, const std::vector<Edit> &edits) {
	last_edit_ok_ = false;
	NodeAddress owner;
	Document::Placement at;
	if (document.path() == view_.active_document && document.placement(view_.selection, at)) owner = at.owner;
	Diagnostic error;
	const uint64_t before = document.revision();
	const std::string active = view_.active_document;
	const NodeAddress primary = view_.selection;
	const std::vector<NodeAddress> selected = view_.selected;
	// A screen's or a window's new name follows into the references of its file that find it
	// by the name it had when the edit's group began (graph/rename_transaction's
	// plan_symbol_rename), in the same step.
	if (!document.apply(edits, plan_symbol_rename, error)) {
		report(error);
		return false;
	}
	last_edit_ok_ = true;
	bool adds = false, gesture = false;
	for (const Edit &edit : edits) {
		adds = adds || edit.operation == EditOperation::Add || edit.operation == EditOperation::Duplicate ||
		       edit.operation == EditOperation::Paste;
		gesture = gesture || edit.gesture != 0;
	}
	if (adds && document.revision() != before) {
		activate(document.path()); // what an edit adds is selected, in its own document
		view_.select_added(document);
	} else {
		view_.repair_selection(document, owner);
	}
	if (view_.active_document != active || view_.selection != primary || view_.selected != selected)
		touch(ViewConcern::Selection);
	update_document_view();
	// A Move that leaves a record where it is changes nothing to validate; a gesture's
	// edits validate once it ends (EndEdit, Undo, Redo, Save).
	if (document.revision() != before) {
		if (gesture) gesture_validation_due_ = true;
		else validate_later();
	}
	view_.status = "Edited " + document.path() + ".";
	touch(ViewConcern::Output);
	return true;
}

void ProjectSession::copy_records(Document &document, bool cut) {
	last_edit_ok_ = false;
	const std::vector<NodeAddress> records =
	        document.path() == view_.active_document ? document.outermost(view_.selected) : std::vector<NodeAddress>();
	if (records.empty())
		return refuse_now("document.copy", "Select the records to copy first.", document.path());
	std::string payload = document.copy(records);
	if (payload.empty())
		return refuse_now("document.copy", "These records cannot be copied.", document.path());
	view_.clipboard = std::move(payload);
	touch(ViewConcern::Selection);
	if (!cut) {
		last_edit_ok_ = true;
		view_.status = "Copied " + std::to_string(records.size()) + " record(s).";
		touch(ViewConcern::Output);
		return;
	}
	std::vector<Edit> removes;
	for (const NodeAddress &record : records) {
		Edit edit;
		edit.operation = EditOperation::Remove;
		edit.address = record;
		removes.push_back(edit);
	}
	if (apply_edits(document, removes)) view_.status = "Cut " + std::to_string(records.size()) + " record(s).";
}

void ProjectSession::paste_records(Document &document, const Edit &target) {
	last_edit_ok_ = false;
	if (view_.clipboard.empty())
		return refuse_now("document.paste", "The clipboard is empty: copy records first.", document.path());
	Edit edit = target;
	edit.operation = EditOperation::Paste;
	edit.value = view_.clipboard;
	if (!edit.address.row && !edit.parent) {
		// No target named: beside the selected record, or into the selected row.
		if (document.path() != view_.active_document || !view_.selection.row)
			return refuse_now("document.paste", "Select where to paste.", document.path());
		Document::Placement at;
		edit.address.row = view_.selection.row;
		if (document.placement(view_.selection, at)) {
			edit.parent = at.owner.child;
			edit.position = at.index + 1;
		} else {
			edit.parent = 0;
			edit.position = SIZE_MAX;
		}
	}
	apply_edits(document, {edit});
}

// Each selected record (a record inside another selected one goes with it) copied right
// after itself, one step: a row on its own, nested records as one batch.
void ProjectSession::duplicate_records(Document &document) {
	last_edit_ok_ = false;
	const std::vector<NodeAddress> records =
	        document.path() == view_.active_document ? document.outermost(view_.selected) : std::vector<NodeAddress>();
	if (records.empty())
		return refuse_now("document.duplicate", "Select the records to duplicate first.", document.path());
	if (!records.front().child) {
		// A row: after itself among the rows (the selection stays inside one row, so it is alone).
		Edit edit;
		edit.operation = EditOperation::Duplicate;
		edit.address = records.front();
		for (size_t i = 0; i < document.rows().size(); ++i)
			if (document.rows()[i]->id == edit.address.row) edit.position = i + 1;
		if (apply_edits(document, {edit})) view_.status = "Duplicated a record.";
		return;
	}
	// In document order; a copy lands after its original, so the originals after it in the
	// same collection shift by the copies made before them.
	struct Item {
		NodeAddress record;
		Document::Placement at;
	};
	std::vector<Item> items;
	for (const NodeAddress &record : records) {
		Item item{record, {}};
		if (!document.placement(record, item.at))
			return refuse_now("document.selection", "The selected record no longer exists.", document.path());
		items.push_back(item);
	}
	std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
		if (a.at.owner != b.at.owner) return a.at.owner.child < b.at.owner.child;
		if (a.at.spec.kind != b.at.spec.kind) return a.at.spec.kind < b.at.spec.kind;
		return a.at.index < b.at.index;
	});
	std::vector<Edit> edits;
	for (size_t i = 0; i < items.size(); ++i) {
		size_t before = 0; // copies already made in this record's collection, ahead of it
		for (size_t j = 0; j < i; ++j)
			before += items[j].at.owner == items[i].at.owner && items[j].at.spec.kind == items[i].at.spec.kind;
		Edit edit;
		edit.operation = EditOperation::Duplicate;
		edit.address = items[i].record;
		edit.position = items[i].at.index + before + 1;
		edits.push_back(edit);
	}
	if (apply_edits(document, edits)) view_.status = "Duplicated " + std::to_string(edits.size()) + " record(s).";
}

// What a request the prompt guards would lose, pack or write over, now: a Close or a Reload,
// its document when that has unsaved edits; a project switch, Quit, Build and Play, every
// document with unsaved edits; an import that replaces files, the documents with unsaved
// edits among the files it writes; a rename (and an assignment, which renames), those among
// the files it rewrites and the renamed file itself; a name renamed everywhere, those among
// the files it rewrites. False for a request the prompt does not guard.
bool ProjectSession::unsaved_files(const EditorRequest &request, std::vector<std::string> &files) {
	files.clear();
	switch (request.kind) {
	case EditorRequestKind::CloseDocument:
	case EditorRequestKind::ReloadDocument:
		if (const Document *document = document_for(request.path); document && document->dirty())
			files.push_back(document->path());
		return true;
	case EditorRequestKind::Build:
	case EditorRequestKind::Play:
	case EditorRequestKind::NewProject:
	case EditorRequestKind::OpenProject:
	case EditorRequestKind::CloseProject:
	case EditorRequestKind::Quit: files = dirty_files(); return true;
	case EditorRequestKind::ImportFiles:
		// An import writes over a project file only when it replaces one (else a file of the
		// name is refused, or kept when it holds the same bytes): the files its sources make
		// land where the plan puts them. The plan has no cap here: import_assets writes every
		// file of the request, so every destination is looked at.
		if (view_.project_open && request.flag && documents_dirty()) {
			validate_pending();
			const ImportPlan plan = plan_import(request.imports, false, paths_, view_.document, view_.scan, *graph_,
			                                    game_install(), SIZE_MAX);
			for (const auto &document : documents_) {
				if (!document->dirty()) continue;
				if (std::any_of(plan.rows.begin(), plan.rows.end(), [&document](const ImportPlanRow &row) {
					    return row.state != ImportPlanRow::State::NotFound && row.destination == document->path();
				    }))
					files.push_back(document->path());
			}
		}
		return true;
	case EditorRequestKind::RenameAsset: rename_unsaved(request.path, request.text, files); return true;
	case EditorRequestKind::RenameSymbol:
		// The documents with unsaved edits among the files the plan rewrites, and those whose
		// saved file names the name (the commit reads the files on disk: an unsaved edit that
		// stopped naming it would leave the saved use behind); none when the plan is refused
		// anyway (the rename then says why, with nothing to save first). Saved, the rename is
		// planned again.
		if (view_.project_open && documents_dirty()) {
			const SymbolRenamePlan plan = plan_symbol(request);
			if (!plan.ok()) return true;
			for (const auto &document : documents_) {
				if (!document->dirty()) continue;
				const bool site = std::any_of(plan.sites.begin(), plan.sites.end(),
				                              [&](const RenameSite &each) { return each.file == document->path(); });
				if (site || saved_file_uses(document->path(), plan)) files.push_back(document->path());
			}
		}
		return true;
	case EditorRequestKind::AssignRequirement: {
		const RequirementRow *row = requirement_row(request.text);
		const AssetEntry *asset = project_file(request.path);
		if (row && asset && row->state != RequirementState::Present && asset->kind == row->expected_kind)
			rename_unsaved(asset->relative_path, row->name, files);
		return true;
	}
	default: return false;
	}
}

// The documents with unsaved edits a rename of `file` to `new_name` would rewrite (its
// plan's sites) or leave behind on the old name (the file's own), in the order they were
// opened; none when the plan is refused anyway (the rename then says why, with nothing to
// save first). The plan reads the graph: an edit a held pump made first reaches it.
void ProjectSession::rename_unsaved(const std::string &file, const std::string &new_name, std::vector<std::string> &files) {
	if (!view_.project_open || !documents_dirty()) return;
	validate_pending();
	const RenamePlan plan = plan_rename(paths_, view_.scan, *graph_, file, new_name);
	if (!plan.ok()) return;
	for (const auto &document : documents_) {
		if (!document->dirty()) continue;
		const std::string &path = document->path();
		if (path == plan.path || std::any_of(plan.sites.begin(), plan.sites.end(),
		                                     [&path](const RenameSite &site) { return site.file == path; }))
			files.push_back(path);
	}
}

// A request that would lose, pack or write over unsaved edits waits on the prompt instead of
// running, the prompt listing those files (unsaved_files); Build and Play pack the files on
// disk and an import or a rename writes them, so those offer no Discard: their Save writes
// the edits first. One that finds nothing unsaved goes ahead, and a prompt still open from
// an earlier request is dropped: what it waited on was saved another way, and this request
// comes after it. False when the request goes ahead.
bool ProjectSession::guard_unsaved(const EditorRequest &request) {
	std::vector<std::string> files;
	if (!unsaved_files(request, files)) return false;
	if (files.empty()) {
		if (pending_request_) close_unsaved_prompt();
		return false;
	}
	const bool one_document = request.kind == EditorRequestKind::CloseDocument || request.kind == EditorRequestKind::ReloadDocument;
	SessionView::UnsavedPrompt prompt;
	prompt.open = true;
	prompt.action = request.kind;
	prompt.target = one_document ? files.front() : request.path;
	prompt.files = std::move(files);
	prompt.can_discard = request.kind != EditorRequestKind::Build && request.kind != EditorRequestKind::Play &&
	                     request.kind != EditorRequestKind::ImportFiles && request.kind != EditorRequestKind::RenameAsset &&
	                     request.kind != EditorRequestKind::AssignRequirement && request.kind != EditorRequestKind::RenameSymbol;
	pending_request_ = request;
	pending_request_->path = prompt.target;
	view_.unsaved_prompt = std::move(prompt);
	outcome_.unsaved_prompt = true;
	touch(ViewConcern::Dialogs);
	return true;
}

// The prompt's answer (UnsavedChoice). Before a Save or a Discard acts, what the request
// would lose or pack is taken again: a file made unsaved since the prompt opened (the
// editor MCP's edit, an undo) renews the prompt with it, and nothing is saved or dropped
// that the prompt did not list. A Save that cannot write every file it lists keeps the
// prompt open over what waits, the failures reported: the modder saves again or cancels.
void ProjectSession::resolve_unsaved(UnsavedChoice choice) {
	if (!pending_request_)
		return refuse_now("unsaved.none", "No unsaved-changes prompt is open: nothing waits on an answer.");
	if (choice == UnsavedChoice::Cancel) return close_unsaved_prompt();
	if (choice == UnsavedChoice::Discard && !view_.unsaved_prompt.can_discard) {
		refuse_now("unsaved.discard", "Build and Play pack the files on disk: save the edited files or cancel.");
		outcome_.unsaved_prompt = true;
		return;
	}
	std::vector<std::string> files;
	unsaved_files(*pending_request_, files);
	const std::vector<std::string> &listed = view_.unsaved_prompt.files;
	for (const std::string &file : files) {
		if (std::find(listed.begin(), listed.end(), file) != listed.end()) continue;
		view_.unsaved_prompt.files = files;
		view_.status = file + " has unsaved changes too: the prompt lists it now.";
		outcome_.unsaved_prompt = true;
		touch(ViewConcern::Dialogs);
		touch(ViewConcern::Output);
		return;
	}
	if (choice == UnsavedChoice::Save) {
		// Its Save writes the files an operation may hold (a build packing them): refused as a
		// Save All is, the prompt kept, unless what waits cancels that operation anyway (a project
		// switch, Quit), which then cancels it first.
		if (busy_for(EditorRequestKind::SaveAll)) {
			if (request_kind_row(pending_request_->kind).on_busy != OnBusy::CancelRunning) {
				refuse_busy(std::string());
				outcome_.unsaved_prompt = true;
				return;
			}
			cancel_operation(false);
		}
		end_edit_groups();
		if (!save_documents(view_.unsaved_prompt.files, false)) {
			outcome_.unsaved_prompt = true;
			return;
		}
	}
	const EditorRequest pending = *pending_request_;
	close_unsaved_prompt();
	if (choice == UnsavedChoice::Discard) {
		if (pending.kind == EditorRequestKind::CloseDocument || pending.kind == EditorRequestKind::ReloadDocument) {
			for (auto it = documents_.begin(); it != documents_.end(); ++it)
				if ((*it)->path() == pending.path) { documents_.erase(it); break; }
			remembered_.erase(pending.path);
			forget_file_state(pending.path);
		} else {
			documents_.clear();
			remembered_.clear();
			stale_.clear();
			conflicts_.clear();
		}
		update_document_view();
	}
	handle(pending);
}

void ProjectSession::close_unsaved_prompt() {
	pending_request_.reset();
	view_.unsaved_prompt = SessionView::UnsavedPrompt();
	touch(ViewConcern::Dialogs);
}

} // namespace opennova::editor
