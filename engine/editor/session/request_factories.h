#pragma once

#include <algorithm>
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
// A project made as new_project makes one, building as the expansion `expansion` on the installed one
// `builds_on` ("" the base game; ADR 0046 S16).
inline EditorRequest new_expansion_project(std::string dir, std::string title, std::string expansion,
		std::string builds_on = std::string(), bool import_pass = true) {
	EditorRequest request = new_project(std::move(dir), std::move(title), std::string(), import_pass);
	request.expansion = std::move(expansion);
	request.builds_on = std::move(builds_on);
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
// The folder `game_install` read as a game install ("" the editor's last chosen): the view's install_check.
inline EditorRequest check_install(std::string game_install = std::string()) {
	EditorRequest request = of(EditorRequestKind::CheckInstall);
	request.game_install = std::move(game_install);
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
// The open import preview's checked rows imported as the dialog's Import takes them, of the plan `plan`
// (DialogsView::ImportPreview::plan_serial).
inline EditorRequest import_planned(uint64_t plan, bool replace = false) {
	EditorRequest request = of(EditorRequestKind::ImportFiles);
	request.planned = true;
	request.plan = plan;
	request.replace = replace;
	return request;
}
// Problems' confirmation applied as its Apply applies it.
inline EditorRequest apply_confirmation() {
	return of(EditorRequestKind::ApplyConfirmation);
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
// The import record of the source `path` names (or of the import a file of that path or name comes
// from) given `values`, each an option's key and value ("" its default), then imported again (S18).
inline EditorRequest set_import_options(std::string path, std::vector<std::pair<std::string, std::string>> values) {
	EditorRequest request = of(EditorRequestKind::SetImportOptions);
	request.path = std::move(path);
	std::sort(values.begin(), values.end());
	request.values = std::move(values);
	return request;
}
// The texture document at `path` edited as a whole image (S18: documents/texture_operations.h): the
// operation `operation` with its `params`, one undo step; `open_first`: the document opened first (a fix).
inline EditorRequest texture_operation(std::string path, std::string operation,
                                       std::vector<std::pair<std::string, std::string>> params = {}, bool open_first = false) {
	EditorRequest request = of(EditorRequestKind::TextureOperation);
	request.path = std::move(path);
	request.operation = std::move(operation);
	std::sort(params.begin(), params.end());
	request.values = std::move(params);
	request.open_first = open_first;
	return request;
}
// The texture `path` (a project file, an import's output, or a name the project lacks) made from the image
// `image` (a PNG, a TGA or a PCX on disk, or a project file), its import's options `values` over the ones
// that make it as it is stored (S18: import/texture_source.h).
inline EditorRequest replace_texture(std::string path, std::string image,
                                     std::vector<std::pair<std::string, std::string>> values = {}) {
	EditorRequest request = of(EditorRequestKind::ReplaceTexture);
	request.path = std::move(path);
	request.paths = {std::move(image)};
	std::sort(values.begin(), values.end());
	request.values = std::move(values);
	return request;
}
// A terrain named `name` made from images (S20: import/terrain_import.h): `values` names them
// (heightmap, colormap, detail, tiles, surface) and the importer's options (top, water, layout).
inline EditorRequest new_terrain(std::string name, std::vector<std::pair<std::string, std::string>> values) {
	EditorRequest request = of(EditorRequestKind::NewTerrain);
	request.path = std::move(name);
	std::sort(values.begin(), values.end());
	request.values = std::move(values);
	return request;
}
// The texture `path` copied as `new_name`, the uses in the project files `referrers` moved to the copy
// (S18: graph/rename_transaction.h plan_split).
inline EditorRequest split_texture(std::string path, std::string new_name, std::vector<std::string> referrers) {
	EditorRequest request = of(EditorRequestKind::SplitTexture);
	request.path = std::move(path);
	request.new_name = std::move(new_name);
	request.paths = std::move(referrers);
	return request;
}
// The texture `path`'s source opened in its program (S18: made once for a plain texture).
inline EditorRequest edit_externally(std::string path) {
	EditorRequest request = of(EditorRequestKind::EditExternally);
	request.path = std::move(path);
	return request;
}
// What changed on disk among the import sources imported again (S18: the Shell's on its window's focus).
inline EditorRequest refresh_changed_sources() { return of(EditorRequestKind::RefreshChangedSources); }
// What a Replace of the texture `path` by the image `image` (none: an Edit externally) would do, asked in
// the dialog before it is done (S18), `values` the options asked over the ones reproducing its form.
inline EditorRequest preview_texture_source(std::string path, std::string image = std::string(),
                                            std::vector<std::pair<std::string, std::string>> values = {}) {
	EditorRequest request = of(EditorRequestKind::PreviewTextureSource);
	request.path = std::move(path);
	if (!image.empty()) request.paths = {std::move(image)};
	std::sort(values.begin(), values.end());
	request.values = std::move(values);
	return request;
}
inline EditorRequest cancel_texture_source() { return of(EditorRequestKind::CancelTextureSource); }
// The texture `path`'s existing source opened in its program, nothing written (S18).
inline EditorRequest open_texture_source(std::string path) {
	EditorRequest request = of(EditorRequestKind::OpenTextureSource);
	request.path = std::move(path);
	return request;
}
// The texture `path`'s use in the project file `referrer` (at `locator` and `field`, when it has several)
// shown where the game draws it (S18: session/texture_show_use.h).
inline EditorRequest show_use(std::string path, std::string referrer, std::string locator = std::string(),
                              std::string field = std::string()) {
	EditorRequest request = of(EditorRequestKind::ShowUse);
	request.path = std::move(path);
	request.paths = {std::move(referrer)};
	request.locator = std::move(locator);
	request.field = std::move(field);
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
// The import dialog on every file of the game install, chosen at once, with no walk (ADR 0046
// S14: the closure of everything is everything).
inline EditorRequest import_whole_install() {
	EditorRequest request = of(EditorRequestKind::PreviewInstallImport);
	request.all = true;
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
// A build, then the game run on it: at its menu, or in `mission` (a .bms of the project by its
// logical name; S14); `behind`, its window behind every other (the MCP gaps lane); `fresh`, its run
// directory emptied first of what the runs before wrote there (a first run).
inline EditorRequest play(std::string mission = std::string(), bool behind = false, bool fresh = false) {
	EditorRequest request = of(EditorRequestKind::Play);
	request.mission = std::move(mission);
	request.behind = behind;
	request.fresh = fresh;
	return request;
}
// A build, then the build copied into `export_dir` ("" the project's export folder) as what ships
// (ADR 0046 S16); with `rehash`, every file read again.
inline EditorRequest export_project(std::string export_dir = std::string(), bool rehash = false) {
	EditorRequest request = of(EditorRequestKind::Export);
	request.export_dir = std::move(export_dir);
	request.rehash = rehash;
	return request;
}
inline EditorRequest stop_play() {
	return of(EditorRequestKind::StopPlay);
}
inline EditorRequest cancel_operation() {
	return of(EditorRequestKind::CancelOperation);
}

// --- files and documents -----------------------------------------------------------------------

// A blank file `path`; `file_kind` (an asset kind's token) where its name cannot say its kind;
// `values`, what its blank takes (a mission's title, terrain and environment), by token, sorted by
// it as the wire reads them (an object's keys are written sorted), so a request equals its round
// trip whatever order its prompt gave them (review F10).
inline EditorRequest create_file(std::string path, std::string file_kind = std::string(),
		std::vector<std::pair<std::string, std::string>> values = {}) {
	EditorRequest request = of(EditorRequestKind::CreateFile);
	request.path = std::move(path);
	request.file_kind = std::move(file_kind);
	std::sort(values.begin(), values.end());
	request.values = std::move(values);
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
// Files' card of the project file `path` (the UX round's project lane).
inline EditorRequest about_file(std::string path) {
	EditorRequest request = of(EditorRequestKind::AboutFile);
	request.path = std::move(path);
	return request;
}
// The project's wave at `path` played (the workspace's sound, the Shell's player), and stopped.
inline EditorRequest play_sound(std::string path) {
	EditorRequest request = of(EditorRequestKind::PlaySound);
	request.path = std::move(path);
	return request;
}
// A sound set played as the game plays it (session/sound_play.h): from `bank` ("" the game's search).
inline EditorRequest play_set(std::string set, std::string bank = std::string()) {
	EditorRequest request = of(EditorRequestKind::PlaySound);
	request.path = std::move(bank);
	request.values = {{"set", std::move(set)}};
	return request;
}
// A SndProf.def profile's slot played (its keyword or number); the footstep a foot plays on a surface.
inline EditorRequest play_profile_slot(std::string profile, std::string slot) {
	EditorRequest request = of(EditorRequestKind::PlaySound);
	request.values = {{"profile", std::move(profile)}, {"slot", std::move(slot)}};
	return request;
}
inline EditorRequest play_footstep(std::string profile, std::string surface, std::string foot) {
	EditorRequest request = of(EditorRequestKind::PlaySound);
	request.values = {{"profile", std::move(profile)}, {"surface", std::move(surface)}, {"foot", std::move(foot)}};
	return request;
}
// What the clip the animation document `path` plays fires at `frame`, once (DI-04: a timeline mark pressed).
inline EditorRequest play_clip_event(std::string path, int frame) {
	EditorRequest request = of(EditorRequestKind::PlaySound);
	request.path = std::move(path);
	request.values = {{"frame", std::to_string(frame)}};
	return request;
}
inline EditorRequest stop_sound() {
	return of(EditorRequestKind::StopSound);
}
// The project file `path` selected in Files ("" none, ADR 0046 S18).
inline EditorRequest select_file(std::string path) {
	EditorRequest request = of(EditorRequestKind::SelectFile);
	request.path = std::move(path);
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
// Back (Forward) `steps` places of the navigation history: the nearest, or one further down Back's
// (Forward's) list.
inline EditorRequest navigate_back(uint32_t steps = 1) {
	EditorRequest request = of(EditorRequestKind::NavigateBack);
	request.steps = steps;
	return request;
}
inline EditorRequest navigate_forward(uint32_t steps = 1) {
	EditorRequest request = of(EditorRequestKind::NavigateForward);
	request.steps = steps;
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
// The words the player sees for a field's string id set to `text`, in the table that defines the id.
inline EditorRequest set_string_text(std::string path, NodeAddress address, std::string field, std::string text) {
	EditorRequest request = of(EditorRequestKind::SetStringText);
	request.path = std::move(path);
	request.address = address;
	request.field = std::move(field);
	request.values = {{"text", std::move(text)}};
	return request;
}
inline EditorRequest end_edit(std::string path = std::string()) {
	EditorRequest request = of(EditorRequestKind::EndEdit);
	request.path = std::move(path);
	return request;
}
// The viewport over the document at `path` ("" the active one) changed as `viewport` says: the JSON
// text of an object {kind?, device?, clock?, and the kind's options and camera} (S13 V5,
// preview/viewports.h).
inline EditorRequest set_viewport(std::string path, std::string viewport) {
	EditorRequest request = of(EditorRequestKind::SetViewport);
	request.path = std::move(path);
	request.viewport = std::move(viewport);
	return request;
}
// What the windows show of their own changed as `workspace` says: the JSON text of an object
// {<part>: {<member>: value}, focus?} (the MCP gaps lane, session/workspace_parts.h).
inline EditorRequest set_workspace(std::string workspace) {
	EditorRequest request = of(EditorRequestKind::SetWorkspace);
	request.workspace = std::move(workspace);
	return request;
}
// A drag, or a command, in the viewport over the document at `path` ("" the active one), planned by
// the viewport as its canvas would plan it (S13 V7).
inline EditorRequest edit_in_viewport(std::string path, ViewportDrag drag) {
	EditorRequest request = of(EditorRequestKind::EditInViewport);
	request.path = std::move(path);
	request.drag = std::move(drag);
	return request;
}
inline EditorRequest edit_in_viewport(std::string path, ViewportCommand command) {
	EditorRequest request = of(EditorRequestKind::EditInViewport);
	request.path = std::move(path);
	request.command = std::move(command);
	return request;
}
// A drop on the picture of the viewport over the document at `path` ("" the active one), planned by
// the viewport (S14: a Files row or a picked name let go on a mission's canvas).
inline EditorRequest edit_in_viewport(std::string path, ViewportDrop drop) {
	EditorRequest request = of(EditorRequestKind::EditInViewport);
	request.path = std::move(path);
	request.drop = std::move(drop);
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

// The last rename's way back planned (only the sites it rewrote), nothing written; the Rename back dialog
// opens when `ask_name`.
inline EditorRequest preview_rename_back(bool ask_name = false) {
	EditorRequest request = of(EditorRequestKind::PreviewRenameBack);
	request.ask_name = ask_name;
	return request;
}
// The last rename taken back, on disk.
inline EditorRequest rename_back() { return of(EditorRequestKind::RenameBack); }

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
