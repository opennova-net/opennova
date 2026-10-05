#include <editor/session/session_core.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <optional>
#include <set>
#include <utility>

#include <base/gameprofile/gameprofile.h>
#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <editor/assets/install_check.h>
#include <editor/assets/project_scan.h>
#include <editor/blank/create_missing.h>
#include <editor/documents/document_types.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/rename_transaction.h>
#include <editor/model/edit.h>
#include <editor/model/field_text.h>
#include <editor/project/expansion_files.h>
#include <editor/project/expansion_name.h>
#include <editor/project/project_files.h>
#include <editor/preview/texture_thumbnails.h>
#include <editor/session/texture_use_index.h>
#include <editor/preview/viewport_model.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_refresh.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/export_build.h>
#include <editor/run/play_lease.h>
#include <editor/session/build_operation.h>
#include <editor/session/build_result.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/import_controller.h>
#include <editor/session/open_operation.h>
#include <editor/session/play_controller.h>
#include <editor/session/problems_service.h>
#include <editor/session/refresh_operation.h>
#include <editor/session/rename_controller.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
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
		platform_(platform), preferences_(preferences), viewports_(std::make_shared<Viewports>()) {
	view_.documents.viewports = viewports_;
	view_.documents.thumbnails = std::make_shared<TextureThumbnails>();
	// A closed model a texture's uses read is read as a document opens it (DocumentSet::load).
	texture_uses_ = std::make_shared<TextureUseIndex>([this](const std::string &path) -> std::shared_ptr<DocumentBase> {
		const AssetEntry *entry = view_.project.scan ? view_.project.scan->at_path(path) : nullptr;
		Diagnostic error;
		return entry ? documents().load(path, entry->kind, error) : nullptr;
	});
	view_.documents.texture_uses = texture_uses_;
	// What a viewport's follow derives (a menu's held window, a model framed, a clip's clock sought)
	// moves the Viewports concern as a SetViewport does; the follow runs at the Shell's pump, outside
	// any request, so the counter alone moves (nothing is tracked again).
	viewports_->set_on_derived_change([this] { view_.revisions.touch(ViewConcern::Viewports); });
}

void SessionCore::touch(ViewConcern concern) {
	update_preview_targets(view_.documents);
	viewports_->track(view_);
	view_.revisions.touch(concern);
}

void SessionCore::start() {
	// A store that cannot be read, or a settings file set aside (another schema): said, the
	// defaults in effect.
	Diagnostic finding;
	if (!preferences_.load(finding) || !finding.code().empty())
		report(finding);
	const Preferences &settings = preferences_.values();
	view_.project.recent_projects = settings.recent_projects;
	read_recent_details();
	show_installs();
	view_.project.play_retail = settings.play_in_install;
	view_.project.runtime_setting = settings.runtime_executable;
	view_.project.import_dependencies = settings.import_dependencies;
	show_recent_items();
	view_.activity.status = "No project open.";
	read_install_expansions();
	// What the editor's install holds is checked when a field shows it (the welcome page's form asks), not
	// as the editor starts: a mount of the whole install at every start for a line nobody may look at.
	touch(ViewConcern::Preferences);
	touch(ViewConcern::Graph);
	touch(ViewConcern::Output);
}

SessionCore::RequestScope::RequestScope(SessionCore &core, bool background)
	: core_(core), outermost_(!core.in_request_), background_(background) {
	assert(outermost_ && "a request entered the session while another was served");
	if (!outermost_) return;
	core_.outcome_ = ActionOutcome();
	core_.in_request_ = true;
	status_before_ = core_.view_.activity.status;
}

SessionCore::RequestScope::~RequestScope() {
	if (!outermost_) return;
	// A refused request's line is its own: kept until a request is served, which said its own line
	// or, having said nothing, leaves none (ADR 0046 S15). A refusal that said nothing on the line
	// claims none. One the Shell sent of its own (S18: a timer's) leaves it.
	if (core_.outcome_.refused) {
		if (core_.view_.activity.status != status_before_) core_.refusal_status_ = core_.view_.activity.status;
	} else if (!background_ && !core_.refusal_status_.empty()) {
		if (core_.view_.activity.status == core_.refusal_status_) {
			core_.view_.activity.status.clear();
			core_.touch(ViewConcern::Output);
		}
		core_.refusal_status_.clear();
	}
	core_.in_request_ = false;
}

void SessionCore::note(std::string line) {
	view_.activity.output.append(std::move(line));
	touch(ViewConcern::Output);
}

uint64_t SessionCore::note_folded(std::string line, std::vector<std::string> folded) {
	const uint64_t index = view_.activity.output.append_folded(std::move(line), std::move(folded));
	touch(ViewConcern::Output);
	return index;
}

bool SessionCore::fold_into_note(uint64_t index, std::string line, std::vector<std::string> more) {
	if (!view_.activity.output.fold_into(index, std::move(line), std::move(more))) return false;
	touch(ViewConcern::Output);
	return true;
}

void SessionCore::report(const Diagnostic &d) {
	problems().add_reported(d);
	record_outcome(d);
	// A request refused says why on the status line too (ADR 0046 S15), as well as in Output.
	if (in_request_ && d.severity == DiagnosticSeverity::Error) view_.activity.status = d.message;
	note(std::string(diagnostic_severity_label(d.severity)) + ": " + d.message);
}

// A finding raised while serving a request is that request's outcome; one raised by a
// poll (the build finishing, the game's log) belongs to no request.
void SessionCore::record_outcome(const Diagnostic &d) {
	if (!in_request_) return;
	outcome_.findings.push_back(d);
	if (d.severity == DiagnosticSeverity::Error) outcome_.refused = true;
}

// A request refused for what it names: its outcome, the status line and an Output line; no row.
void SessionCore::refuse_request(const Diagnostic &d) {
	record_outcome(d);
	if (in_request_) outcome_.refused = true;
	view_.activity.status = d.message;
	touch(ViewConcern::Output);
	note(std::string(diagnostic_severity_label(d.severity)) + ": " + d.message);
}

void SessionCore::refuse_request(CoreFinding code, const std::string &message, const std::string &asset,
                                 DiagnosticSeverity severity) {
	refuse_request(make_finding(code, severity, message, asset));
}

// A request that cannot run now (a build is packing the project's files, the document it
// names is not open): nothing is wrong with the project, so the row is a warning, but the
// request did nothing and its outcome says so.
void SessionCore::refuse_now(CoreFinding code, const std::string &message, const std::string &asset) {
	report(make_finding(code, DiagnosticSeverity::Warning, message, asset));
	if (!in_request_) return;
	outcome_.refused = true;
	view_.activity.status = message;
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
	const bool plan = running->kind() == OperationKind::ImportPlan;
	if (!operations_.cancel()) {
		if (asked) refuse_now(CoreFinding::OperationNotCancellable, "This cannot be cancelled now: wait for " + noun + " to finish.");
		return false;
	}
	// An import's plan cancelled: the dialog plans no more (a plan that takes its place says so again).
	if (plan && view_.dialogs.import_preview.planning) {
		view_.dialogs.import_preview.planning = false;
		touch(ViewConcern::Dialogs);
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
// gameprofile has, an expansion the game cannot take or the install refuses) is asked before anything
// changes: the open project stays open, its operation running. Then the open project closes, its
// operation cancelled (refused, nothing made, when it cannot be), and only then is the folder made,
// titled `title` or else, left empty, after the folder (create_project's rule, the one every path to a
// new project takes).
bool SessionCore::new_project(const std::string &dir, const std::string &title, const std::string &game,
                              bool import_pass, const ProjectExpansion &expansion, const std::string &game_install) {
	if (dir.empty()) return false;
	view_.project.refused.clear();
	const std::string target_game = game.empty() ? std::string(kDefaultTargetGame) : game;
	ProjectDocument doc;
	Diagnostic error;
	const auto refused = [this](const Diagnostic &why) {
		refuse_project(why, "The project could not be created");
		return false;
	};
	if (!can_create_project(dir, target_game, error, expansion)) return refused(error);
	// The install the form names (the UX round's project lane): a folder that holds the game, which the
	// project is made on and the editor's install from then on (written once the project is made); another is
	// refused, nothing made. None named: the editor's, as an Open seeds a local.json that names none.
	std::string chosen;
	if (!game_install.empty()) {
		view_.project.install_check = editor::check_install(game_install, target_game);
		touch(ViewConcern::Preferences);
		if (!view_.project.install_check.ok())
			return refused(make_finding(CoreFinding::ProjectInstallInvalid, DiagnosticSeverity::Error,
			                            view_.project.install_check.words()));
		chosen = view_.project.install_check.root;
	}
	// The expansion against the game install the new project opens with (the one named, else the editor's,
	// whatever install an open project names): a name it has already, one to build on it lacks, a name whose
	// files are those of one of its missions (ADR 0046 S16). With no install, nothing to weigh.
	const std::string install_root = chosen.empty() ? absolute_install_path(preferences_.values().game_install) : chosen;
	if (!install_root.empty() && !expansion.standalone()) {
		std::vector<Diagnostic> install;
		expansion_install_findings(expansion, vfs_list_expansions(install_root), DiagnosticSeverity::Error, install);
		if (!install.empty()) return refused(install.front());
		ProjectDocument made;
		made.target_game = target_game;
		made.expansion = expansion;
		const std::string mission = expansion_name_mission_problem(expansion.name, list_retail_file_names(install_root, made));
		if (!mission.empty())
			return refused(make_finding(CoreFinding::ProjectFieldInvalid, DiagnosticSeverity::Error, mission));
	}
	if (!close_project()) return false;
	if (!create_project(dir, title, target_game, doc, error, expansion)) return refused(error);
	note("Created " + doc.title + ".");
	// The install named: the project's (its local.json, which the Open reads), and the editor's from now on.
	// A preference that cannot be written is said; the project keeps the install all the same.
	if (!chosen.empty()) {
		LocalSettings local;
		local.game_install = chosen;
		Diagnostic unwritten;
		if (!save_local_settings(ProjectPaths::for_root(dir), local, unwritten)) report(unwritten);
		if (absolute_install_path(preferences_.values().game_install) != chosen) {
			Preferences next = preferences_.values();
			next.game_install = chosen;
			if (!preferences_.write(next, unwritten)) report(unwritten);
		}
	}
	// An expansion's own files a new project makes (ADR 0046 S16): its version text always, its text
	// table on the base game (one that builds on an installed expansion imports that one's).
	if (!doc.expansion.standalone()) {
		std::vector<std::string> roles{ expansion_file_row(ExpansionFileRole::Version).manifest_role };
		if (doc.expansion.builds_on.empty()) roles.push_back(expansion_file_row(ExpansionFileRole::Table).manifest_role);
		const ProjectPaths paths = ProjectPaths::for_root(dir);
		const CreateMissingResult made =
				create_missing_requirements(paths, doc, evaluate_requirements(doc, scan_project_assets(paths, doc)), roles);
		for (const std::string &path : made.created) note("Created " + path);
		for (const Diagnostic &d : made.diagnostics) report(d);
	}
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
	view_.project.refused.clear();
	ProjectDocument doc;
	Diagnostic error;
	if (!::opennova::editor::open_project(dir, doc, error)) {
		// Taken off the recent projects, and said so where the welcome page shows why.
		const bool listed = std::find(view_.project.recent_projects.begin(), view_.project.recent_projects.end(), dir) !=
		                    view_.project.recent_projects.end();
		if (listed) {
			preferences_.forget_recent_project(dir);
			save_preferences();
		}
		refuse_project(error, listed ? "The project could not be opened, and it is off the recent projects now"
		                             : "The project could not be opened");
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
	// The session's own install over local.json's, kept apart from it so nothing writes it there (a close's
	// workspace, a setting's Apply).
	run_install_ = open.run_install().empty() ? std::string() : absolute_install_path(open.run_install());
	view_.project.refused.clear();
	view_.project.build_folder = local_.build_folder;
	view_.project.open = true;
	view_.project.root = paths_.root;
	view_.project.document = std::make_shared<const ProjectDocument>(open.document());
	view_.activity.has_build = false;
	view_.activity.last_build = std::make_shared<const BuildReport>();
	view_.activity.has_export = false;
	view_.activity.last_export = std::make_shared<const ExportReport>();
	preferences_.remember_recent_project(paths_.root);
	save_preferences();
	read_recent_details(view_.project.document.get(), paths_.root); // its entry as it is now, whatever was there
	view_.activity.runtime_executable = play().resolve_runtime_executable();
	imports().set_install_files(std::move(open.install_files()), std::move(open.base_files()));
	read_install_expansions(); // the project's install's, before its requirements weigh them
	absorb_refresh(open.refresh());
	restore_workspace(); // the documents it was left with (the UX round's project lane)
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
	remember_workspace(); // what it reopens with (the UX round's project lane)
	const std::string title = view_.project.document->title;
	const std::shared_ptr<const ProjectDocument> closing = view_.project.document;
	const std::string closing_root = paths_.root;
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
	view_.documents.thumbnails->clear();
	texture_uses_->clear();
	view_.project.root.clear();
	view_.project.document = std::make_shared<const ProjectDocument>();
	view_.project.scan = std::make_shared<const AssetScan>();
	view_.project.requirements = std::make_shared<const RequirementReport>();
	view_.findings.diagnostics.clear();
	view_.findings.marks.reset();
	view_.activity.last_rename = ActivityView::LastRename(); // its way back is this project's
	renames().forget();
	view_.activity.has_build = false;
	view_.activity.last_build = std::make_shared<const BuildReport>();
	view_.activity.has_export = false;
	view_.activity.last_export = std::make_shared<const ExportReport>();
	// The last build's findings are this project's and go with it.
	problems().clear_build_findings();
	paths_ = ProjectPaths();
	local_ = LocalSettings();
	run_install_.clear();
	view_.project.build_folder.clear();
	view_.activity.runtime_executable = play().resolve_runtime_executable();
	show_installs();
	// The welcome page's, with the project as it closed (renamed in its settings), from its document.
	read_recent_details(closing.get(), closing_root);
	show_recent_items(); // the project's game's go with it
	touch(ViewConcern::Preferences);
	read_install_expansions();
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
			requirements_of(*view_.project.document, *view_.project.scan));
	touch(ViewConcern::Files);
	problems().show_requirements(); // the rows they lead with, at once
	// A whole refresh (an open, a Rescan, a Reimport) makes a new scan, on which the game's own data's
	// baseline looks at the install's folder again (S15: a patch over it is validated again).
	problems().validate_later();
	// The open documents whose files the refresh changed (an import's outputs made again: S18, a texture
	// a program saved the source of) read again; one with unsaved edits is a conflict, as on a Rescan.
	documents().reload_changed();
	return imports;
}

bool SessionCore::start_changed_refresh(ExternalChanges changes) {
	const uint64_t id =
			start_operation(std::make_unique<ChangedSourcesOperation>(paths_, *view_.project.document, std::move(changes)));
	if (id == 0) {
		refuse_busy(std::string());
		return false;
	}
	outcome_.operation = id;
	return true;
}

void SessionCore::absorb_changed(ImportRunResult &imports, const std::vector<std::string> &files) {
	std::vector<ImportedSource> sources = view_.project.imports ? *view_.project.imports : std::vector<ImportedSource>();
	std::vector<std::string> paths = files;
	std::set<std::string> refreshed;
	for (ImportedSource &source : imports.sources) {
		refreshed.insert(source.source);
		refreshed.insert(source.sidecar);
		paths.push_back(source.source); // its visit lists its outputs, the old and the new
		if (source.reimported)
			note("Imported " + source.source + " (" + std::to_string(source.outputs.size()) + " file" +
			     (source.outputs.size() == 1 ? "" : "s") + ")");
		const auto known = std::find_if(sources.begin(), sources.end(),
		                                [&](const ImportedSource &each) { return each.source == source.source; });
		if (known != sources.end()) *known = std::move(source);
		else sources.push_back(std::move(source));
	}
	std::sort(sources.begin(), sources.end(), [](const ImportedSource &a, const ImportedSource &b) { return a.source < b.source; });
	view_.project.imports = std::make_shared<const std::vector<ImportedSource>>(std::move(sources));
	// The pass's findings on those sources in place of the ones the last pass made of them.
	AssetScan scan = *view_.project.scan;
	std::vector<Diagnostic> findings;
	for (const Diagnostic &d : scan.import_findings())
		if (!refreshed.count(d.asset)) findings.push_back(d);
	findings.insert(findings.end(), imports.diagnostics.begin(), imports.diagnostics.end());
	scan.set_import_findings(std::move(findings));
	files_scanned_ = scan.update(paths_, *view_.project.document, paths);
	view_.project.scan = std::make_shared<const AssetScan>(std::move(scan));
	problems().set_scan(paths_.root, *view_.project.scan, view_.project.document->target_game);
	view_.project.requirements = std::make_shared<const RequirementReport>(
			requirements_of(*view_.project.document, *view_.project.scan));
	touch(ViewConcern::Files);
	problems().show_requirements(); // the rows they lead with, at once
	problems().validate_later();
	// The open documents of what changed (a PNG its program saved, an output made again) read again; one with
	// unsaved edits is a conflict, as on a Rescan.
	documents().reload_changed();
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
			requirements_of(*view_.project.document, *view_.project.scan));
	touch(ViewConcern::Files);
	problems().show_requirements(); // the rows they lead with, at once
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
	// The project's expansion (ADR 0046 S16): the game's rule for it, then the game install's
	// expansions (the install this Apply leaves in effect: a name it has, one to build on it lacks),
	// each refusal a failure the result carries, the expansion as it was. Each half is weighed against
	// the install only when it changes: a name the install has since (the project's own export copied
	// in) does not refuse a change of what it builds on, nor an install lacking the expansion it builds
	// on a change of its name (an open project lists both). A name changed renames the project's own
	// files of the old name's with the document's save, all or nothing (rename_expansion_files).
	ProjectExpansion expansion = project.expansion;
	if (change.expansion) expansion.name = *change.expansion;
	if (change.builds_on) expansion.builds_on = *change.builds_on;
	bool expansion_changed = false;
	const std::string expansion_was = project.expansion.name;
	if (expansion != project.expansion) {
		Diagnostic why;
		std::vector<Diagnostic> install;
		const std::string install_after =
				change.game_install ? absolute_install_path(*change.game_install) : game_install();
		if (!install_after.empty())
			expansion_install_findings(expansion,
			                           change.game_install ? vfs_list_expansions(install_after) : install_expansion_names_,
			                           DiagnosticSeverity::Error, install);
		const bool name_moved = expansion.name != project.expansion.name;
		const bool base_moved = !strutil::iequals(expansion.builds_on, project.expansion.builds_on);
		install.erase(std::remove_if(install.begin(), install.end(),
		                             [&](const Diagnostic &d) {
			                             return (d.code() == "project.expansion.name_taken" && !name_moved) ||
			                                    (d.code() == "project.expansion.not_installed" && !base_moved);
		                             }),
		              install.end());
		// A new name against the missions the game would list with it (the install's view as the project
		// would import it, and the project's own files).
		std::string mission_problem;
		if (!expansion.name.empty() && !strutil::iequals(expansion.name, project.expansion.name)) {
			ProjectDocument after = project;
			after.expansion = expansion;
			std::vector<std::string> names =
					install_after.empty() ? std::vector<std::string>() : list_retail_file_names(install_after, after);
			for (const AssetEntry &entry : view_.project.scan->entries) names.push_back(entry.logical_name);
			mission_problem = expansion_name_mission_problem(expansion.name, names);
		}
		if (!check_project_expansion(project.target_game, expansion, why)) {
			failures.push_back(why);
		} else if (!mission_problem.empty()) {
			failures.push_back(make_finding(CoreFinding::ProjectFieldInvalid, DiagnosticSeverity::Error, mission_problem));
		} else if (!install.empty()) {
			failures.insert(failures.end(), install.begin(), install.end());
		} else if (busy_for(HoldsNothing, HoldsProject)) {
			refuse_part("before changing the project's expansion");
		} else {
			project.expansion = expansion;
			project_changed = expansion_changed = true;
		}
	}
	if (project_changed && !view_.project.open) {
		failures.push_back(make_finding(CoreFinding::ProjectNone, DiagnosticSeverity::Error,
		                                "Open a project to change its name, its features or its expansion."));
		project_changed = features_changed = expansion_changed = false;
	}
	if (project_changed) {
		Diagnostic error;
		// The project's own expansion files follow a new name (none to rename to or from a standalone
		// project, none for a change of the name's case alone, which the game reads the same).
		const bool rename_files = expansion_changed && !expansion_was.empty() && !project.expansion.name.empty() &&
		                          !strutil::iequals(expansion_was, project.expansion.name);
		bool saved = false;
		if (rename_files) {
			saved = rename_expansion_files(expansion_was, project.expansion.name, project, failures);
		} else {
			saved = save_project_document(paths_.project_file, project, error);
			if (!saved) failures.push_back(error);
		}
		if (saved) {
			view_.project.document = std::make_shared<const ProjectDocument>(project);
			if (expansion_changed) {
				// The install as the project imports it moved (install_view.h): its names are found again
				// (the base's first, which the requirements' words read), and what it makes of its own data
				// once the validation below asks for it (OriginalFiles::want, which validates the install
				// again for another expansion).
				imports().refresh_install_files();
			}
			if (features_changed || expansion_changed) {
				// The requirements follow the features and the expansion, over the files as the scan
				// lists them.
				view_.project.requirements = std::make_shared<const RequirementReport>(
						requirements_of(project, *view_.project.scan));
				touch(ViewConcern::Files);
				problems().show_requirements(); // the rows they lead with, at once
				problems().validate_later();
			}
		} else {
			project_changed = expansion_changed = features_changed = false;
		}
	}
	// The game install is the open project's (ADR 0046 d6/d10), written to its local.json,
	// which opennova-project reads too; the editor's machine setting keeps the install last
	// chosen, where a project that names none starts. Both keep it absolute: a relative path
	// is taken from the editor's working directory, not from wherever the command line runs.
	std::optional<std::string> install =
	        change.game_install ? std::optional<std::string>(absolute_install_path(*change.game_install))
	                                : std::nullopt;
	if (install && view_.project.open && *install != game_install() && busy_for(HoldsNothing, HoldsProject)) {
		refuse_part("before changing the game install");
		install.reset();
	}
	bool install_changed = false;
	if (install && view_.project.open && *install != game_install()) {
		// The install asked for takes the place of a session's own (open_project's game_install): the
		// project's from now on, written to its local.json unless it names it already.
		if (*install == local_.game_install) {
			run_install_.clear();
			install_changed = true;
		} else {
			LocalSettings local = local_;
			local.game_install = *install;
			Diagnostic error;
			if (save_local_settings(paths_, local, error)) {
				local_ = std::move(local);
				run_install_.clear();
				install_changed = true;
			} else {
				failures.push_back(error);
			}
		}
	}
	// The folder Build to folder builds into, kept with the project's local settings (Build > Build to <it>): the
	// modder's pick, never what a build request's out_dir names (a script's build keeps nothing, L4).
	if (change.build_folder && !view_.project.open) {
		failures.push_back(make_finding(CoreFinding::ProjectNone, DiagnosticSeverity::Error,
		                                "Open a project to keep a folder to build it into."));
	} else if (change.build_folder && !change.build_folder->empty() &&
	           inside(path_of(*change.build_folder).lexically_normal(), path_of(paths_.root))) {
		failures.push_back(make_finding(CoreFinding::BuildOutDirInProject, DiagnosticSeverity::Error,
		                                "A folder to build into for players lies outside the project: " +
		                                        *change.build_folder + " is inside it."));
	} else if (change.build_folder && *change.build_folder != local_.build_folder) {
		LocalSettings local = local_;
		local.build_folder = *change.build_folder;
		Diagnostic error;
		if (save_local_settings(paths_, local, error)) {
			local_ = std::move(local);
			view_.project.build_folder = local_.build_folder;
			touch(ViewConcern::Preferences);
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
	view_.project.editor_install = preferences_.values().game_install;
	if (game_install() != view_.project.retail_directory) {
		show_installs();
		touch(ViewConcern::Preferences);
		imports().refresh_install_files();
		read_install_expansions();
		// The project's expansion weighed against the install's (listed).
		if (view_.project.open) {
			view_.project.requirements =
					std::make_shared<const RequirementReport>(requirements_of(*view_.project.document, *view_.project.scan));
			touch(ViewConcern::Files);
			problems().show_requirements(); // the rows they lead with, at once
			problems().validate_later();
		}
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
	const CreateMissingResult result = create_missing_requirements(paths_, doc, requirements_of(doc, now), roles);
	for (const std::string &path : result.created) note("Created " + path);
	for (const std::string &name : result.unavailable) {
		note("The editor cannot create " + name + " yet: no writer exists for this kind of file.");
	}
	now.update(paths_, doc, result.created);
	view_.project.scan = std::make_shared<const AssetScan>(std::move(now));
	problems().set_scan(paths_.root, *view_.project.scan, doc.target_game);
	view_.project.requirements = std::make_shared<const RequirementReport>(requirements_of(doc, *view_.project.scan));
	touch(ViewConcern::Files);
	problems().show_requirements(); // the rows they lead with, at once
	problems().validate_later();
	for (const Diagnostic &d : result.diagnostics) report(d);
	if (result.created.empty() && result.unavailable.empty() && result.diagnostics.empty()) {
		view_.activity.status = "Nothing to create.";
	} else {
		view_.activity.status = counted(result.created.size(), "file") + " created" +
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

void SessionCore::check_install(const std::string &path) {
	const std::string asked = path.empty() ? preferences_.values().game_install : path;
	const std::string game = view_.project.open ? view_.project.document->target_game : std::string(kDefaultTargetGame);
	view_.project.install_check = editor::check_install(asked, game);
	touch(ViewConcern::Preferences);
}

void SessionCore::remember_workspace() {
	if (!workspace_kept_ || !view_.project.open) return;
	// A project whose folder moved or went while it was open: nothing written at its old path.
	std::error_code gone;
	if (!fs::is_regular_file(system_path(paths_.project_file), gone)) return;
	std::vector<LocalSettings::OpenDocument> open;
	for (const auto &[path, locator] : documents().open_with_selection()) open.push_back({path, locator});
	const std::string active = open.empty() ? std::string() : view_.documents.active;
	if (open == local_.open_documents && active == local_.active_document) return;
	// The workspace's fields alone, over the file as it is: nothing else of the session's goes into it.
	LocalSettings next;
	Diagnostic unread;
	if (!load_local_settings(paths_, next, unread) || !unread.code().empty()) next = local_;
	next.open_documents = open;
	next.active_document = active;
	Diagnostic error;
	if (!save_local_settings(paths_, next, error)) {
		report(error);
		return;
	}
	local_.open_documents = std::move(open);
	local_.active_document = active;
}

void SessionCore::restore_workspace() {
	if (!workspace_kept_) return;
	size_t reopened = 0;
	for (const LocalSettings::OpenDocument &open : local_.open_documents) {
		const AssetEntry *file = view_.project.scan->at_path(open.path);
		if (!file || !is_editable_kind(file->kind)) continue;
		EditorRequest request = request::open_document(file->relative_path);
		request.locator = open.locator;
		documents().open_document(request);
		reopened += documents().document_for(file->relative_path) ? 1 : 0;
	}
	if (!local_.active_document.empty() && documents().document_for(local_.active_document))
		documents().open_document(request::open_document(local_.active_document));
	if (reopened) note("Reopened " + counted(reopened, "file") + " as the project was left.");
}

// Each recent project's title, game and expansion, as its project file has them (a folder that no longer
// holds one: not found): an entry the details hold kept as it is, a root new to the list read once, the open
// project's from its document.
void SessionCore::read_recent_details(const ProjectDocument *open, const std::string &open_root) {
	const auto worded = [](const std::string &root, const ProjectDocument &doc) {
		ProjectView::RecentProject recent;
		recent.root = root;
		recent.found = true;
		recent.title = doc.title;
		const gameprofile::GameProfile *profile = gameprofile::gameprofile_by_code(doc.target_game.c_str());
		recent.game = profile ? profile->display_name : doc.target_game;
		recent.expansion = doc.expansion.name;
		recent.builds_on = doc.expansion.builds_on;
		return recent;
	};
	std::vector<ProjectView::RecentProject> details;
	for (const std::string &root : view_.project.recent_projects) {
		if (open && root == open_root) {
			details.push_back(worded(root, *open));
			continue;
		}
		const auto known = std::find_if(view_.project.recent_details.begin(), view_.project.recent_details.end(),
		                                [&root](const ProjectView::RecentProject &each) { return each.root == root; });
		if (known != view_.project.recent_details.end()) {
			details.push_back(*known);
			continue;
		}
		ProjectDocument doc;
		Diagnostic error;
		if (::opennova::editor::open_project(root, doc, error)) {
			details.push_back(worded(root, doc));
		} else {
			ProjectView::RecentProject recent;
			recent.root = root;
			details.push_back(std::move(recent));
		}
	}
	view_.project.recent_details = std::move(details);
}

void SessionCore::refuse_project(const Diagnostic &why, const std::string &what) {
	report(why);
	view_.activity.status = what + ": " + why.message;
	view_.project.refused = view_.activity.status;
	touch(ViewConcern::Project);
	touch(ViewConcern::Output);
}

void SessionCore::show_installs() {
	view_.project.retail_directory = game_install();
	view_.project.editor_install = preferences_.values().game_install;
}

void SessionCore::clear_output() {
	view_.activity.output.clear();
	touch(ViewConcern::Output);
}

std::string SessionCore::viewport_document(const std::string &path) {
	const DocumentBase *document = documents().document_for(path);
	return document ? document->path() : path;
}

void SessionCore::set_viewport(const std::string &path, const std::string &change) {
	io::JsonValue json;
	std::string error;
	const std::string at = viewport_document(path);
	if (!io::json_parse(change, json, error)) {
		error = "The viewport's change is not JSON: " + error;
	} else if (path.empty() && json.is_object() && json.object.size() == 1 && json.get("clock")) {
		// The clock alone, named by no document: the one preview clock every viewport reads, whatever
		// document is active (none, or one that shows in no viewport).
		if (viewports_->set_clock(*json.get("clock"), error)) touch(ViewConcern::Viewports);
	} else if (viewports_->set(view_, at, json, error)) {
		touch(ViewConcern::Viewports);
	}
	// The change's own fault: refused, no Problems row (the demo round's review).
	if (!error.empty()) refuse_request(CoreFinding::ViewportRefused, error, at, DiagnosticSeverity::Error);
}

SessionCore::WireDrag *SessionCore::wire_drag(const std::string &path) {
	const auto held = wire_drags_.find(path);
	if (held == wire_drags_.end()) return nullptr;
	const OpenGesture &open = open_gesture(path);
	if (open.wire() && open.token == held->second.token) return &held->second;
	wire_drags_.erase(held); // its gesture ended another way (an Undo, a Save, another gesture)
	return nullptr;
}

void SessionCore::end_wire_gesture(const std::string &path) {
	wire_drags_.erase(path);
	documents().end_edit(path);
}

void SessionCore::edit_in_viewport(const EditorRequest &request) {
	// What the viewport plans, as its canvas would raise it.
	struct Planned final : CanvasRequests {
		std::vector<EditorRequest> requests;
		void request(EditorRequest each) override { requests.push_back(std::move(each)); }
	} planned;
	const ViewportDrag &asked = request.drag;
	const bool drag = asked != ViewportDrag(), command = request.command != ViewportCommand(),
			   drop = request.drop != ViewportDrop();
	const std::string at = request.path.empty() ? view_.documents.active : viewport_document(request.path);
	// The gesture a drag names, its answer's whether the drag is refused or not.
	if (drag) outcome_.gesture = asked.gesture;
	std::string error;
	// A sample naming a gesture goes on with one the wire opened in the document, of the same record's
	// handle.
	const WireDrag *going = nullptr;
	if (int(drag) + int(command) + int(drop) != 1) {
		error = "edit_in_viewport names a drag, a command or a drop, one of them.";
	} else if (drag && asked.gesture && documents().document_for(at)) {
		going = wire_drag(at);
		if (!going || going->token != asked.gesture) {
			going = nullptr;
			error = "No open gesture " + std::to_string(asked.gesture) + " on " + at +
					": a gesture's samples are consecutive drags of one handle on its document (any other "
					"request on it, another gesture, or 10 s with no sample ends it).";
		} else if (asked.id != going->id || asked.handle != going->handle ||
				(asked.kind != ViewportKind::kCount && asked.kind != going->kind)) {
			error = "Gesture " + std::to_string(asked.gesture) + " drags record " + std::to_string(going->id) +
					"'s " + going->handle + " handle in the " + viewport_kind_token(going->kind) + " viewport.";
		}
	}
	// What the sample plans. A gesture's sample goes from the point its samples took the handle to, as
	// a canvas drags from its press, so a snapped gesture lands where the canvas's would; one that stays
	// open takes its token now, so it is open even where its first sample plans nothing.
	ViewportDrag sample = asked;
	float x = asked.x, y = asked.y; // the point this sample takes the handle to
	ViewportKind shown = ViewportKind::kCount;
	if (error.empty()) {
		const ViewportKind named = drop ? request.drop.kind : !drag ? request.command.kind : going ? going->kind : asked.kind;
		if (const ViewportModel *viewport = viewports_->resolve(view_, at, named, error)) {
			shown = viewport->kind();
			const ViewportContext context = viewport_context(view_, *viewport, drag ? asked.snap : 0.0f);
			if (drop) {
				viewport->drop(context, request.drop, planned, error);
			} else if (!drag) {
				viewport->command_of(context, request.command, planned, error);
			} else {
				if (going && asked.by) {
					x = going->x + asked.x;
					y = going->y + asked.y;
				} else if (!going && !asked.end) {
					sample.gesture = next_edit_gesture();
					float hx = 0.0f, hy = 0.0f;
					if (asked.by && viewport->handle_point(context, asked.id, asked.handle, hx, hy, error)) {
						x = hx + asked.x;
						y = hy + asked.y;
					}
				}
				// A step that moves nothing keeps its `by` (it plans nothing); a gesture's other samples go
				// to the point they take the handle to.
				if (going && !(asked.by && asked.x == 0.0f && asked.y == 0.0f)) {
					sample.by = false;
					sample.x = x;
					sample.y = y;
				}
				if (error.empty()) viewport->drag(context, sample, planned, error);
			}
		}
	}
	if (!error.empty()) {
		refuse_request(CoreFinding::ViewportRefused, error, at, DiagnosticSeverity::Error);
		// A refused last sample of an open gesture ends it all the same.
		if (going && asked.end) end_wire_gesture(at);
		return;
	}
	// The gesture the drag's batch carries (the one it went on with, the session's for one kept open,
	// or the new one its planner took), which the gesture's next sample names.
	if (drag) {
		outcome_.gesture = sample.gesture;
		for (const EditorRequest &each : planned.requests)
			if (each.kind == EditorRequestKind::EditRecord && !each.edits.empty()) outcome_.gesture = each.edits.front().gesture;
	}
	// Served in order, as the parts serve what they compose (never through handle()): each meets its
	// own row's gate, and its findings are this request's outcome.
	for (const EditorRequest &each : planned.requests) serve_request(*this, each);
	// An item placed is among the recently placed (ADR 0046 S15: the Place tool's palette lists them
	// first), kept with the editor's preferences.
	if (drop && !outcome_.refused && request.drop.reference == "item" && !request.drop.box)
		if (const std::optional<int> item = strutil::parse_int(request.drop.name)) remember_recent_item(*item);
	if (!drag) return;
	// The gesture ends with this sample (its EndEdit served), or stays open for the next: the
	// document's open one, its last sample's time kept, and the point this sample took the handle to
	// where the next goes from once its batch went through (a first sample that did not go through
	// opens none).
	if (asked.end) {
		wire_drags_.erase(at);
		return;
	}
	if (!going && outcome_.refused) {
		outcome_.gesture = 0;
		return;
	}
	WireDrag &kept = wire_drags_[at];
	if (!going) {
		kept = WireDrag{ sample.gesture, shown, asked.id, asked.handle, x, y };
	} else if (!outcome_.refused) {
		kept.x = x;
		kept.y = y;
	}
	documents().open_gesture(at, kept.token, platform_.now_ms());
}

void SessionCore::request_arrives(const EditorRequest &request) {
	const std::string on = !request.path.empty() ? viewport_document(request.path)
			: request_kind_row(request.kind).names_active ? view_.documents.active
			: std::string();
	if (on.empty() || !open_gesture(on).wire()) return;
	const bool goes_on = request.kind == EditorRequestKind::EditInViewport && request.drag != ViewportDrag() &&
			request.drag.gesture == open_gesture(on).token;
	if (!goes_on) end_wire_gesture(on);
}

void SessionCore::lapse_wire_gestures() {
	const int64_t now = platform_.now_ms();
	std::vector<std::string> lapsed;
	for (const OpenGesture &gesture : view_.documents.gestures)
		if (gesture.wire() && now - gesture.sampled_ms >= kWireGestureLapseMs) lapsed.push_back(gesture.path);
	for (const std::string &path : lapsed) end_wire_gesture(path);
	// The drags whose gestures ended another way.
	for (auto it = wire_drags_.begin(); it != wire_drags_.end();) {
		const OpenGesture &open = open_gesture(it->first);
		it = open.wire() && open.token == it->second.token ? std::next(it) : wire_drags_.erase(it);
	}
}

// The running operation goes first (a build's staging directory with it); one that cannot be
// cancelled keeps the editor open.
void SessionCore::quit() {
	if (!cancel_operation(false)) {
		refuse_busy(std::string());
		return;
	}
	save_recent_items();
	remember_workspace(); // what the project reopens with (the UX round's project lane)
	view_.dialogs.quit_requested = true;
	touch(ViewConcern::Project);
}

void SessionCore::save_preferences() {
	Diagnostic error;
	recent_items_unsaved_ = false; // this save keeps them
	if (!preferences_.save(error)) report(error);
	const Preferences &settings = preferences_.values();
	view_.project.recent_projects = settings.recent_projects;
	read_recent_details(); // a root new to the list read, the others kept
	show_installs();
	view_.project.play_retail = settings.play_in_install;
	view_.project.import_dependencies = settings.import_dependencies;
	show_recent_items();
	touch(ViewConcern::Preferences);
}

void SessionCore::remember_recent_item(int64_t item) {
	// In effect at once (the palette lists it first); written at the next poll, outside the request that
	// placed it, and not at all when it was first already. Kept under the open project's game: an id
	// names another item in another game's catalogs.
	if (!view_.project.open || !preferences_.remember_recent_item(recent_items_game(*view_.project.document), item))
		return;
	show_recent_items();
	touch(ViewConcern::Preferences);
	recent_items_unsaved_ = true;
}

void SessionCore::show_recent_items() {
	view_.project.recent_items = view_.project.open ? preferences_.recent_items(recent_items_game(*view_.project.document))
	                                                : std::vector<int64_t>();
}

void SessionCore::save_recent_items() {
	if (!recent_items_unsaved_) return;
	recent_items_unsaved_ = false;
	// A placement already went through: a settings file that cannot be written is a note, never the
	// placement's refusal.
	Diagnostic error;
	if (!preferences_.save(error))
		note("note: the recently placed items could not be kept with the editor's settings: " + error.message);
}

std::string SessionCore::game_install() const {
	if (!view_.project.open) return preferences_.values().game_install;
	return run_install_.empty() ? local_.game_install : run_install_;
}

void SessionCore::read_install_expansions() {
	const auto read = [](const std::string &install, std::vector<std::string> &names) {
		names = install.empty() ? std::vector<std::string>() : vfs_list_expansions(install);
		std::vector<ProjectView::InstallExpansion> expansions;
		for (const std::string &name : names) {
			const ExpansionInfo info = vfs_expansion_info(install, name);
			expansions.push_back({ name, info.name, info.description });
		}
		return expansions;
	};
	const std::string install = game_install();
	view_.project.install_expansions = read(install, install_expansion_names_);
	const std::string chosen = absolute_install_path(preferences_.values().game_install);
	std::vector<std::string> chosen_names;
	view_.project.new_project_expansions =
			chosen == install ? view_.project.install_expansions : read(chosen, chosen_names);
	touch(ViewConcern::Preferences);
}

RequirementReport SessionCore::requirements_of(const ProjectDocument &doc, const AssetScan &scan) const {
	return evaluate_requirements(doc, scan, game_install().empty() ? nullptr : &install_expansion_names_,
	                             &view_.project.base_files);
}

bool SessionCore::rename_expansion_files(const std::string &from, const std::string &to, const ProjectDocument &project,
                                         std::vector<Diagnostic> &failures) {
	// Every rename planned and checked before anything moves: the graph brought to the files first, so
	// a file another file names is found (the game forms these names itself; none of its data names
	// them, so a site refuses rather than half-rewrites).
	problems().validate_pending();
	struct Move {
		std::string from, to; // project-relative
	};
	std::vector<Move> moves;
	std::vector<Diagnostic> refusals;
	for (const ExpansionFile &file : expansion_files(from)) {
		if (file.row->fixed) continue;
		const AssetEntry *held = view_.project.scan->find(file.name);
		if (!held) continue;
		const std::string name = expansion_file_name(*file.row, to);
		const RenamePlan plan = plan_rename(paths_, *view_.project.scan, problems().graph(), held->relative_path, name);
		refusals.insert(refusals.end(), plan.refusals.begin(), plan.refusals.end());
		if (plan.ok() && !plan.sites.empty())
			refusals.push_back(make_finding(CoreFinding::RenameSite, DiagnosticSeverity::Error,
			                                plan.sites.front().file + " names " + held->logical_name +
			                                        ": rename it there first, then change the expansion's name.",
			                                plan.sites.front().file));
		const DocumentBase *open = documents().document_for(held->relative_path);
		if (open && open->dirty())
			refusals.push_back(make_finding(CoreFinding::RenameConflict, DiagnosticSeverity::Error,
			                                held->relative_path + " has unsaved edits: save or close it, then change "
			                                                      "the expansion's name.",
			                                held->relative_path));
		moves.push_back({ held->relative_path, plan.new_path });
	}
	if (!refusals.empty()) {
		failures.insert(failures.end(), refusals.begin(), refusals.end());
		return false;
	}
	// Then each file moved, the document saved last; whatever fails puts back what moved before it.
	std::vector<const Move *> moved;
	const auto put_back = [&]() {
		for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
			std::error_code ec;
			fs::rename(system_path(join_path(paths_.root, (*it)->to)), system_path(join_path(paths_.root, (*it)->from)), ec);
		}
	};
	for (const Move &move : moves) {
		std::error_code ec;
		fs::rename(system_path(join_path(paths_.root, move.from)), system_path(join_path(paths_.root, move.to)), ec);
		if (ec) {
			put_back();
			failures.push_back(make_finding(CoreFinding::RenameMove, DiagnosticSeverity::Error,
			                                "Could not rename " + move.from + " to " + basename_of(move.to) + ": " +
			                                        ec.message() + ". Nothing was changed.",
			                                move.from));
			return false;
		}
		moved.push_back(&move);
	}
	Diagnostic error;
	if (!save_project_document(paths_.project_file, project, error)) {
		put_back();
		failures.push_back(error);
		return false;
	}
	// In: the files read again under their new names, an open one opened again there (the active one
	// staying active).
	std::vector<std::string> touched;
	std::vector<std::string> reopen;
	std::string active_now;
	for (const Move &move : moves) {
		touched.push_back(move.from);
		touched.push_back(move.to);
		if (DocumentBase *open = documents().document_for(move.from)) {
			if (open->path() == view_.documents.active) active_now = move.to;
			documents().close_document(open->path());
			reopen.push_back(move.to);
		}
		note("Renamed " + move.from + " to " + basename_of(move.to) + ", the expansion's new name.");
	}
	update_files(touched);
	for (const std::string &path : reopen) documents().open_document(request::open_document(path));
	if (!active_now.empty()) documents().open_document(request::open_document(active_now));
	return true;
}

const RequirementRow *SessionCore::requirement_row(const std::string &role) const {
	const RequirementRow *row = nullptr;
	for (const RequirementRow &candidate : view_.project.requirements->rows)
		if (candidate.role == role) row = &candidate;
	return row;
}

const AssetEntry *SessionCore::project_file(const std::string &file, bool *ambiguous) const {
	if (ambiguous) *ambiguous = false;
	return view_.project.scan ? view_.project.scan->named(file, ambiguous) : nullptr;
}

// --- the build -------------------------------------------------------------------------------

// The build as an operation (BuildOperation): planned here from the Problems rows as they are,
// then stepped by the polls and landed by the one that sees it done (absorb_build). Unsaved
// edits never reach here: Build and Play wait on the unsaved prompt first (UnsavedGuard), whose
// Save writes them. A build running already served the request at the busy gate (it joined).
std::string SessionCore::export_folder(const std::string &to) {
	const std::string own = paths_.export_dir(*view_.project.document);
	if (to.empty()) return own;
	fs::path out = path_of(to);
	if (out.is_relative()) out = path_of(paths_.root) / out;
	out = out.lexically_normal();
	if (inside(out, path_of(paths_.root)) && !inside(out, path_of(own))) {
		view_.activity.status = "The export was refused: its folder is inside the project.";
		report(make_finding(CoreFinding::ExportFolder, DiagnosticSeverity::Error,
		                    "An export cannot land in " + utf8_of(out) +
		                            ": it is inside the project, whose files the next build would pack. Choose a folder "
		                            "outside it, or the project's export folder."));
		return std::string();
	}
	return without_trailing_separator(utf8_of(out));
}

BuildTarget SessionCore::build_target() const {
	BuildTarget target;
	if (!view_.project.open) return target;
	target.expansion = view_.project.document->expansion.name;
	target.install = game_install();
	target.game = view_.project.document->target_game;
	return target;
}

ShippedFiles SessionCore::shipped_files() { return shipped_files(problems().gate_findings()); }

ShippedFiles SessionCore::shipped_files(const std::vector<Diagnostic> &gate) {
	ShippedFiles shipped;
	for (const auto &open : view_.documents.open)
		if (open && open->dirty()) shipped.unsaved.insert(open->path());
	if (!view_.project.open) return shipped;
	// Only the files a gate row says do not serialize are asked of the install (a few, each read once while
	// it stands).
	std::vector<std::string> asked;
	for (const Diagnostic &d : gate)
		if (d.severity == DiagnosticSeverity::Error && d.row() && d.row()->blocks_save && !d.asset.empty() &&
		    !shipped.unsaved.count(d.asset))
			asked.push_back(d.asset);
	std::sort(asked.begin(), asked.end());
	asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
	shipped.original = original_bytes_.identical(game_install(), *view_.project.document, paths_.root, asked);
	return shipped;
}

void SessionCore::start_build(const PlayIntent &intent, const std::string &out_dir, bool rehash,
                              const ExportIntent &exported) {
	if (intent.wanted && play().refused(intent.mission)) return;
	if (exported.wanted && export_folder(exported.to).empty()) return;
	// Where it lands: out_dir taken from the project's folder when relative. One inside the project
	// but in its cache or its export folder (which the scan passes over) would be files of the
	// project the next scan lists, an archive every later build refuses: refused before anything
	// is read.
	std::string output_root = paths_.build_dir + "/play";
	if (!out_dir.empty()) {
		fs::path out = path_of(out_dir);
		if (out.is_relative()) out = path_of(paths_.root) / out;
		out = out.lexically_normal();
		if (inside(out, path_of(paths_.root)) && !inside(out, path_of(paths_.cache_dir)) &&
		    !inside(out, path_of(paths_.export_dir(*view_.project.document)))) {
			view_.activity.status = "The build was refused: its folder is inside the project.";
			report(make_finding(CoreFinding::BuildOutDirInProject, DiagnosticSeverity::Error,
			                    "A build cannot land in " + utf8_of(out) +
			                            ": it is inside the project, whose files the next build would pack. "
			                            "Choose a folder outside it, or its export folder."));
			return;
		}
		output_root = utf8_of(out);
	}
	problems().clear_build_findings(); // the last build's rows go: this one reports anew
	documents().reload_changed();
	refresh_now();
	// The plan gates on the findings the refresh above just produced (the Problems rows),
	// not on a validation of its own; the build's own findings are those its report adds to
	// these rows (absorb_build), whatever the rows are when it ends.
	const BaseNames base{&view_.project.base_files};
	std::vector<Diagnostic> gate = problems().gate_findings();
	const ShippedFiles shipped = shipped_files();
	BuildPlan plan = plan_build(paths_, *view_.project.scan, *view_.project.requirements, gate, build_target(), &base,
	                            &shipped);
	plan.rehash = rehash;
	// The build is this project's: in a folder several projects build into (Build to folder), it reuses, prunes
	// and replaces only its own. Inside the project (its default folder, its cache, its export folder) every
	// build there is its own.
	plan.project = view_.project.document->project_id;
	plan.own_folder = inside(path_of(output_root), path_of(paths_.root));
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
	        plan, output_root, std::move(protected_dirs), view_.findings.diagnostics, intent, exported,
	        [this](const ExportIntent &wanted, const BuildReport &built, ExportRequest &request, ExportReport &refused) {
		        return export_request(wanted, built, request, refused);
	        }));
	if (id == 0) return refuse_busy(std::string()); // another operation runs, holding nothing it needs
	outcome_.operation = id;
	view_.activity.status = intent.wanted     ? "Building, then playing..."
	                        : exported.wanted ? "Building, then exporting..."
	                                          : "Building...";
	note("Build started.");
	show_operation();
}

bool SessionCore::export_request(const ExportIntent &intent, const BuildReport &built, ExportRequest &request,
                                 ExportReport &refused) {
	// What ships (ADR 0046 S16): the build copied whole into the export folder, the runtime's folder
	// beside a standalone game when the project asks for it and a packaged runtime is set.
	const ProjectDocument &document = *view_.project.document;
	request.build_dir = built.build_dir;
	request.build_id = built.build_id;
	request.expansion = built.expansion;
	request.export_dir = export_folder(intent.to);
	request.project_id = document.project_id;
	// The runtime ships beside a standalone game alone (an expansion plays in the install's game), and
	// only a packaged one: a source run's is the checkout.
	const bool runtime = document.export_settings.include_runtime && built.expansion.empty();
	const bool packaged = !view_.activity.source_run && !view_.activity.runtime_executable.empty();
	if (runtime && packaged) request.runtime_dir = utf8_of(path_of(view_.activity.runtime_executable).parent_path());
	refused.export_dir = request.export_dir;
	if (request.export_dir.empty()) {
		refused.diagnostics.push_back(make_finding(CoreFinding::ExportFolder, DiagnosticSeverity::Error,
		                                           "The export's folder is inside the project: nothing was copied."));
		return false;
	}
	if (runtime && !packaged) {
		refused.diagnostics.push_back(make_finding(
		        CoreFinding::ExportRuntime, DiagnosticSeverity::Error,
		        "The project ships the runtime beside the game, and Play runs no packaged runtime here: set one in "
		        "File > Project settings..., or export without it."));
		return false;
	}
	return true;
}

OperationOutcome SessionCore::absorb_build(const BuildReport &result, const std::vector<Diagnostic> &gate,
                                           const PlayIntent &intent, const ExportIntent &exported,
                                           const ExportReport *shipped) {
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
	// The build panel comes forward with what it came to (the UX round's problems lane).
	// `flag` it landed; `tag` 1 when a Play waits on it (the game is what follows, not the panel).
	ViewEvent ended;
	ended.kind = ViewEventKind::BuildEnded;
	ended.flag = result.ok;
	ended.tag = intent.wanted ? 1 : 0;
	view_.events.post(std::move(ended));
	touch(ViewConcern::Dialogs);
	if (result.ok) {
		BuildResult said = build_result(result, true);
		said.where = shown_path(result.build_dir, paths_.root);
		note(build_result_line(said));
		view_.activity.status = said.headline;
	} else if (result.refused) {
		// What refused it, by name (the UX round's problems lane): its first refusal on the status line,
		// the whole line (the build.blocked row's) in Output and Problems.
		std::vector<Diagnostic> blockers;
		for (const Diagnostic &d : result.diagnostics)
			if (blocks_build(d) && d.row() != &finding_code(CoreFinding::BuildBlocked)) blockers.push_back(d);
		note("Build refused.");
		view_.activity.status = blockers.empty()
		                                ? std::string("Build refused: see Problems.")
		                                : "Build refused: " + blocker_words(blockers.front()) +
		                                          (blockers.size() > 1 ? " (and " + std::to_string(blockers.size() - 1) + " more)" : "") +
		                                          ". See Problems.";
	} else {
		note("Build failed.");
		view_.activity.status = "Build failed; see Problems.";
	}
	if (result.ok && intent.wanted) play().start(intent.mission);
	bool exported_ok = true;
	if (result.ok && exported.wanted && shipped) {
		for (const Diagnostic &d : shipped->diagnostics) {
			report(d);
			own.push_back(d);
		}
		exported_ok = shipped->ok;
		if (shipped->ok) {
			note("Exported " + counted(shipped->files.size(), "file") + " to " + shown_path(shipped->export_dir, paths_.root));
			view_.activity.status = "Exported.";
		} else {
			note("Export failed.");
			view_.activity.status = "Export failed; see Problems.";
		}
		problems().set_build_findings(own);
		view_.activity.has_export = true;
		view_.activity.last_export = std::make_shared<const ExportReport>(*shipped);
	}
	touch(ViewConcern::Operation);
	OperationOutcome outcome;
	outcome.end = result.ok && exported_ok ? OperationEnd::Done : OperationEnd::Failed;
	outcome.findings = std::move(own);
	return outcome;
}

} // namespace opennova::editor
