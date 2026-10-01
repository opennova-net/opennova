#include <editor/session/session_core.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <optional>
#include <utility>

#include <editor/assets/project_scan.h>
#include <editor/blank/create_missing.h>
#include <editor/project/project_files.h>
#include <editor/project/project_refresh.h>
#include <editor/project_build/build_plan.h>
#include <editor/run/play_lease.h>
#include <editor/session/build_operation.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/import_controller.h>
#include <editor/session/open_operation.h>
#include <editor/session/play_controller.h>
#include <editor/session/problems_service.h>
#include <editor/session/refresh_operation.h>
#include <editor/session/unsaved_guard.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

bool same_finding(const Diagnostic &a, const Diagnostic &b) {
	return a.severity == b.severity && a.row() == b.row() && a.message == b.message && a.asset == b.asset &&
	       a.field == b.field && a.record == b.record && a.line == b.line;
}

// Whether `path` is the folder `dir` or under it, symbolic links resolved (weakly_canonical, which
// also gives a folder that exists its own spelling).
bool inside(const fs::path &path, const fs::path &dir) {
	std::error_code ec;
	const fs::path base = fs::weakly_canonical(dir, ec);
	if (ec) return false;
	const fs::path full = fs::weakly_canonical(path, ec);
	if (ec) return false;
	const fs::path relative = full.lexically_relative(base);
	return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

} // namespace

SessionCore::SessionCore(ProcessPlatform &platform, EditorPreferences &preferences) :
		platform_(platform), preferences_(preferences) {}

void SessionCore::start() {
	// A store that cannot be read, or a settings file set aside (another schema): said, the
	// defaults in effect.
	Diagnostic finding;
	if (!preferences_.load(finding) || !finding.code().empty())
		report(finding);
	const Preferences &settings = preferences_.values();
	view_.project.recent_projects = settings.recent_projects;
	view_.project.retail_directory = settings.game_install;
	view_.project.play_retail = settings.play_in_install;
	view_.project.runtime_setting = settings.runtime_executable;
	view_.project.import_dependencies = settings.import_dependencies;
	view_.activity.status = "No project open.";
	touch(ViewConcern::Preferences);
	touch(ViewConcern::Graph);
	touch(ViewConcern::Output);
}

SessionCore::RequestScope::RequestScope(SessionCore &core) : core_(core), outermost_(!core.in_request_) {
	assert(outermost_ && "a request entered the session while another was served");
	if (!outermost_) return;
	core_.outcome_ = ActionOutcome();
	core_.in_request_ = true;
}

SessionCore::RequestScope::~RequestScope() {
	if (outermost_) core_.in_request_ = false;
}

void SessionCore::note(std::string line) {
	view_.activity.output.append(std::move(line));
	touch(ViewConcern::Output);
}

void SessionCore::report(const Diagnostic &d) {
	problems().add_reported(d);
	record_outcome(d);
	note(std::string(diagnostic_severity_label(d.severity)) + ": " + d.message);
}

// A finding raised while serving a request is that request's outcome; one raised by a
// poll (the build finishing, the game's log) belongs to no request.
void SessionCore::record_outcome(const Diagnostic &d) {
	if (!in_request_) return;
	outcome_.findings.push_back(d);
	if (d.severity == DiagnosticSeverity::Error) outcome_.refused = true;
}

// A request that cannot run now (a build is packing the project's files, the document it
// names is not open): nothing is wrong with the project, so the row is a warning, but the
// request did nothing and its outcome says so.
void SessionCore::refuse_now(CoreFinding code, const std::string &message, const std::string &asset) {
	report(make_finding(code, DiagnosticSeverity::Warning, message, asset));
	if (in_request_) outcome_.refused = true;
}

// --- the operation slot ------------------------------------------------------------------

uint64_t SessionCore::start_operation(std::unique_ptr<SessionOperation> operation) {
	const uint64_t id = operations_.start(std::move(operation));
	if (id != 0) show_operation();
	return id;
}

// The running operation's steps, within `budget`: its progress moves Operation alone.
void SessionCore::step_operation(const PollBudget &budget) {
	if (operations_.running() && !operations_.done()) {
		operations_.poll(budget, steady_clock_ms);
		view_.activity.operation = operations_.status();
		touch(ViewConcern::Operation);
	}
}

bool SessionCore::busy_for(Holds reads, Holds writes) const {
	const OperationStatus running = operations_.status();
	return running.running() && holds_conflict(reads, writes, running.reads, running.writes);
}

std::string SessionCore::busy_message(const std::string &until) const {
	const SessionOperation *running = operations_.running();
	const std::string noun = running ? operation_kind_row(running->kind()).noun : "the operation";
	return "Wait for " + noun + " to finish" + (running && running->cancellable() ? ", or cancel it, " : " ") + until +
	       ".";
}

void SessionCore::refuse_busy(const std::string &asset) {
	refuse_now(CoreFinding::OperationBusy, busy_message("first"), asset);
}

// The running operation stopped between two steps, its work discarded (a build's staging
// directory removed, the Play waiting on it dropped); `asked` is CancelOperation's, which says
// so when nothing runs or the operation cannot be cancelled. True when none runs now.
bool SessionCore::cancel_operation(bool asked) {
	const SessionOperation *running = operations_.running();
	if (!running) {
		if (asked) refuse_now(CoreFinding::OperationNone, "Nothing is running to cancel.");
		return true;
	}
	const std::string noun = operation_kind_row(running->kind()).noun;
	if (!operations_.cancel()) {
		if (asked) refuse_now(CoreFinding::OperationNotCancellable, "This cannot be cancelled now: wait for " + noun + " to finish.");
		return false;
	}
	const std::string line = "Cancelled " + noun + ".";
	if (asked) view_.activity.status = line;
	note(line);
	show_operation();
	return true;
}

void SessionCore::finish_operation() {
	operations_.finish(*this);
	problems().show_validation(); // a finish that read the files leaves their validation due
	show_operation();
}

// The slot as the view shows it: the running operation (none) and what the last one came to.
void SessionCore::show_operation() {
	view_.activity.operation = operations_.status();
	view_.activity.last_operation = operations_.last();
	touch(ViewConcern::Operation);
}

// --- the project ---------------------------------------------------------------------------

// The project made in `dir`, then opened. What would refuse it (a project there already, a game no
// gameprofile has) is asked before anything changes: the open project stays open, its operation
// running. Then the open project closes, its operation cancelled (refused, nothing made, when it
// cannot be), and only then is the folder made, titled `title` or else, left empty, after the
// folder (create_project's rule, the one every path to a new project takes).
bool SessionCore::new_project(const std::string &dir, const std::string &title, const std::string &game,
                              bool import_pass) {
	if (dir.empty()) return false;
	const std::string target_game = game.empty() ? std::string(kDefaultTargetGame) : game;
	ProjectDocument doc;
	Diagnostic error;
	if (!can_create_project(dir, target_game, error)) {
		report(error);
		view_.activity.status = "The project could not be created.";
		touch(ViewConcern::Output);
		return false;
	}
	if (!close_project()) return false;
	if (!create_project(dir, title, target_game, doc, error)) {
		report(error);
		view_.activity.status = "The project could not be created.";
		touch(ViewConcern::Output);
		return false;
	}
	note("Created " + doc.title + ".");
	return open_project(dir, import_pass);
}

// The project in `dir` read before the open one closes: one that does not open leaves the open
// project open, its operation running; one that does closes it (close_project: refused, nothing
// opened, when its operation cannot be cancelled), and the Open reads its files as an operation.
// Without its import pass (`import_pass` false) it opens on its files as they are: a dry run's
// read imports no source, and a request whose own refresh runs the pass (a build, a reimport) runs
// it once. `game_install` is the install it opens with for this session alone, in place of the one
// its local.json names, which stays as it is (a dry run's).
bool SessionCore::open_project(const std::string &dir, bool import_pass, const std::string &game_install) {
	if (dir.empty()) return false;
	ProjectDocument doc;
	Diagnostic error;
	if (!::opennova::editor::open_project(dir, doc, error)) {
		report(error);
		preferences_.forget_recent_project(dir);
		save_preferences();
		view_.activity.status = "The project could not be opened.";
		touch(ViewConcern::Output);
		return false;
	}
	if (!close_project()) return false;
	const ProjectPaths paths = ProjectPaths::for_root(dir);
	// The install whose names the Open lists: the run's own (game_install, for this session alone),
	// else the project's (its local.json, read here and written nowhere), else the one last chosen
	// in the editor, which the Open's finish writes into a local.json that names none (absorb_open,
	// open_local_settings), saying then what it set aside: a cancelled Open leaves the file as it
	// was and reports nothing.
	LocalSettings local;
	Diagnostic unread;
	if (!load_local_settings(paths, local, unread)) local = LocalSettings();
	const std::string seed = game_install.empty() ? preferences_.values().game_install : std::string();
	if (!game_install.empty()) local.game_install = absolute_install_path(game_install);
	else if (local.game_install.empty()) local.game_install = absolute_install_path(seed);
	const std::string title = doc.title;
	const uint64_t id = start_operation(
			std::make_unique<OpenOperation>(paths, std::move(local), std::move(doc), import_pass, seed, game_install));
	if (id == 0) {
		refuse_busy(dir); // the close cancelled what ran: nothing does
		return false;
	}
	outcome_.operation = id;
	view_.activity.status = "Opening " + title + "...";
	touch(ViewConcern::Output);
	return true;
}

// The project an Open read made the open one, as the Open read it: its paths, its local settings
// (opened now: the install last chosen written into a local.json that names none, a file of another
// schema set aside, either said; the run's own install over them, written nowhere) and its
// document, the recent projects and the runtime, the game install's names, then its files
// (absorb_refresh). Output names the project; the menu bar's tooltip on what was said names its
// folder.
OperationOutcome SessionCore::absorb_open(OpenOperation &open) {
	OperationOutcome outcome;
	paths_ = open.paths();
	Diagnostic local_finding;
	if (!open_local_settings(paths_, open.seed(), local_, local_finding) || !local_finding.code().empty()) {
		report(local_finding);
		outcome.findings.push_back(local_finding); // what the Open came to says it too
	}
	if (!open.run_install().empty()) local_.game_install = absolute_install_path(open.run_install());
	view_.project.open = true;
	view_.project.root = paths_.root;
	view_.project.document = std::make_shared<const ProjectDocument>(open.document());
	view_.activity.has_build = false;
	view_.activity.last_build = std::make_shared<const BuildReport>();
	preferences_.remember_recent_project(paths_.root);
	save_preferences();
	view_.activity.runtime_executable = play().resolve_runtime_executable();
	imports().set_install_files(std::move(open.install_files()));
	absorb_refresh(open.refresh());
	const std::string &title = view_.project.document->title;
	note("Opened " + title + ".");
	view_.activity.status = "Opened " + title + ".";
	touch(ViewConcern::Project);
	touch(ViewConcern::Operation); // no build yet
	touch(ViewConcern::Output);
	return outcome;
}

bool SessionCore::close_project() {
	// Its operation goes first (the gate let a project switch through for its flow to cancel it
	// here, once the request's own checks passed): a build is cancelled, and a Play waiting on it.
	// One that cannot be cancelled keeps the project open.
	if (!cancel_operation(false)) {
		refuse_busy(std::string());
		return false;
	}
	problems().clear();
	if (!view_.project.open) return true;
	const std::string title = view_.project.document->title;
	// What belongs to the project goes with it: its documents, their selections, a prompt
	// waiting on them (an answer to it afterwards is refused: nothing waits), and the boot
	// report of the game started in it (a later line of that game's log is ignored; the
	// next project opens with none, even when it is this one again).
	documents().close_all();
	guard().close_prompt_if_open();
	play().forget_project();
	documents().activate(std::string());
	view_.documents.clipboard.clear();
	documents().update_view();
	view_.project.open = false;
	imports().clear();
	view_.project.root.clear();
	view_.project.document = std::make_shared<const ProjectDocument>();
	view_.project.scan = std::make_shared<const AssetScan>();
	view_.project.requirements = std::make_shared<const RequirementReport>();
	view_.findings.diagnostics.clear();
	view_.activity.has_build = false;
	view_.activity.last_build = std::make_shared<const BuildReport>();
	// The last build's findings are this project's and go with it.
	problems().clear_build_findings();
	paths_ = ProjectPaths();
	local_ = LocalSettings();
	view_.activity.runtime_executable = play().resolve_runtime_executable();
	view_.project.retail_directory = game_install();
	note("Closed " + title + ".");
	view_.activity.status = "No project open.";
	// What the project was goes with it: every concern of the view moves.
	for (size_t concern = 0; concern < kViewConcernCount; ++concern)
		touch(static_cast<ViewConcern>(concern));
	return true;
}

bool SessionCore::start_refresh(bool reimport, bool force, const std::string &only) {
	const uint64_t id =
			start_operation(std::make_unique<RefreshOperation>(paths_, *view_.project.document, reimport, force, only));
	if (id == 0) {
		refuse_busy(std::string());
		return false;
	}
	outcome_.operation = id;
	return true;
}

// The project's files as a refresh read them through the engine's one refresh
// (project/project_refresh.h: import, scan, requirements), the same the command line runs; the
// validation the Problems rows follow is left due.
ImportRunResult SessionCore::absorb_refresh(ProjectRefresh &refresh) {
	ImportRunResult imports = std::move(refresh.imports());
	view_.project.imports = std::make_shared<const std::vector<ImportedSource>>(imports.sources);
	for (const ImportedSource &source : imports.sources)
		if (source.reimported) note("Imported " + source.source + " (" + std::to_string(source.outputs.size()) + " file" +
		                            (source.outputs.size() == 1 ? "" : "s") + ")");
	files_scanned_ = refresh.files_scanned();
	view_.project.scan = std::make_shared<const AssetScan>(std::move(refresh.scan()));
	problems().set_scan(paths_.root, *view_.project.scan, view_.project.document->target_game);
	// The requirements over that scan, with the project's document as it is now: a features change
	// made while the refresh ran (S13 A3: it waits for none) is the one they follow.
	view_.project.requirements = std::make_shared<const RequirementReport>(
			evaluate_requirements(*view_.project.document, *view_.project.scan));
	touch(ViewConcern::Files);
	problems().validate_later();
	return imports;
}

void SessionCore::refresh_now() {
	ProjectRefresh refresh(paths_, *view_.project.document);
	while (!refresh.step(kWholeWalkStep)) {
	}
	absorb_refresh(refresh);
	problems().validate_pending();
}

void SessionCore::update_files(const std::vector<std::string> &paths) {
	AssetScan scan = *view_.project.scan;
	files_scanned_ = scan.update(paths_, *view_.project.document, paths);
	view_.project.scan = std::make_shared<const AssetScan>(std::move(scan));
	problems().set_scan(paths_.root, *view_.project.scan, view_.project.document->target_game);
	view_.project.requirements = std::make_shared<const RequirementReport>(
			evaluate_requirements(*view_.project.document, *view_.project.scan));
	touch(ViewConcern::Files);
	problems().validate_later();
}

// The settings a request names that differ from those in effect, written: the project's
// (its name and features) to project.opennova and the editor's (the game install, the
// runtime, Play in the game install) to its preferences. Each is written from a copy and
// its values take effect once it is written, so a setting that failed is still the one in
// effect and a retry writes it again, while one that was written is compared with from then
// on. The result (the view's settings_result) lists what could not be written, each also a
// finding, and a SettingsApplied event carries the request's serial back. The request is never
// refused whole (the settings dialog waits on its result): each part is weighed against the
// running operation as what it reads and writes, and a part that conflicts is a failure the
// result carries, its setting unchanged. The name writes the project; the features write it
// too and evaluate the requirements again over the scan the view holds (no file is read: S13
// A3); the game install writes the project's local settings. The editor's own preferences hold
// nothing an operation holds.
void SessionCore::apply_project_settings(const ProjectSettingsChange &change) {
	std::vector<Diagnostic> failures;
	const auto refuse_part = [this, &failures](const std::string &until) {
		failures.push_back(make_finding(CoreFinding::OperationBusy, DiagnosticSeverity::Warning, busy_message(until)));
	};
	ProjectDocument project = *view_.project.document;
	bool project_changed = false, features_changed = false;
	if (change.title && *change.title != project.title) {
		if (change.title->empty()) {
			failures.push_back(make_finding(CoreFinding::ProjectTitleEmpty, DiagnosticSeverity::Error, "A project needs a name."));
		} else if (busy_for(HoldsNothing, HoldsProject)) {
			refuse_part("before renaming the project");
		} else {
			project.title = *change.title;
			project_changed = true;
		}
	}
	const bool mission = change.mission && *change.mission != project.features.mission;
	const bool multiplayer = change.multiplayer && *change.multiplayer != project.features.multiplayer;
	// The features write the project and evaluate the requirements again over the scan the view
	// holds (no file read): they wait only for what writes the project.
	if ((mission || multiplayer) && busy_for(HoldsNothing, HoldsProject)) {
		refuse_part("before changing the project's features");
	} else {
		if (mission) project.features.mission = *change.mission;
		if (multiplayer) project.features.multiplayer = *change.multiplayer;
		if (mission || multiplayer) project_changed = features_changed = true;
	}
	if (project_changed && !view_.project.open) {
		failures.push_back(make_finding(CoreFinding::ProjectNone, DiagnosticSeverity::Error,
		                                "Open a project to change its name or its features."));
		project_changed = features_changed = false;
	}
	if (project_changed) {
		Diagnostic error;
		if (save_project_document(paths_.project_file, project, error)) {
			view_.project.document = std::make_shared<const ProjectDocument>(project);
			if (features_changed) {
				// The requirements follow the features, over the files as the scan lists them.
				view_.project.requirements = std::make_shared<const RequirementReport>(
						evaluate_requirements(project, *view_.project.scan));
				touch(ViewConcern::Files);
				problems().validate_later();
			}
		} else {
			failures.push_back(error);
			project_changed = false;
		}
	}
	// The game install is the open project's (ADR 0046 d6/d10), written to its local.json,
	// which opennova-project reads too; the editor's machine setting keeps the install last
	// chosen, where a project that names none starts. Both keep it absolute: a relative path
	// is taken from the editor's working directory, not from wherever the command line runs.
	std::optional<std::string> install =
	        change.game_install ? std::optional<std::string>(absolute_install_path(*change.game_install))
	                                : std::nullopt;
	if (install && view_.project.open && *install != local_.game_install && busy_for(HoldsNothing, HoldsProject)) {
		refuse_part("before changing the game install");
		install.reset();
	}
	bool install_changed = false;
	if (install && view_.project.open && *install != local_.game_install) {
		LocalSettings local = local_;
		local.game_install = *install;
		Diagnostic error;
		if (save_local_settings(paths_, local, error)) {
			local_ = std::move(local);
			install_changed = true;
		} else {
			failures.push_back(error);
		}
	}
	const Preferences &settings = preferences_.values();
	Preferences editor = settings;
	if (install) editor.game_install = *install;
	if (change.runtime_executable) editor.runtime_executable = *change.runtime_executable;
	if (change.play_in_install) editor.play_in_install = *change.play_in_install;
	bool editor_changed = editor.game_install != settings.game_install ||
	                      editor.runtime_executable != settings.runtime_executable ||
	                      editor.play_in_install != settings.play_in_install;
	if (editor_changed) {
		Diagnostic error;
		if (preferences_.write(editor, error)) {
			view_.project.play_retail = preferences_.values().play_in_install;
			view_.project.runtime_setting = preferences_.values().runtime_executable;
			view_.activity.runtime_executable = play().resolve_runtime_executable();
		} else {
			failures.push_back(error);
			editor_changed = false;
		}
	}
	if (game_install() != view_.project.retail_directory) {
		view_.project.retail_directory = game_install();
		touch(ViewConcern::Preferences);
		imports().refresh_install_files();
	}
	for (const Diagnostic &failure : failures) report(failure);
	view_.project.settings_result.failures = failures;
	// The dialog waiting on its Apply learns it came by the event naming its serial.
	ViewEvent applied;
	applied.kind = ViewEventKind::SettingsApplied;
	applied.flag = !failures.empty();
	applied.tag = change.serial;
	view_.events.post(std::move(applied));
	view_.activity.status = !failures.empty() ? "A setting could not be saved: see Problems."
			: project_changed || install_changed || editor_changed ? "Saved the settings."
																   : "No setting changed.";
	if (project_changed) touch(ViewConcern::Project);
	if (install_changed || editor_changed) touch(ViewConcern::Preferences);
	touch(ViewConcern::Dialogs);
	touch(ViewConcern::Output);
}

// The required files `roles` names, made from their factories. The checklist the request
// was raised from may be older than the tree: it is evaluated again over the files as they
// are now (a scan, the import pass's findings kept), so a file that has appeared since is
// refused (create_missing.exists), never overwritten; that scan, the files made read into it
// (AssetScan::update), is then the view's.
void SessionCore::create_missing(const std::vector<std::string> &roles) {
	if (roles.empty()) {
		view_.activity.status = "Nothing to create.";
		touch(ViewConcern::Output);
		return;
	}
	const ProjectDocument &doc = *view_.project.document;
	AssetScan now = scan_project_assets(paths_, doc);
	now.set_import_findings(view_.project.scan->import_findings());
	const CreateMissingResult result = create_missing_requirements(paths_, doc, evaluate_requirements(doc, now), roles);
	for (const std::string &path : result.created) note("Created " + path);
	for (const std::string &name : result.unavailable) {
		note("The editor cannot create " + name + " yet: no writer exists for this kind of file.");
	}
	now.update(paths_, doc, result.created);
	view_.project.scan = std::make_shared<const AssetScan>(std::move(now));
	problems().set_scan(paths_.root, *view_.project.scan, doc.target_game);
	view_.project.requirements = std::make_shared<const RequirementReport>(evaluate_requirements(doc, *view_.project.scan));
	touch(ViewConcern::Files);
	problems().validate_later();
	for (const Diagnostic &d : result.diagnostics) report(d);
	if (result.created.empty() && result.unavailable.empty() && result.diagnostics.empty()) {
		view_.activity.status = "Nothing to create.";
	} else {
		view_.activity.status = std::to_string(result.created.size()) + " file(s) created" +
		               (result.unavailable.empty()
		                        ? "."
		                        : ", " + std::to_string(result.unavailable.size()) + " not yet possible.");
	}
	touch(ViewConcern::Output);
}

void SessionCore::forget_recent(const std::string &root) {
	preferences_.forget_recent_project(root);
	save_preferences();
}

void SessionCore::clear_output() {
	view_.activity.output.clear();
	touch(ViewConcern::Output);
}

// The running operation goes first (a build's staging directory with it); one that cannot be
// cancelled keeps the editor open.
void SessionCore::quit() {
	if (!cancel_operation(false)) {
		refuse_busy(std::string());
		return;
	}
	view_.dialogs.quit_requested = true;
	touch(ViewConcern::Project);
}

void SessionCore::save_preferences() {
	Diagnostic error;
	if (!preferences_.save(error)) report(error);
	const Preferences &settings = preferences_.values();
	view_.project.recent_projects = settings.recent_projects;
	view_.project.retail_directory = game_install();
	view_.project.play_retail = settings.play_in_install;
	view_.project.import_dependencies = settings.import_dependencies;
	touch(ViewConcern::Preferences);
}

std::string SessionCore::game_install() const {
	return view_.project.open ? local_.game_install : preferences_.values().game_install;
}

const RequirementRow *SessionCore::requirement_row(const std::string &role) const {
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view_.project.requirements->rows)
		if (candidate.role == role) row = &candidate;
	return row;
}

// Two files of one name are the scan's asset.name.duplicate: the path named, else the first of
// the name.
const AssetEntry *SessionCore::project_file(const std::string &file) const {
	for (const AssetEntry &candidate : view_.project.scan->entries)
		if (candidate.relative_path == file) return &candidate;
	const std::string wanted = normalized_logical_name(fs::path(file).filename().string());
	for (const AssetEntry &candidate : view_.project.scan->entries)
		if (normalized_logical_name(candidate.logical_name) == wanted) return &candidate;
	return nullptr;
}

// --- the build -------------------------------------------------------------------------------

// The build as an operation (BuildOperation): planned here from the Problems rows as they are,
// then stepped by the polls and landed by the one that sees it done (absorb_build). Unsaved
// edits never reach here: Build and Play wait on the unsaved prompt first (UnsavedGuard), whose
// Save writes them. A build running already served the request at the busy gate (it joined).
void SessionCore::start_build(bool then_play, const std::string &out_dir, bool rehash) {
	if (then_play && play().refused()) return;
	// Where it lands: out_dir taken from the project's folder when relative. One inside the project
	// but in its cache or its export folder (which the scan passes over) would be files of the
	// project the next scan lists, an archive every later build refuses: refused before anything
	// is read.
	std::string output_root = paths_.build_dir + "/play";
	if (!out_dir.empty()) {
		fs::path out(out_dir);
		if (out.is_relative()) out = fs::path(paths_.root) / out;
		out = out.lexically_normal();
		if (inside(out, paths_.root) && !inside(out, paths_.cache_dir) &&
		    !inside(out, paths_.export_dir(*view_.project.document))) {
			view_.activity.status = "The build was refused: its folder is inside the project.";
			report(make_finding(CoreFinding::BuildOutDirInProject, DiagnosticSeverity::Error,
			                    "A build cannot land in " + out.generic_string() +
			                            ": it is inside the project, whose files the next build would pack. "
			                            "Choose a folder outside it, or its export folder."));
			return;
		}
		output_root = out.generic_string();
	}
	problems().clear_build_findings(); // the last build's rows go: this one reports anew
	documents().reload_changed();
	refresh_now();
	// The plan gates on the findings the refresh above just produced (the Problems rows),
	// not on a validation of its own; the build's own findings are those its report adds to
	// these rows (absorb_build), whatever the rows are when it ends.
	BuildPlan plan =
			plan_build(paths_, *view_.project.scan, *view_.project.requirements, problems().gate_findings());
	plan.rehash = rehash;
	// No directory a game runs from is pruned, asked when the build publishes (a game started
	// while it packed counts): this editor's game's, and every one whose lease names a process
	// that may still run (a game left running across an editor restart; one the platform cannot
	// check is kept); a lease whose game is gone is deleted (run/play_lease.h). The operation
	// lives in the session's slot, so the session outlives every call.
	ProtectedDirs protected_dirs = play().protected_dirs(output_root);
	// The build lands under the cache by default, which keeps itself out of the modder's
	// repository, and keeps its hash cache there wherever it lands (BuildPlan::hash_cache); a cache
	// that cannot be made fails the build's own first step when it lands there, which says why, and
	// otherwise only leaves every file to be hashed.
	std::string cache_error;
	ensure_project_cache_dir(paths_, cache_error);
	const uint64_t id = operations_.start(std::make_unique<BuildOperation>(
	        plan, output_root, std::move(protected_dirs), view_.findings.diagnostics, then_play));
	if (id == 0) return refuse_busy(std::string()); // another operation runs, holding nothing it needs
	outcome_.operation = id;
	view_.activity.status = then_play ? "Building, then playing..." : "Building...";
	note("Build started.");
	show_operation();
}

OperationOutcome SessionCore::absorb_build(const BuildReport &result, const std::vector<Diagnostic> &gate,
                                           bool then_play) {
	view_.activity.has_build = true;
	view_.activity.last_build = std::make_shared<const BuildReport>(result);
	// A blocked build's report repeats the findings that blocked it, which were Problems rows
	// when it started: only the ones those rows lacked (the plan's own, the build's) are the
	// build's, whatever an edit made of the rows while it packed, and every validation keeps
	// them until the next build starts or the project closes.
	std::vector<Diagnostic> own;
	for (const Diagnostic &d : result.diagnostics)
		if (std::none_of(gate.begin(), gate.end(), [&d](const Diagnostic &gated) { return same_finding(gated, d); }))
			own.push_back(d);
	for (const Diagnostic &d : own) report(d);
	problems().set_build_findings(own);
	if (result.ok) {
		if (result.reused_existing) {
			note("Build unchanged: " + shown_path(result.build_dir, paths_.root));
			view_.activity.status = "Build unchanged.";
		} else {
			note("Built " + shown_path(result.build_dir, paths_.root) + " (" + std::to_string(result.archives_written.size()) +
			     " archive(s) written, " + std::to_string(result.archives_reused.size()) + " reused, " +
			     std::to_string(result.loose_written.size()) + " loose file(s); " + std::to_string(result.files_hashed) +
			     " file(s) hashed)");
			view_.activity.status = "Build finished.";
		}
	} else {
		note("Build failed.");
		view_.activity.status = "Build failed; see Problems.";
	}
	if (result.ok && then_play) play().start();
	touch(ViewConcern::Operation);
	OperationOutcome outcome;
	outcome.end = result.ok ? OperationEnd::Done : OperationEnd::Failed;
	outcome.findings = std::move(own);
	return outcome;
}

} // namespace opennova::editor
