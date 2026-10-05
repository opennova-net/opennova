#include <editor/session/request_kinds.h>

#include <cstddef>
#include <iterator>
#include <optional>
#include <vector>

#include <editor/project/project_files.h>
#include <editor/session/build_operation.h>
#include <editor/session/document_set.h>
#include <editor/session/import_controller.h>
#include <editor/session/play_controller.h>
#include <editor/session/problem_confirmation.h>
#include <editor/session/problems_service.h>
#include <editor/session/rename_controller.h>
#include <editor/session/session_core.h>
#include <editor/session/sound_play.h>
#include <editor/session/texture_show_use.h>
#include <editor/session/unsaved_guard.h>
#include <editor/session/workspace_parts.h>

namespace opennova::editor {

namespace {

using K = EditorRequestKind;
using F = RequestFieldId;

constexpr Holds kNone = HoldsNothing;
constexpr Holds kFiles = HoldsFiles;
constexpr Holds kDocuments = HoldsDocuments;
constexpr Holds kFilesAndDocuments = HoldsFiles | HoldsDocuments;
constexpr Holds kSlot = HoldsSlot;

// --- the handlers: what serves each session row, through the part it names -----------------------

void serve_new_project(SessionCore &core, const EditorRequest &request) {
	core.new_project(request.dir, request.title, request.game, request.import_pass,
	                 ProjectExpansion{ request.expansion, request.builds_on }, request.game_install);
}
void serve_check_install(SessionCore &core, const EditorRequest &request) {
	core.check_install(request.game_install);
}
void serve_open_project(SessionCore &core, const EditorRequest &request) {
	core.open_project(request.dir, request.import_pass, request.game_install);
}
void serve_close_project(SessionCore &core, const EditorRequest &) {
	core.close_project();
}
void serve_forget_recent(SessionCore &core, const EditorRequest &request) {
	core.forget_recent(request.dir);
}
// Only what changed outside the editor is read again: an open document whose file holds what it
// was read from keeps its records, its history and its selection; then the project's files, as an
// operation (RefreshOperation).
void serve_rescan(SessionCore &core, const EditorRequest &) {
	if (!core.view().project.open)
		return;
	core.documents().reload_changed();
	core.start_refresh();
}
void serve_apply_project_settings(SessionCore &core, const EditorRequest &request) {
	core.apply_project_settings(request.settings);
}
void serve_preview_import(SessionCore &core, const EditorRequest &request) {
	core.imports().preview_files(request);
}
void serve_plan_import(SessionCore &core, const EditorRequest &request) {
	core.imports().plan(request);
}
void serve_set_import_dependencies(SessionCore &core, const EditorRequest &request) {
	core.imports().set_dependencies(request.with_dependencies);
}
void serve_import_files(SessionCore &core, const EditorRequest &request) {
	core.imports().import_files(request);
}
void serve_cancel_import(SessionCore &core, const EditorRequest &) {
	core.imports().cancel();
}
void serve_create_missing(SessionCore &core, const EditorRequest &request) {
	if (core.view().project.open)
		core.create_missing(request.roles);
}
void serve_build(SessionCore &core, const EditorRequest &request) {
	if (core.view().project.open)
		core.start_build(PlayIntent(), request.out_dir, request.rehash, ExportIntent(), request.report);
}
void serve_play(SessionCore &core, const EditorRequest &request) {
	if (core.view().project.open)
		core.start_build(PlayIntent{ true, request.mission, request.behind }, std::string(), false, ExportIntent());
}
void serve_export(SessionCore &core, const EditorRequest &request) {
	if (core.view().project.open)
		core.start_build(PlayIntent(), std::string(), request.rehash, ExportIntent{ true, request.export_dir });
}
void serve_stop_play(SessionCore &core, const EditorRequest &) {
	core.play().stop();
}
void serve_cancel_operation(SessionCore &core, const EditorRequest &) {
	core.cancel_operation(true);
}
void serve_create_file(SessionCore &core, const EditorRequest &request) {
	core.documents().create_file(request);
}
// OpenDocument and ReloadDocument.
void serve_open_document(SessionCore &core, const EditorRequest &request) {
	core.documents().open_document(request);
}
void serve_show_in_files(SessionCore &core, const EditorRequest &request) {
	core.documents().show_in_files(request);
}
void serve_select_file(SessionCore &core, const EditorRequest &request) {
	core.documents().select_file(request.path);
}
void serve_close_document(SessionCore &core, const EditorRequest &request) {
	core.documents().close_document(request.path);
}
void serve_select_record(SessionCore &core, const EditorRequest &request) {
	core.documents().select_record(request);
}
void serve_edit_record(SessionCore &core, const EditorRequest &request) {
	core.documents().edit_record(request);
}
void serve_set_string_text(SessionCore &core, const EditorRequest &request) {
	core.documents().set_string_text(request);
}

void serve_revert_to_saved(SessionCore &core, const EditorRequest &request) {
	core.documents().revert_to_saved(request);
}
void serve_end_edit(SessionCore &core, const EditorRequest &request) {
	core.documents().end_edit(request.path);
}
// Copy and Cut.
void serve_copy(SessionCore &core, const EditorRequest &request) {
	core.documents().copy(request);
}
void serve_paste(SessionCore &core, const EditorRequest &request) {
	core.documents().paste(request);
}
void serve_duplicate(SessionCore &core, const EditorRequest &request) {
	core.documents().duplicate(request);
}
void serve_save(SessionCore &core, const EditorRequest &request) {
	core.documents().save(request.path);
}
void serve_save_all(SessionCore &core, const EditorRequest &) {
	core.documents().save_all();
}
// Undo and Redo.
void serve_undo_redo(SessionCore &core, const EditorRequest &request) {
	core.documents().undo_redo(request);
}
// The prompt's answer runs what waited on it through its own row again (serve_request), never
// through handle(): a request from outside is one entry and one outcome.
void serve_resolve_unsaved(SessionCore &core, const EditorRequest &request) {
	if (const std::optional<EditorRequest> waited = core.guard().resolve(request.choice))
		serve_request(core, *waited);
}
void serve_rename_asset(SessionCore &core, const EditorRequest &request) {
	core.renames().rename_asset(request.path, request.new_name);
}
void serve_assign_requirement(SessionCore &core, const EditorRequest &request) {
	core.renames().assign_requirement(request.role, request.path);
}
void serve_preview_rename(SessionCore &core, const EditorRequest &request) {
	core.renames().preview(request);
}
void serve_rename_symbol(SessionCore &core, const EditorRequest &request) {
	core.renames().rename_symbol(request);
}
void serve_preview_rename_back(SessionCore &core, const EditorRequest &request) {
	core.renames().preview_back(request);
}
void serve_rename_back(SessionCore &core, const EditorRequest &) {
	core.renames().rename_back();
}
void serve_reimport(SessionCore &core, const EditorRequest &request) {
	core.imports().reimport(request.path, request.force);
}
void serve_set_import_options(SessionCore &core, const EditorRequest &request) {
	core.imports().set_options(request.path, request.values);
}
void serve_texture_operation(SessionCore &core, const EditorRequest &request) {
	core.documents().texture_operation(request);
}
void serve_replace_texture(SessionCore &core, const EditorRequest &request) {
	core.imports().replace_texture(request);
}
void serve_split_texture(SessionCore &core, const EditorRequest &request) {
	core.renames().split_texture(request);
}
void serve_edit_externally(SessionCore &core, const EditorRequest &request) {
	core.imports().edit_externally(request);
}
void serve_refresh_changed_sources(SessionCore &core, const EditorRequest &) {
	core.imports().refresh_changed_sources();
}
void serve_show_use(SessionCore &core, const EditorRequest &request) {
	show_texture_use(core, request);
}
void serve_preview_texture_source(SessionCore &core, const EditorRequest &request) {
	core.imports().preview_texture_source(request);
}
void serve_cancel_texture_source(SessionCore &core, const EditorRequest &) {
	core.imports().close_texture_source();
}
void serve_open_texture_source(SessionCore &core, const EditorRequest &request) {
	core.imports().open_texture_source(request);
}
void serve_preview_install_import(SessionCore &core, const EditorRequest &request) {
	core.imports().preview_install(request);
}
void serve_clear_output(SessionCore &core, const EditorRequest &) {
	core.clear_output();
}
void serve_set_viewport(SessionCore &core, const EditorRequest &request) {
	core.set_viewport(request.path, request.viewport);
}
void serve_edit_in_viewport(SessionCore &core, const EditorRequest &request) {
	core.edit_in_viewport(request);
}
void serve_set_workspace(SessionCore &core, const EditorRequest &request) {
	set_workspace(core, request.workspace);
}
void serve_play_sound(SessionCore &core, const EditorRequest &request) {
	serve_sound_play(core, request);
}
void serve_stop_sound(SessionCore &core, const EditorRequest &) {
	stop_sound(core);
}
void serve_apply_confirmation(SessionCore &core, const EditorRequest &) {
	apply_confirmation(core);
}
void serve_quit(SessionCore &core, const EditorRequest &) {
	core.quit();
}

// --- the table ----------------------------------------------------------------------------------

// A row built up column by column (as the asset kinds' rows are), so each row names only what it
// sets: a session row's handler and doc always, a shell row's server.
struct Request {
	RequestKindRow row;
	constexpr Request(K kind, const char *token, RequestHandler handler, const char *doc) : row() {
		row.kind = kind;
		row.token = token;
		row.handler = handler;
		row.doc = doc;
	}
	// A shell row: the shell serves it, the session never does (no handler).
	constexpr Request served_by(ServedBy by) const {
		Request out = *this;
		out.row.served_by = by;
		return out;
	}
	constexpr Request takes(RequestParams params) const {
		Request out = *this;
		out.row.params = params;
		return out;
	}
	constexpr Request holds(Holds reads, Holds writes, OnBusy on_busy = OnBusy::Refuse) const {
		Request out = *this;
		out.row.reads = reads;
		out.row.writes = writes;
		out.row.on_busy = on_busy;
		return out;
	}
	// What the unsaved-changes prompt guards of it, and its words: what waits ("Close %s", the %s
	// the file the request names), the Save button.
	constexpr Request guarded(GuardScope guard, const char *waiting, const char *save_label) const {
		Request out = *this;
		out.row.guard = guard;
		out.row.waiting = waiting;
		out.row.save_label = save_label;
		return out;
	}
	constexpr Request can_discard() const {
		Request out = *this;
		out.row.can_discard = true;
		return out;
	}
	constexpr Request acts_on_saved() const {
		Request out = *this;
		out.row.acts_on_saved = true;
		return out;
	}
	constexpr Request names_active() const {
		Request out = *this;
		out.row.names_active = true;
		return out;
	}
	constexpr Request ends_edit_groups() const {
		Request out = *this;
		out.row.ends_edit_groups = true;
		return out;
	}
	constexpr Request background() const {
		Request out = *this;
		out.row.background = true;
		return out;
	}
};

// What each request reads and writes: the files when it reads or writes them on disk (the import
// pass writes them too), the open documents when it reads them or changes what they hold or which
// are open, everything when it switches or closes the project, and the slot when it starts an
// operation (S13 A3: opening a project, a refresh, an import's plan and its write, a rename's
// commit, a build). A running build reads the files and writes only the slot, so an edit, an open
// or a selection goes on beside it, and a save, a create, an import (its preview too, whose plan is
// an operation of its own) or a rename waits.
constexpr RequestKindRow kRows[] = {
	Request(K::NewProject, "new_project", serve_new_project,
			"A project made in dir (its title, else the folder's name; its game, else jo; S16: built as "
			"the expansion `expansion` on the installed one `builds_on`, its version text made and, on "
			"the base game, its text table), then opened as open_project opens it (import_pass false: "
			"no source the folder holds imported); refused, the open project kept, where dir holds a "
			"project already, game names no game, or the expansion is one the game cannot take or the "
			"install refuses (a name it has, one it lacks to build on). game_install (the UX round's "
			"project lane): the game install it imports from and plays in, checked first (check_install: "
			"refused, project.install.invalid, where the folder holds none of the game's archives) and "
			"then the editor's install, which the project's local.json takes as it opens.")
			.takes(request_params({ F::Dir }, { F::Title, F::Game, F::Expansion, F::BuildsOn, F::ImportPass, F::GameInstall }))
			.holds(kNone, kHoldsAll | kSlot, OnBusy::CancelRunning)
			.ends_edit_groups()
			.guarded(GuardScope::AllDirty, "Create a new project", "Save all")
			.can_discard()
			.acts_on_saved()
			.row,
	Request(K::OpenProject, "open_project", serve_open_project,
			"The project in dir opened, an operation (the outcome names it: the game install's "
			"names, its import pass (import_pass false: on its files as they are, no source "
			"imported), the scan, the requirements; the project is the open one once it ends), on "
			"game_install for the session alone when given (its .opennova/local.json kept); one "
			"that does not open leaves the open project open.")
			.takes(request_params({ F::Dir }, { F::GameInstall, F::ImportPass }))
			.holds(kNone, kHoldsAll | kSlot, OnBusy::CancelRunning)
			.ends_edit_groups()
			.guarded(GuardScope::AllDirty, "Open another project", "Save all")
			.can_discard()
			.acts_on_saved()
			.row,
	Request(K::CloseProject, "close_project", serve_close_project, "The open project closed.")
			.holds(kNone, kHoldsAll, OnBusy::CancelRunning)
			.guarded(GuardScope::AllDirty, "Close the project", "Save all")
			.can_discard()
			.acts_on_saved()
			.row,
	Request(K::ForgetRecent, "forget_recent", serve_forget_recent,
			"The recent project in dir dropped from the editor's list.")
			.takes(request_params({ F::Dir }))
			.row,
	// A read of a folder, never of the project: it runs beside any operation.
	Request(K::CheckInstall, "check_install", serve_check_install,
			"The folder game_install (left out: the editor's last chosen) read as an install of the open "
			"project's game (else jo), as an import mounts one: whether it is there, whether its archives "
			"mount, how many files it serves, its expansions and whether the game's program is beside them; "
			"the project section's install_check, with its words in a line.")
			.takes(request_params({}, { F::GameInstall }))
			.row,
	// It reads again the clean documents whose files changed, then starts the refresh.
	Request(K::Rescan, "rescan", serve_rescan,
			"The project's files read again: an open document whose file changed outside the "
			"editor "
			"is read again (one with unsaved edits keeps them, document.conflict; one that no "
			"longer "
			"reads stays open, document.stale), then the import pass, the scan and the "
			"requirements, an operation (the outcome names it).")
			.holds(kFiles, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.acts_on_saved()
			.row,
	// The settings dialog waits on its answer, so a settings change is never refused whole: each
	// of its parts is weighed against the running operation inside, a refused part a failure the
	// view's settings_result lists and its settings_applied event flags
	// (SessionCore::apply_project_settings).
	Request(K::ApplyProjectSettings, "apply_project_settings", serve_apply_project_settings,
			"The settings set, each one left out as it is: the project's name and features "
			"(project.opennova), the game install (the project's .opennova/local.json, and the "
			"editor's, where a project naming none starts), the runtime and Play in the game "
			"install (the editor's); its settings_applied view event carries the serial back, "
			"flagged when a setting could not be written, and the view's settings_result lists "
			"what could not be.")
			.takes(request_params({ F::Settings }))
			.row,
	// An import's plan is an operation (S13 A3, ImportPlan): these rows (and PreviewInstallImport's)
	// start one, so they write the slot, as Build and Play do, and none of them runs beside another
	// operation; a new plan takes the place of a running one (Supersede).
	Request(K::PreviewImport, "preview_import", serve_preview_import,
			"The import dialog on the files on disk paths (a loose file chosen, an archive's members "
			"listed to choose from), planned with the files they need, found beside them or in the "
			"game install, when with_dependencies: the plan an operation (the outcome names it), "
			"which a new plan takes the place of.")
			.takes(request_params({ F::Paths }, { F::WithDependencies }))
			.holds(kFiles, kSlot, OnBusy::Supersede)
			.ends_edit_groups()
			.row,
	Request(K::PlanImport, "plan_import", serve_plan_import,
			"The import dialog planned again over the files chosen, imports, with the files they "
			"need "
			"when with_dependencies (a preview opens when none is), an operation.")
			.takes(request_params({ F::Imports }, { F::WithDependencies }))
			.holds(kFiles, kSlot, OnBusy::Supersede)
			.ends_edit_groups()
			.row,
	// A preference alone: it holds nothing of the session's. With the import dialog open it plans the
	// dialog again as a plan_import does, through the gate (a running plan gives way to it).
	Request(K::SetImportDependencies, "set_import_dependencies", serve_set_import_dependencies,
			"The editor's setting of whether an import brings the files the chosen ones need "
			"(with_dependencies), remembered; an open import dialog is planned again with it, as "
			"plan_import plans it (whatever the setting was).")
			.takes(request_params({ F::WithDependencies }))
			.row,
	Request(K::ImportFiles, "import_files", serve_import_files,
			"The import dialog's rows kept, imports (or, with planned and the plan it names, the open "
			"preview's checked rows as the dialog's Import takes them: an unchecked row never, a checked "
			"row the project holds replacing it), copied into the project, every file checked and "
			"staged before any is published (replace: over the project's files of the names), then "
			"the project's files read again, an operation (the outcome names it; it can be cancelled "
			"until it writes); with a preview open the files are planned again first, and nothing is "
			"written when that is not the plan shown (import.changed: the checks carried over to the "
			"new plan, which a planned import names again). A planned import of a plan made since is "
			"refused (import.not_planned).")
			.takes(request_params({}, { F::Imports, F::Replace, F::Planned, F::Plan }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.guarded(GuardScope::PlannedWrites, "Import", "Save all and import")
			.acts_on_saved()
			.row,
	// It stops the import's plan or write that runs (Supersede: ImportPlan and ImportApply give way to
	// it; an import that wrote cannot be cancelled, and refuses it until it ends).
	Request(K::CancelImport, "cancel_import", serve_cancel_import,
			"The import dialog closed, its plan stopped when one runs, and an import it raised stopped "
			"before it writes.")
			.holds(kNone, kNone, OnBusy::Supersede)
			.row,
	Request(K::CreateMissing, "create_missing", serve_create_missing,
			"The required files roles names made from scratch (none: nothing); a file there since "
			"the last refresh is refused, never overwritten.")
			.takes(request_params({}, { F::Roles }))
			.holds(kFiles, kFiles)
			.row,
	// Before its operation starts, a Build (a Play's too) reads again the documents whose files
	// changed and refreshes the project (SessionCore::start_build: the import pass, the scan): it
	// writes the files and the documents, and the slot. A running build still serves it (its row's
	// joined_by).
	Request(K::Build, "build", serve_build,
			"The project packed into a build under out_dir (taken from the project's folder when "
			"relative; left out, the project's .opennova/build/play; refused inside the project "
			"but in its cache or its export folder, build.out_dir_in_project), an operation (the "
			"outcome names it); a build running already serves it, where it packs. rehash: every "
			"file read again, the build cache set aside. report false: its result panel does not open as it "
			"ends (a build asked over the wire, the person's work left as it is). Unsaved edits wait on "
			"the prompt first.")
			.takes(request_params({}, { F::OutDir, F::Rehash, F::Report }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, OnBusy::Join)
			.guarded(GuardScope::AllDirty, "Build", "Save all and build")
			.acts_on_saved()
			.ends_edit_groups()
			.row,
	Request(K::Play, "play", serve_play,
			"A build, then the game run on it once it lands, at its menu, or in mission (a .bms of "
			"the project by its logical name; one the project does not hold is refused before "
			"anything is built, play.mission.unknown; Play in the game install starts at its menu "
			"all the same); a build running already serves it and starts the game when it lands, in "
			"the mission the last Play named. A mission that does not load is a Problems row "
			"(play.mission.failed) until the next Play. behind: the game's window starts behind every other "
			"and never takes the foreground (the run section says behind).")
			.takes(request_params({}, { F::Mission, F::Behind }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, OnBusy::Join)
			.guarded(GuardScope::AllDirty, "Play", "Save all and play")
			.acts_on_saved()
			.ends_edit_groups()
			.row,
	// ADR 0046 S16: what ships, through the build's gate.
	Request(K::Export, "export", serve_export,
			"A build, then the build copied into export_dir (left out, the project's export folder; "
			"taken from the project's folder when relative; refused inside the project but its export "
			"folder) as what ships: a standalone game's archives and loose files (the runtime's folder "
			"under runtime/ when project.opennova's export.include_runtime asks), an expansion's "
			"expansion/<name>/ laid out as in an install, with export.json naming the project. The "
			"folder is replaced only when missing, empty or an export of this project (export.folder). "
			"An operation (the outcome names it); a build running already serves it and exports once it "
			"lands. rehash: every file read again.")
			.takes(request_params({}, { F::ExportDir, F::Rehash }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | HoldsSlot, OnBusy::Join)
			.guarded(GuardScope::AllDirty, "Export", "Save all and export")
			.acts_on_saved()
			.ends_edit_groups()
			.row,
	Request(K::StopPlay, "stop_play", serve_stop_play, "The running game stopped.").row,
	Request(K::CancelOperation, "cancel_operation", serve_cancel_operation,
			"The running operation stopped between two steps, its work discarded (a build's "
			"staging "
			"removed, a Play waiting on it dropped).")
			.row,
	Request(K::CreateFile, "create_file", serve_create_file,
			"A blank file path made from its name's requirement factory, else its kind's free-form "
			"one (file_kind where the name cannot say the kind), with the values its blank takes "
			"(a mission's terrain and environment, files of the project, and its title: one it "
			"does not take, a required one left out or a file the project lacks is refused, "
			"document.values, nothing made); a new mission comes with its text table (<mission>.bin: "
			"its title, an empty briefing) where the project has none of that name; opened when the "
			"editor edits its kind.")
			.takes(request_params({ F::Path }, { F::FileKind, F::Values }))
			.holds(kFiles, kFilesAndDocuments)
			.row,
	Request(K::OpenDocument, "open_document", serve_open_document,
			"The document at path opened, or made active, with the record at locator (a Go to) or "
			"at address selected and its field shown (a RevealRecord view event).")
			.takes(request_params({}, { F::Path, F::Locator, F::Field, F::Address }))
			.holds(kFiles, kDocuments)
			.names_active()
			.row,
	Request(K::ShowInFiles, "show_in_files", serve_show_in_files,
			"Files selects the project file path and scrolls to it (a RevealFile view event), a "
			"file the editor does not open included; ask_name: and asks its new name "
			"(Rename...).")
			.takes(request_params({ F::Path }, { F::AskName }))
			.row,
	Request(K::AboutFile, "about_file", serve_show_in_files,
			"Files selects the project file path and comes forward (a RevealFile view event), and its card opens "
			"(the workspace section's card; set_workspace closes it): what it is, where a build puts it, what it "
			"names and who names it, a wave's sound; the file_card query reads the same.")
			.takes(request_params({ F::Path }))
			.row,
	Request(K::SelectFile, "select_file", serve_select_file,
			"The project file path selected in Files (left out, none): a file a viewport draws whether "
			"or not it is open (a texture) shows in the Preview window until another document is made "
			"active.")
			.takes(request_params({}, { F::Path }))
			.row,
	Request(K::ReloadDocument, "reload_document", serve_open_document,
			"The document at path read again from its file, its unsaved edits dropped (the prompt "
			"asks first).")
			.takes(request_params({}, { F::Path }))
			.holds(kFiles, kDocuments)
			.guarded(GuardScope::Document, "Reload %s", "Save")
			.can_discard()
			.acts_on_saved()
			.names_active()
			.row,
	Request(K::CloseDocument, "close_document", serve_close_document,
			"The document at path closed (the prompt asks about its unsaved edits first).")
			.takes(request_params({}, { F::Path }))
			.holds(kNone, kDocuments)
			.guarded(GuardScope::Document, "Close %s", "Save")
			.can_discard()
			.acts_on_saved()
			.names_active()
			.row,
	Request(K::SelectRecord, "select_record", serve_select_record,
			"The record at address, the primary, and the records named with it selected in the "
			"document at path, over any of its rows, joining the selection as mode says.")
			.takes(request_params({ F::Address }, { F::Path, F::Records, F::Mode }))
			.names_active()
			.row,
	// A fix's edit opens its document first.
	Request(K::EditRecord, "edit_record", serve_edit_record,
			"The edits, a batch over any rows (records, rows and file-wide values), applied to the "
			"document at path as one undo step (batches sharing a nonzero gesture fold into one "
			"until end_edit); open_first: the document opened first when it is not (a fix's "
			"edit).")
			.takes(request_params({ F::Edits }, { F::Path, F::OpenFirst }))
			.holds(kFiles, kDocuments)
			.names_active()
			.row,
	Request(K::RevertToSaved, "revert_to_saved", serve_revert_to_saved,
			"Each field the edits name ({id, field}: a record by its identity, a field by its id) "
			"of the document at path given back the value and presence the saved file holds, one "
			"undo step; refused when none has anything to go back to (the Inspector's Revert to "
			"saved).")
			.takes(request_params({ F::Edits }, { F::Path }))
			.holds(kNone, kDocuments)
			.names_active()
			.row,
	// The plain-words lane (the audit's 3.2): a label edited as the words the player sees.
	Request(K::SetStringText, "set_string_text", serve_set_string_text,
			"The string a field's string id names (the record at address of the document at path, its "
			"field: a menu window's Text, a weapon's loadout name) given the text values.text: the string "
			"table that defines the id, as the game's lookup reaches it, opened where it is not (the active "
			"document kept), its string's text set there, one undo step in that table; refused where the "
			"field names no string id or the project defines none of that id.")
			.takes(request_params({ F::Address, F::Field, F::Values }, { F::Path }))
			.holds(kFiles, kDocuments)
			.names_active()
			.row,
	Request(K::EndEdit, "end_edit", serve_end_edit,
			"The coalesced edit group, or the gesture, of the document at path (left out, the active "
			"document) ends; a gesture open in another document stays open.")
			.takes(request_params({}, { F::Path }))
			.names_active()
			.row,
	// It reads the documents as they stand, which an operation writes only as it finishes (in one
	// poll): it waits for none.
	Request(K::Copy, "copy", serve_copy,
			"The selected records of the document at path onto the session's clipboard, as the "
			"document type's own payload.")
			.takes(request_params({}, { F::Path }))
			.holds(kNone, kNone)
			.names_active()
			.row,
	Request(K::Cut, "cut", serve_copy,
			"The selected records of the document at path copied, then removed in one undo step.")
			.takes(request_params({}, { F::Path }))
			.holds(kNone, kDocuments)
			.names_active()
			.row,
	Request(K::Paste, "paste", serve_paste,
			"The clipboard pasted into the document at path where paste_at says, else after the "
			"selection, one undo step.")
			.takes(request_params({}, { F::Path, F::PasteAt }))
			.holds(kNone, kDocuments)
			.names_active()
			.row,
	Request(K::Duplicate, "duplicate", serve_duplicate,
			"Each selected record of the document at path (those no other selected one holds) "
			"copied "
			"right after itself, one undo step; the copies become the selection.")
			.takes(request_params({}, { F::Path }))
			.holds(kNone, kDocuments)
			.names_active()
			.row,
	Request(K::Save, "save", serve_save,
			"The document at path written, with no unsaved edits too when its file holds other "
			"bytes "
			"than it would write (a canonical rewrite); a file that is not open is read, rewritten "
			"that way when it must be, and left closed.")
			.takes(request_params({}, { F::Path }))
			.holds(kFilesAndDocuments, kFilesAndDocuments)
			.acts_on_saved()
			.names_active()
			.row,
	Request(K::SaveAll, "save_all", serve_save_all,
			"Every document with unsaved edits written, past a failure.")
			.holds(kFilesAndDocuments, kFilesAndDocuments)
			.acts_on_saved()
			.row,
	Request(K::Undo, "undo", serve_undo_redo, "The last step of the document at path undone.")
			.takes(request_params({}, { F::Path }))
			.holds(kNone, kDocuments)
			.names_active()
			.row,
	Request(K::Redo, "redo", serve_undo_redo,
			"The last step undone in the document at path done again.")
			.takes(request_params({}, { F::Path }))
			.holds(kNone, kDocuments)
			.names_active()
			.row,
	// Its Save is weighed as a Save All and its Discard as a write of the documents, inside; the
	// request it answers meets the gate itself.
	Request(K::ResolveUnsaved, "resolve_unsaved", serve_resolve_unsaved,
			"The unsaved-changes prompt answered with choice: save writes the files it lists and "
			"discard drops their edits, then what waited runs; cancel drops what waited.")
			.takes(request_params({ F::Choice }))
			.acts_on_saved()
			.row,
	Request(K::RenameAsset, "rename_asset", serve_rename_asset,
			"The project file path renamed to new_name, every reference to it rewritten, or "
			"refused "
			"with the reasons; not undoable. Committed as an operation (the outcome names it: a "
			"file at a time, then the one step that writes).")
			.takes(request_params({ F::Path, F::NewName }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.guarded(GuardScope::PlannedWrites, "Rename %s", "Save all and rename")
			.acts_on_saved()
			.row,
	Request(K::AssignRequirement, "assign_requirement", serve_assign_requirement,
			"The requirement role met by renaming the project file path, of the kind it expects, "
			"to "
			"the name the engine demands (as rename_asset renames).")
			.takes(request_params({ F::Role, F::Path }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.guarded(GuardScope::PlannedWrites, "Rename %s", "Save all and rename")
			.acts_on_saved()
			.row,
	Request(K::PreviewRename, "preview_rename", serve_preview_rename,
			"What a rename would do, planned into the view's rename_preview, nothing written: the "
			"name the record at locator of path defines in field renamed everywhere to new_name, "
			"or, "
			"with no field, the file path renamed to new_name; ask_name: the Rename everywhere "
			"dialog opens (an AskRename view event).")
			.takes(request_params({ F::Path }, { F::Locator, F::Field, F::NewName, F::AskName }))
			.holds(kFiles, kNone)
			.row,
	Request(K::RenameSymbol, "rename_symbol", serve_rename_symbol,
			"The name the record at locator of path defines in field renamed everywhere to "
			"new_name, "
			"every use that reaches it rewritten on disk, or refused with the reasons; not "
			"undoable. Committed as an operation (the outcome names it).")
			.takes(request_params({ F::Path, F::Locator, F::Field, F::NewName }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.guarded(GuardScope::PlannedWrites, "Rename everywhere (defined in %s)",
					"Save all and rename")
			.acts_on_saved()
			.row,
	Request(K::PreviewRenameBack, "preview_rename_back", serve_preview_rename_back,
			"The last rename's way back planned into the view's rename_preview (back), nothing written: the "
			"name it gave found again by itself (its kind and name in the file and field defining it; a file's "
			"rename: the file at its new path), renamed back to the old name at only the sites the rename "
			"rewrote; refused where a file it wrote changed since, a use it wrote is gone, or the name is; "
			"ask_name: the Rename back dialog opens (an AskRename view event).")
			.takes(request_params({}, { F::AskName }))
			.holds(kFiles, kNone)
			.row,
	Request(K::RenameBack, "rename_back", serve_rename_back,
			"The last rename taken back, its true inverse (preview_rename_back's plan): only the sites it "
			"rewrote rewritten again on disk, or refused with the reasons; not undoable. Committed as an "
			"operation (the outcome names it), which is then the last rename.")
			.takes(request_params({}))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.guarded(GuardScope::PlannedWrites, "Rename back", "Save all and rename back")
			.acts_on_saved()
			.row,
	Request(K::Reimport, "reimport", serve_reimport,
			"A refresh whose import pass takes the source path (left out: every source) even when "
			"it "
			"is unchanged when force says so, an operation (the outcome names it); the findings on "
			"the source are what the operation came to (last_operation).")
			.takes(request_params({}, { F::Path, F::Force }))
			.holds(kFiles, kFiles | kSlot)
			.ends_edit_groups()
			.row,
	// An import's options are its record's (S18): the record written, then the refresh that imports it
	// again, which holds the slot as a reimport does.
	Request(K::SetImportOptions, "set_import_options", serve_set_import_options,
			"The import record of the source path names, or of the import a file of that path or "
			"logical name comes from, given values, each an option's key as the import_options query "
			"lists them and a value its row takes (\"\" its default, left out of the record); then "
			"imported again, a refresh (the outcome names the operation): an output whose format moves "
			"takes the name its extension gives and the old file goes. Refused, nothing written, for a "
			"file no import makes, or a key or a value no row takes (import.option).")
			.takes(request_params({ F::Path, F::Values }))
			.holds(kFiles, kFiles | kSlot)
			.ends_edit_groups()
			.row,
	// A texture's whole-image edit is an edit of its document (S18), as edit_record's.
	Request(K::TextureOperation, "texture_operation", serve_texture_operation,
			"The texture document at path (left out, the active one) edited as a whole image by "
			"operation, one undo step, its "
			"params in values: resize (size: pow2_down, pow2_up, <W>x<H> or fit:<W>x<H>), alpha "
			"(alpha: opaque, luminance, invert, threshold:<n> or key:#RRGGBB), format (the stored form "
			"within the name's extension: format tga or tga24, a DDS's dds and mips, a PCX's palette), "
			"reorder_rows (a TGA stored top first saved bottom first) or remap_palette (an 8-bit PCX's "
			"indices, each \"<from>\": \"<to>\"); the file made anew through the editor's writers, which "
			"Save writes. open_first: the document opened first when it is not (a fix's). Refused, "
			"nothing changed (texture.operation): a file an import makes (its import's options make it), "
			"an operation it does not take, one it cannot do (a form that holds no alpha).")
			.takes(request_params({ F::Operation }, { F::Path, F::Values, F::OpenFirst }))
			.holds(kFiles, kDocuments)
			.names_active()
			.row,
	// A texture made from an image is an import of it (S18): its record written, then the refresh that
	// imports it, which holds the slot as a reimport does; the texture's open document is closed.
	Request(K::ReplaceTexture, "replace_texture", serve_replace_texture,
			"The texture path (a project file, an import's output, or a name the project lacks, as a field "
			"names it) made from the image in paths (a PNG, a TGA or a PCX: a file on disk, or a project "
			"file): the image copied into art/ as an import source whose record makes the texture under its "
			"name, in the form it is stored in (its format and compression; an import's output keeps its "
			"import's options), values over those; the plain file it replaces set aside under "
			".opennova/replaced/, never deleted; then imported, a refresh (the outcome names the "
			"operation). Refused, nothing written (texture.replace): an image the importer does not read, a "
			"texture that is no texture, an option no row takes, a texture open with unsaved edits.")
			.takes(request_params({ F::Path, F::Paths }, { F::Values }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.row,
	Request(K::SplitTexture, "split_texture", serve_split_texture,
			"The texture path copied as new_name, the uses in the project files paths names moved to the "
			"copy (their fields rewritten, every other use left on the texture); an import's output split "
			"as its source copied beside it, the copy's record making new_name with the output's options. "
			"Refused as rename_asset is, and with texture.split for an import's source, no referrer named "
			"or none that uses it; not undoable. Committed as an operation (the outcome names it).")
			.takes(request_params({ F::Path, F::NewName, F::Paths }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.guarded(GuardScope::PlannedWrites, "Split %s", "Save all and split")
			.acts_on_saved()
			.row,
	// A source made once is an import of it (S18), as replace_texture's.
	Request(K::EditExternally, "edit_externally", serve_edit_externally,
			"The texture path's source opened in the program the system has for its kind: an import's "
			"output's own source, a PNG the game reads as it is itself, a plain texture's made once (a copy "
			"of a TGA or a PCX, a PNG of a DDS's first level, in art/ under a name of its own, its record "
			"reproducing the texture, the plain file set aside under .opennova/replaced/) and imported, a "
			"refresh. The open_externally view event names the file on disk, which the Shell opens. Refused "
			"(texture.external): a name the project lacks, a file that does not read, a texture open with "
			"unsaved edits.")
			.takes(request_params({ F::Path }))
			.holds(kFilesAndDocuments, kFilesAndDocuments | kSlot)
			.ends_edit_groups()
			.row,
	Request(K::RefreshChangedSources, "refresh_changed_sources", serve_refresh_changed_sources,
			"When a watched file's size or last write moved since the scan (a program saved it: an import "
			"source, a file an import read, a PNG the game reads as it is), a refresh of what moved alone (the "
			"outcome names the operation): the sources it touches imported again, the scan updated for them "
			"and those files, the open documents of them read again. A file written within the last two "
			"seconds waits for a later check, never read half-written. Nothing otherwise, no operation "
			"started. The Shell sends it when its window gains the focus and once a second while it has it, "
			"of its own: the status line a refused request left stays.")
			.holds(kFiles, kFiles | kSlot)
			.background()
			.row,
	Request(K::ShowUse, "show_use", serve_show_use,
			"The texture path's use in the project file paths names (at locator and field, when given: the "
			"use there, a native text's use, which has no locator, by its record; else its first) shown "
			"where the game draws it: a model's or a menu's opened at the "
			"use, the Preview window brought forward (a reveal_preview view event); a terrain's or an "
			"environment's, the view of a mission that names it (one open, else the first); any other "
			"referrer's, the texture opened with its viewport's as_used set to the use. Refused, nothing "
			"opened (texture.show_use): the references not read yet, a file that does not use it, a name "
			"the game opens itself, a terrain or an environment no mission names.")
			.takes(request_params({ F::Path, F::Paths }, { F::Locator, F::Field }))
			.holds(kFiles, kDocuments)
			.row,
	Request(K::PreviewTextureSource, "preview_texture_source", serve_preview_texture_source,
			"What a Replace of the texture path by the image in paths (a file on disk, or a project file), "
			"or with no image an Edit externally, would do, planned into the view's texture_source dialog, "
			"nothing written: the stored forms the texture's extension offers and the one written (values "
			"over the form reproduced, as replace_texture takes them), the changes in words, the texture "
			"before and after in words and as pictures (the file the import would make), or why it would "
			"be refused. The dialog asks before replace_texture or edit_externally does it.")
			.takes(request_params({ F::Path }, { F::Paths, F::Values }))
			.holds(kFiles, kNone)
			.row,
	Request(K::CancelTextureSource, "cancel_texture_source", serve_cancel_texture_source,
			"The texture_source dialog closed, nothing done.")
			.row,
	// Nothing written: it goes on beside a build, as a read does.
	Request(K::OpenTextureSource, "open_texture_source", serve_open_texture_source,
			"The texture path's existing source opened in the program the system has for its kind, "
			"nothing written: an import's output's own source, a PNG the game reads as it is itself (the "
			"open_externally view event names it). Refused (texture.external): a texture with no source "
			"yet (edit_externally makes one), a PNG open with unsaved edits (its program edits the file "
			"as saved), no texture.")
			.takes(request_params({ F::Path }))
			.holds(kFiles, kNone)
			.row,
	Request(K::PreviewInstallImport, "preview_install_import", serve_preview_install_import,
			"The import dialog on the game install's files: the names alone, chosen, or with none "
			"every file listed to choose from, with the files they need when with_dependencies "
			"(their "
			"sources carry install: true); planned as preview_import plans. With all, every file "
			"chosen at once (the archives' and the loose files the game ships beside them), with no "
			"walk: import_files with planned then takes them.")
			.takes(request_params({}, { F::Names, F::WithDependencies, F::All }))
			.holds(kFiles, kSlot, OnBusy::Supersede)
			.ends_edit_groups()
			.row,
	Request(K::ClearOutput, "clear_output", serve_clear_output,
			"The output lines emptied, as Output's Clear does.")
			.row,
	// A viewport's state is no file and no document: it runs beside any operation.
	Request(K::SetViewport, "set_viewport", serve_set_viewport,
			"The viewport over the document at path (left out, the active one; of the kind viewport "
			"names, else the Preview's kind that shows the document, else its Main view) changed as "
			"viewport says: the size its device draws at where no canvas sizes the picture, the "
			"preview clock, the kind's options and camera; the clock alone with no path sets the "
			"preview clock every viewport reads, whatever document is active. Refused, nothing "
			"changed, naming a member it does not take, a value out of range, a document not open, a "
			"kind that does not show it or a device a canvas sizes (viewport.refused).")
			.takes(request_params({ F::Viewport }, { F::Path }))
			.names_active()
			.row,
	// What a viewport's canvas would raise, planned by the viewport (S13 V7). It holds nothing of its
	// own: each request its plan makes meets its own row's gate as it is served (an edit_record's
	// waits for an operation that holds the documents; a frame's set_viewport runs beside any).
	Request(K::EditInViewport, "edit_in_viewport", serve_edit_in_viewport,
			"A drag, a command or a drop in the viewport over the document at path (left out, the "
			"active one; of the kind the drag, the command or the drop names, else the Preview's kind "
			"that shows it, else its Main view), planned as its canvas plans it and served, each "
			"request it plans "
			"meeting its own row's gate: drag, one batch of the edits under one gesture over every "
			"selected record the drag moves (a gesture's samples, consecutive drags of one handle "
			"on its document, one undo step, which end ends; the outcome's gesture names it); "
			"command, one request (a menu's arrange of windows, a model's frame of its camera, which "
			"runs beside any operation); drop, one batch of what the thing dropped makes at the "
			"point, one undo step. Refused, nothing changed, naming a document not open, a "
			"viewport that does not show it as it is now, a record or a handle it does not show, a "
			"command it has not, a gesture the document holds no open one of, a drag that writes "
			"nothing the session takes, or a drop the viewport does not take (viewport.refused); a "
			"planned edit the session refuses is not done.")
			.takes(request_params({}, { F::Path, F::Drag, F::Command, F::Drop }))
			.names_active()
			.row,
	// What the windows show of their own (the MCP gaps lane): no file and no document, so it runs beside any
	// operation, as a viewport's state does.
	Request(K::SetWorkspace, "set_workspace", serve_set_workspace,
			"What the windows show of their own set as workspace says, {<part>: {<member>: value}}, each part "
			"and member it leaves out as it is (the workspace section shows it, the windows draw it; the "
			"catalog's workspace lists every part and member): a file's card (card {path}, \"\" closing it and "
			"stopping its sound), the build result's panel, the new-project form (nothing made until "
			"new_project), Project settings (its fields until apply_project_settings), Files' New file prompt, "
			"filter and Rename..., Rename everywhere and Rename back, the find bar, Find in project, Problems' "
			"filters and the confirmation a Fix all or a Use fix waits in, and what a document's views show of "
			"it (document {path, ...}: its outline's filter and kinds, the Inspector's filter, a menu's new "
			"window type and Remove screen prompt, a texture's palette remap); focus brings a window forward "
			"(a focus_window view event; a window closed opens). Each part refused alone, nothing of it changed "
			"(workspace.refused, in the outcome alone: no Problems row, the status line as it was): a part or a "
			"member the table has not, a value of another type, a text longer than its window's field holds, a "
			"file or a document the project lacks, a value no window of it shows (a kind, a screen, a window "
			"type, a confirmation of nothing Problems offers, an import plan made since), a field of a part that "
			"is closed. Of the dialogs that take the whole editor one shows at a time (the section's modal), the "
			"others held until it closes. A window's own changes are view state: the status line a refused "
			"request left stays.")
			.takes(request_params({ F::Workspace }))
			.background()
			.row,
	// The sound is the session's state, the Shell playing what it says and reporting how it goes
	// (ProjectSession::report_sound), so a play is seen in the workspace section.
	Request(K::PlaySound, "play_sound", serve_play_sound,
			"A sound played by the editor as the game plays it, once, in place of any sound it plays: the "
			"workspace section's sound says how it stands (starting until the Shell has decoded it, playing, "
			"ended, stopped, failed with why; a headless editor plays nothing, its sound staying starting), "
			"and for a set its set, bank, words and voices (each a wave at the pitch and volume the game's pick "
			"gave it). With no values: the project's wave at path (a project-relative path or a logical name) "
			"as recorded. values {set}: the sound set of that name, from the bank path names, else from the "
			"first bank of the game's search holding it (an expansion's <n>L.lwf and <n>.lwf, gamelocl.lwf, "
			"game.lwf, game3.lwf, game2.lwf), each layer's member picked and its pitch composed as the game "
			"does. values {profile?, slot}: the SndProf.def profile's slot (its keyword or 0 to 50; profile "
			"left out: default). values {profile?, surface, foot?}: the footstep that profile plays on a "
			"surface (ground, snow, object, water) with that foot (left, right), the slot the game's test "
			"picks. Refused (workspace.refused): a name no wave of the project has, a set no bank searched "
			"holds, an empty slot, waves the project lacks, a wave past what a card reads.")
			.takes(request_params({}, { F::Path, F::Values }))
			.row,
	Request(K::StopSound, "stop_sound", serve_stop_sound,
			"The sound the editor plays stopped (the workspace section's sound: stopped).")
			.row,
	// Problems' Apply (the MCP gaps lane: session/problem_confirmation.h): what it raises are requests of their
	// own, each through the busy gate and its guard as it is served.
	Request(K::ApplyConfirmation, "apply_confirmation", serve_apply_confirmation,
			"The confirmation Problems holds open (the workspace's problems.confirm: a Fix all, the summary's, a "
			"Use fix) applied as its Apply applies it: what it proposes now (the workspace section's "
			"problems.confirm.proposal: one create_missing naming every role, one import list naming every file, "
			"a Use fix's assign_requirement), each request served in turn, and the confirmation closed. Refused "
			"(workspace.refused), nothing raised: none open, or it proposes nothing now (the problems it was for "
			"are gone).")
			.row,
	Request(K::Quit, "quit", serve_quit,
			"The editor quits once the prompt has asked about unsaved edits; the running operation "
			"is cancelled first (one that cannot be keeps the editor open).")
			.holds(kNone, kHoldsAll, OnBusy::CancelRunning)
			.guarded(GuardScope::AllDirty, "Quit", "Save all")
			.can_discard()
			.acts_on_saved()
			.row,
	Request(K::PickDirectory, "pick_directory", nullptr,
			"A native folder dialog for purpose; its answer comes back as the request the purpose "
			"makes (a person answers it: through the editor MCP, pass the path instead).")
			.served_by(ServedBy::ShellNeedsPerson)
			.takes(request_params({ F::Purpose }))
			.row,
	Request(K::PickFile, "pick_file", nullptr,
			"A native file dialog for purpose, as pick_directory (import_files picks several).")
			.served_by(ServedBy::ShellNeedsPerson)
			.takes(request_params({ F::Purpose }))
			.row,
	Request(K::RevealPath, "reveal_path", nullptr,
			"The file or folder path shown in the OS file manager.")
			.served_by(ServedBy::Shell)
			.takes(request_params({ F::Path }))
			.row,
};

static_assert(
		std::size(kRows) == kEditorRequestKindCount, "every request kind has exactly one row");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kEditorRequestKindCount; ++i)
		if (kRows[i].kind != static_cast<EditorRequestKind>(i))
			return false;
	return true;
}
static_assert(rows_in_order(), "the request kind rows follow the enum's order");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Every row has a token of its own and says what it does.
constexpr bool rows_named() {
	for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
		if (!kRows[i].token[0] || !kRows[i].doc || !kRows[i].doc[0])
			return false;
		for (size_t j = i + 1; j < kEditorRequestKindCount; ++j)
			if (same_text(kRows[i].token, kRows[j].token))
				return false;
	}
	return true;
}
static_assert(rows_named(), "each request kind has a token of its own and a doc");

// The session serves a row by its handler; a shell row has none, and holds, guards and ends
// nothing of the session's.
constexpr bool handlers_where_served() {
	for (const RequestKindRow &row : kRows) {
		const bool session = row.served_by == ServedBy::Session;
		if (session != (row.handler != nullptr))
			return false;
		if (!session &&
				(row.guard != GuardScope::None || row.acts_on_saved || row.names_active ||
						row.ends_edit_groups || row.reads != kNone ||
						row.writes != kNone))
			return false;
	}
	return true;
}
static_assert(handlers_where_served(), "a session row has a handler and a shell row has none");

// The prompt offers Discard only for a request it guards whose plan writes nothing over the files:
// an import's or a rename's Save writes the edits first, as Build's and Play's does.
constexpr bool discard_only_where_nothing_is_written() {
	for (const RequestKindRow &row : kRows)
		if (row.can_discard &&
				(row.guard == GuardScope::None || row.guard == GuardScope::PlannedWrites))
			return false;
	return true;
}
static_assert(discard_only_where_nothing_is_written(),
		"Discard is offered only where the files are not written over");

// A request the prompt guards has its words (what waits, its Save), and a request it does not
// guard has none.
constexpr bool prompt_words_where_guarded() {
	for (const RequestKindRow &row : kRows) {
		const bool guarded = row.guard != GuardScope::None;
		if (guarded != (row.waiting != nullptr && row.waiting[0] != 0) ||
				guarded != (row.save_label != nullptr && row.save_label[0] != 0))
			return false;
	}
	return true;
}
static_assert(prompt_words_where_guarded(), "the prompt's words are a guarded row's");

// A request that must carry a field takes it; one whose empty path names the active document takes
// a path and never needs one; one the prompt guards by its document names it by its path.
constexpr bool params_hold() {
	for (const RequestKindRow &row : kRows) {
		if ((row.params.required & ~row.params.takes) != 0)
			return false;
		if (row.names_active &&
				(!row.params.has(RequestFieldId::Path) || row.params.needs(RequestFieldId::Path)))
			return false;
		if (row.guard == GuardScope::Document && !row.params.has(RequestFieldId::Path))
			return false;
	}
	return true;
}
static_assert(params_hold(), "each row's params hold its other columns");

// The viewport rows (S13 V7): a viewport's change and an edit in a viewport name the active
// document's viewport when their path is left out, as every pathless request names the active
// document; an edit in a viewport holds nothing of its own, each request its plan makes meeting its
// own row's gate as it is served (a frame's set_viewport beside any operation, an edit_record's not
// beside one that holds the documents), and ends no edit group (its drags are a gesture's).
constexpr bool viewport_rows_hold() {
	for (const RequestKindRow &row : kRows) {
		const bool edits = row.params.has(F::Drag) || row.params.has(F::Command);
		if ((edits || row.params.has(F::Viewport)) && !row.names_active) return false;
		if (edits && (row.reads != kNone || row.writes != kNone || row.ends_edit_groups)) return false;
	}
	return true;
}
static_assert(viewport_rows_hold(),
		"a viewport row names the active document's viewport, and an edit in a viewport holds nothing of its own");

// --- the busy gate
// --------------------------------------------------------------------------------

// A Build or a Play onto the running build: the build serves it (a Play refused before it could,
// as it would be before any build: no spawn here, a game running, or a mission the project does
// not hold). The outcome names the operation joined.
void join_operation(SessionCore &core, const EditorRequest &request) {
	if (request.kind == EditorRequestKind::Play && core.play().refused(request.mission))
		return;
	core.operations().running()->join(request);
	core.outcome().operation = core.operations().status().id;
	if (request.kind != EditorRequestKind::Build || request.report) core.report_build();
	if (request.kind == EditorRequestKind::Play) {
		core.view().activity.status = "Building, then playing...";
		core.note("Play starts the game when the build lands.");
	}
	if (request.kind == EditorRequestKind::Export) {
		core.view().activity.status = "Building, then exporting...";
		core.note("Export copies the build when it lands.");
	}
}

// The busy gate (gate_answer): the request's row and the running operation's weighed, the one
// answer the windows disable by (busy_refuses). Refused: an operation.busy warning, the outcome not
// done, nothing changed. Joined: the running operation serves it (a Build or a Play onto a build).
// Superseded: the running operation is cancelled for it (a new import plan over a running one).
// CancelRunning: the request goes on, and its own flow cancels the operation when it commits
// (close_project, Quit), once its own checks pass: a NewProject that fails keeps the build. A
// request that conflicts with nothing it reads or writes (an edit, an open, an import's preview
// while a build packs) goes on. One the unsaved-changes prompt would hold, and which the gate does
// not refuse, asks first and meets the gate once it goes ahead: a Close the prompt then drops
// cancels nothing, and a Build with unsaved edits never joins a build that packs the files without
// them. True when the request was refused or joined the running operation.
bool gate_busy(SessionCore &core, const EditorRequest &request) {
	const GateAnswer answer = gate_answer(request.kind, core.operations().status());
	if (answer == GateAnswer::Proceed)
		return false;
	if (answer != GateAnswer::Refuse) {
		std::vector<std::string> unsaved;
		if (core.guard().files(request, unsaved) && !unsaved.empty())
			return false;
	}
	switch (answer) {
		case GateAnswer::CancelRunning:
			return false;
		case GateAnswer::Join:
			join_operation(core, request);
			return true;
		case GateAnswer::Supersede:
			if (core.cancel_operation(false))
				return false;
			break;
		default:
			break;
	}
	// The refusal names what the request named: the project a switch opens, else its file.
	core.refuse_busy(request.dir.empty() ? request.path : request.dir);
	return true;
}

} // namespace

const RequestKindRow &request_kind_row(EditorRequestKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return kRows[index < kEditorRequestKindCount
					? index
					: static_cast<size_t>(EditorRequestKind::RevealPath)];
}

bool request_kind_from_token(const std::string &token, EditorRequestKind &out) {
	for (const RequestKindRow &row : kRows) {
		if (token == row.token) {
			out = row.kind;
			return true;
		}
	}
	return false;
}

GateAnswer gate_answer(EditorRequestKind kind, const OperationStatus &running) {
	if (!running.running())
		return GateAnswer::Proceed;
	const RequestKindRow &request = request_kind_row(kind);
	const OperationKindRow &operation = operation_kind_row(running.kind);
	if (request.on_busy == OnBusy::Join && operation.joined_by.has(kind))
		return GateAnswer::Join;
	// What would take the place of an operation that cannot be cancelled waits for it.
	if (request.on_busy == OnBusy::Supersede && operation.superseded_by.has(kind))
		return running.cancellable ? GateAnswer::Supersede : GateAnswer::Refuse;
	if (!holds_conflict(request.reads, request.writes, running.reads, running.writes))
		return GateAnswer::Proceed;
	if (request.on_busy == OnBusy::CancelRunning && running.cancellable)
		return GateAnswer::CancelRunning;
	return GateAnswer::Refuse;
}

bool busy_refuses(EditorRequestKind kind, const OperationStatus &running) {
	return gate_answer(kind, running) == GateAnswer::Refuse;
}

bool busy_refuses_answer(
		EditorRequestKind waiting, UnsavedChoice choice, const OperationStatus &running) {
	if (!running.running() || choice == UnsavedChoice::Cancel)
		return false;
	const GateAnswer answer = gate_answer(waiting, running);
	if (answer == GateAnswer::Refuse)
		return true;
	if (answer == GateAnswer::CancelRunning)
		return false; // it cancels the operation first
	const RequestKindRow &save_all = request_kind_row(EditorRequestKind::SaveAll);
	return choice == UnsavedChoice::Save
			? holds_conflict(save_all.reads, save_all.writes, running.reads, running.writes)
			: holds_conflict(HoldsNothing, HoldsDocuments, running.reads, running.writes);
}

bool serve_request(SessionCore &core, const EditorRequest &request) {
	const RequestKindRow &row = request_kind_row(request.kind);
	if (!row.handler)
		return false;
	// Build and Play pack the files as saved: every edit group ends first, as EndEdit ends one, so
	// a keystroke after them is a step of its own.
	if (row.ends_edit_groups)
		core.documents().end_edit_groups();
	if (gate_busy(core, request))
		return true;
	// The dialog whose button the request is closes as it is taken (the MCP gaps lane: workspace_parts.h).
	workspace_closes_for(core, request);
	if (core.guard().holds(request))
		return true;
	row.handler(core, request);
	workspace_follows(core, request);
	// What the workspace holds kept true to what the request changed (a file gone, a screen removed).
	workspace_tidies(core);
	return true;
}

std::string waiting_words(EditorRequestKind kind, const std::string &target) {
	const RequestKindRow &row = request_kind_row(kind);
	if (!row.waiting)
		return std::string();
	std::string words = row.waiting;
	const size_t at = words.find("%s");
	if (at != std::string::npos)
		words.replace(at, 2, basename_of(target));
	return words;
}

} // namespace opennova::editor
