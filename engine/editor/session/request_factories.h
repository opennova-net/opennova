#pragma once

#include <string>
#include <utility>
#include <vector>

#include <editor/session/editor_request.h>

// A request of each kind with the fields its row takes (ADR 0046 S13 A4, request_kinds.cpp), so a
// caller names what the request means and never a field another kind would read. A field left at
// its default is one the request does not carry; the wire form writes only what a kind takes.
namespace opennova::editor::request {

// A request of `kind` carrying no field: what a loop over the kinds makes (a kind that needs a
// field is then refused where the session reads it, as its row would refuse it on the wire).
inline EditorRequest of(EditorRequestKind kind) {
	EditorRequest request;
	request.kind = kind;
	return request;
}

// --- the project -------------------------------------------------------------------------------

// A project made in `dir` (titled `title`, else the folder's name; of `game`, a gameprofile code,
// else jo), then opened, with its import pass unless `import_pass` is false.
inline EditorRequest new_project(std::string dir, std::string title = std::string(),
		std::string game = std::string(), bool import_pass = true) {
	EditorRequest request = of(EditorRequestKind::NewProject);
	request.dir = std::move(dir);
	request.title = std::move(title);
	request.game = std::move(game);
	request.import_pass = import_pass;
	return request;
}
// The project in `dir` opened, its import pass first unless `import_pass` is false (its files as
// they are, no source imported), on `game_install` for the session alone when given.
inline EditorRequest open_project(
		std::string dir, bool import_pass = true, std::string game_install = std::string()) {
	EditorRequest request = of(EditorRequestKind::OpenProject);
	request.dir = std::move(dir);
	request.import_pass = import_pass;
	request.game_install = std::move(game_install);
	return request;
}
inline EditorRequest close_project() {
	return of(EditorRequestKind::CloseProject);
}
inline EditorRequest forget_recent(std::string dir) {
	EditorRequest request = of(EditorRequestKind::ForgetRecent);
	request.dir = std::move(dir);
	return request;
}
inline EditorRequest rescan() {
	return of(EditorRequestKind::Rescan);
}
inline EditorRequest apply_project_settings(ProjectSettingsChange settings) {
	EditorRequest request = of(EditorRequestKind::ApplyProjectSettings);
	request.settings = std::move(settings);
	return request;
}
inline EditorRequest create_missing(std::vector<std::string> roles) {
	EditorRequest request = of(EditorRequestKind::CreateMissing);
	request.roles = std::move(roles);
	return request;
}
inline EditorRequest quit() {
	return of(EditorRequestKind::Quit);
}
inline EditorRequest clear_output() {
	return of(EditorRequestKind::ClearOutput);
}

// --- imports -----------------------------------------------------------------------------------

inline EditorRequest preview_import(std::vector<std::string> paths, bool with_dependencies) {
	EditorRequest request = of(EditorRequestKind::PreviewImport);
	request.paths = std::move(paths);
	request.with_dependencies = with_dependencies;
	return request;
}
inline EditorRequest plan_import(std::vector<ImportChoice> imports, bool with_dependencies) {
	EditorRequest request = of(EditorRequestKind::PlanImport);
	request.imports = std::move(imports);
	request.with_dependencies = with_dependencies;
	return request;
}
inline EditorRequest set_import_dependencies(bool with_dependencies) {
	EditorRequest request = of(EditorRequestKind::SetImportDependencies);
	request.with_dependencies = with_dependencies;
	return request;
}
inline EditorRequest import_files(std::vector<ImportChoice> imports, bool replace = false) {
	EditorRequest request = of(EditorRequestKind::ImportFiles);
	request.imports = std::move(imports);
	request.replace = replace;
	return request;
}
inline EditorRequest cancel_import() {
	return of(EditorRequestKind::CancelImport);
}
// The source at `path` ("" every source) imported again, even when unchanged when `force`.
inline EditorRequest reimport(std::string path = std::string(), bool force = false) {
	EditorRequest request = of(EditorRequestKind::Reimport);
	request.path = std::move(path);
	request.force = force;
	return request;
}
// The import dialog on the game install's files: `names` alone, chosen; none, every file listed.
inline EditorRequest preview_install_import(
		std::vector<std::string> names = {}, bool with_dependencies = false) {
	EditorRequest request = of(EditorRequestKind::PreviewInstallImport);
	request.names = std::move(names);
	request.with_dependencies = with_dependencies;
	return request;
}

// --- the build and Play ------------------------------------------------------------------------

// The project packed into a build under `out_dir` ("" the project's .opennova/build/play); with
// `rehash`, every file read again, the build cache set aside.
inline EditorRequest build(std::string out_dir = std::string(), bool rehash = false) {
	EditorRequest request = of(EditorRequestKind::Build);
	request.out_dir = std::move(out_dir);
	request.rehash = rehash;
	return request;
}
inline EditorRequest play() {
	return of(EditorRequestKind::Play);
}
inline EditorRequest stop_play() {
	return of(EditorRequestKind::StopPlay);
}
inline EditorRequest cancel_operation() {
	return of(EditorRequestKind::CancelOperation);
}

// --- files and documents -----------------------------------------------------------------------

// A blank file `path`; `file_kind` (an asset kind's token) where its name cannot say its kind.
inline EditorRequest create_file(std::string path, std::string file_kind = std::string()) {
	EditorRequest request = of(EditorRequestKind::CreateFile);
	request.path = std::move(path);
	request.file_kind = std::move(file_kind);
	return request;
}
// The document at `path` opened (made active when it is open), the record at `locator` selected
// (a Go to) and its `field` shown.
inline EditorRequest open_document(
		std::string path, std::string locator = std::string(), std::string field = std::string()) {
	EditorRequest request = of(EditorRequestKind::OpenDocument);
	request.path = std::move(path);
	request.locator = std::move(locator);
	request.field = std::move(field);
	return request;
}
// The document at `path` opened with the record at `address` selected (a Problems row's place)
// and its `field` shown.
inline EditorRequest open_record(
		std::string path, NodeAddress address, std::string field = std::string()) {
	EditorRequest request = of(EditorRequestKind::OpenDocument);
	request.path = std::move(path);
	request.address = address;
	request.field = std::move(field);
	return request;
}
// Files shows the project file `path`, and asks its new name when `ask_name` (Rename...).
inline EditorRequest show_in_files(std::string path, bool ask_name = false) {
	EditorRequest request = of(EditorRequestKind::ShowInFiles);
	request.path = std::move(path);
	request.ask_name = ask_name;
	return request;
}
inline EditorRequest reload_document(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::ReloadDocument);
	request.path = std::move(path);
	return request;
}
inline EditorRequest close_document(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::CloseDocument);
	request.path = std::move(path);
	return request;
}
inline EditorRequest save(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::Save);
	request.path = std::move(path);
	return request;
}
inline EditorRequest save_all() {
	return of(EditorRequestKind::SaveAll);
}
inline EditorRequest reveal_path(std::string path) {
	EditorRequest request = of(EditorRequestKind::RevealPath);
	request.path = std::move(path);
	return request;
}

// --- records -----------------------------------------------------------------------------------

// The record at `address` (the primary) and the `records` named with it (a marquee's, of any rows)
// selected in the document at `path` as `mode` says.
inline EditorRequest select_record(std::string path, NodeAddress address,
		SelectMode mode = SelectMode::Replace, std::vector<NodeAddress> records = {}) {
	EditorRequest request = of(EditorRequestKind::SelectRecord);
	request.path = std::move(path);
	request.address = address;
	request.records = std::move(records);
	request.mode = mode;
	return request;
}
// A batch over any rows of the document at `path`, one undo step; opened first when it is not and
// `open_first` (a fix's edit).
inline EditorRequest edit_record(
		std::string path, std::vector<Edit> edits, bool open_first = false) {
	EditorRequest request = of(EditorRequestKind::EditRecord);
	request.path = std::move(path);
	request.edits = std::move(edits);
	request.open_first = open_first;
	return request;
}
// One edit: a batch of one.
inline EditorRequest edit_record(std::string path, Edit edit, bool open_first = false) {
	return edit_record(std::move(path), std::vector<Edit>{ std::move(edit) }, open_first);
}
// Each field `targets` names (their address and field) given back what the saved file holds.
inline EditorRequest revert_to_saved(std::string path, std::vector<Edit> targets) {
	EditorRequest request = of(EditorRequestKind::RevertToSaved);
	request.path = std::move(path);
	request.edits = std::move(targets);
	return request;
}
inline EditorRequest end_edit(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::EndEdit);
	request.path = std::move(path);
	return request;
}
inline EditorRequest copy(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::Copy);
	request.path = std::move(path);
	return request;
}
inline EditorRequest cut(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::Cut);
	request.path = std::move(path);
	return request;
}
// The clipboard pasted into the document at `path` after the selection, or where `at` names.
inline EditorRequest paste(std::string path = std::string(), PasteAt at = PasteAt()) {
	EditorRequest request = of(EditorRequestKind::Paste);
	request.path = std::move(path);
	request.paste_at = at;
	return request;
}
inline EditorRequest duplicate(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::Duplicate);
	request.path = std::move(path);
	return request;
}
inline EditorRequest undo(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::Undo);
	request.path = std::move(path);
	return request;
}
inline EditorRequest redo(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::Redo);
	request.path = std::move(path);
	return request;
}
inline EditorRequest resolve_unsaved(UnsavedChoice choice) {
	EditorRequest request = of(EditorRequestKind::ResolveUnsaved);
	request.choice = choice;
	return request;
}

// --- renames -----------------------------------------------------------------------------------

inline EditorRequest rename_asset(std::string path, std::string new_name) {
	EditorRequest request = of(EditorRequestKind::RenameAsset);
	request.path = std::move(path);
	request.new_name = std::move(new_name);
	return request;
}
inline EditorRequest assign_requirement(std::string role, std::string path) {
	EditorRequest request = of(EditorRequestKind::AssignRequirement);
	request.role = std::move(role);
	request.path = std::move(path);
	return request;
}
// What renaming the name the record at `locator` of `path` defines in `field` to `new_name`
// everywhere would do, planned, nothing written; the Rename everywhere dialog opens when
// `ask_name`.
inline EditorRequest preview_rename(std::string path, std::string locator, std::string field,
		std::string new_name, bool ask_name = false) {
	EditorRequest request = of(EditorRequestKind::PreviewRename);
	request.path = std::move(path);
	request.locator = std::move(locator);
	request.field = std::move(field);
	request.new_name = std::move(new_name);
	request.ask_name = ask_name;
	return request;
}
// What renaming the file `path` to `new_name` would do (Files' Rename...), planned.
inline EditorRequest preview_file_rename(std::string path, std::string new_name) {
	EditorRequest request = of(EditorRequestKind::PreviewRename);
	request.path = std::move(path);
	request.new_name = std::move(new_name);
	return request;
}
inline EditorRequest rename_symbol(
		std::string path, std::string locator, std::string field, std::string new_name) {
	EditorRequest request = of(EditorRequestKind::RenameSymbol);
	request.path = std::move(path);
	request.locator = std::move(locator);
	request.field = std::move(field);
	request.new_name = std::move(new_name);
	return request;
}

// --- the shell's -------------------------------------------------------------------------------

inline EditorRequest pick_directory(PickPurpose purpose) {
	EditorRequest request = of(EditorRequestKind::PickDirectory);
	request.purpose = purpose;
	return request;
}
inline EditorRequest pick_file(PickPurpose purpose) {
	EditorRequest request = of(EditorRequestKind::PickFile);
	request.purpose = purpose;
	return request;
}

} // namespace opennova::editor::request
