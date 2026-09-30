// S13 A1: the windows show the session's busy gate. Every menu item and button that raises a
// request (the File, Edit and Build menus, the menu bar's buttons and its list of unsaved files,
// the document toolbar, Files' tools and a file's menu, the Problems summary's fixes and their
// confirmation, the import dialog, Rename everywhere, the welcome view) is enabled exactly when
// the gate takes that request (SessionView::allows: busy_refuses over the running operation).
// Drawn on the null backend over a view where only the gate can hold a control back, under no
// operation and under the status of every operation kind, cancellable and not: each control
// pressed, and whether its request came (or, for one that opens a dialog first, the dialog
// opened) matched with the gate's answer for its kind. editor_session_operation's
// test_gate_is_what_busy_refuses_says holds the session to the same answer.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/mnu_document.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_run.h>
#include <editor/project_build/build_run.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_windows.h>
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

// An operation of `kind` as the slot shows it: what its row says it reads and writes.
OperationStatus status_of(OperationKind kind, bool cancellable) {
	OperationStatus status;
	status.id = 9;
	status.kind = kind;
	status.label = "Working";
	status.total = 100;
	status.unit = OperationUnit::Bytes;
	status.cancellable = cancellable;
	status.reads = operation_kind_row(kind).reads;
	status.writes = operation_kind_row(kind).writes;
	return status;
}

std::string status_name(const OperationStatus &status) {
	if (!status.running()) return "no operation";
	return std::string(operation_kind_row(status.kind).token) + (status.cancellable ? " (cancellable)" : " (not cancellable)");
}

bool modal_open(const char *title) {
	const ImGuiWindow *window = ImGui::FindWindowByName(title);
	return window && window->Active;
}

// The point along `x` in [top, bottom] where the mouse hovers the item `id`, found moving down.
bool hover_find(Ui &ui, ImGuiID id, float x, float top, float bottom, ImVec2 &at) {
	for (float y = top; y < bottom; y += 2.0f) {
		ui.mouse(x, y);
		if (GImGui->HoveredId == id) {
			at = ImVec2(x, y);
			return true;
		}
	}
	return false;
}

int factory_index(const char *role, AssetKind kind) {
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory *factory = blank_factory_at(i);
		if (std::string(factory->role) == role && factory->kind == kind) return static_cast<int>(i);
	}
	return -1;
}

// A field of the menu's TITLE window renamed: an unsaved edit, an undo step.
bool rename_title(MnuDocument &document, const char *name) {
	Edit rename;
	rename.address = named(document, "TITLE");
	if (!rename.address.row) rename.address = named(document, "HEADING");
	rename.field = "name";
	rename.value = std::string(name);
	Diagnostic error;
	return document.apply(rename, error);
}

// A control of the windows: the kind of request it raises, pressed by `press`, which answers
// whether that request came (or its dialog opened) and leaves no popup of its own open.
// `shown` says whether the control is drawn at all in the view as it stands (Cancel only while
// an operation that can be cancelled runs).
struct Probe {
	std::string name;
	EditorRequestKind kind;
	std::function<bool()> press;
	std::function<bool(const SessionView &)> shown;
};

// Every probe pressed under `status`: the ones whose answer is not the gate's reported.
void check_probes(Ui &ui, SessionView &v, const OperationStatus &status, const std::vector<Probe> &probes) {
	v.activity.operation = status;
	v.revisions.touch(ViewConcern::Operation);
	ui.frames(2);
	ui.away();
	ui.drain();
	for (const Probe &probe : probes) {
		if (probe.shown && !probe.shown(v)) continue;
		const bool came = probe.press();
		ImGui::ClosePopupsExceptModals();
		ui.frames(2);
		ui.drain();
		const bool allowed = v.allows(probe.kind);
		if (came != allowed) {
			const std::string message = probe.name + " under " + status_name(status) + ": " +
			                            (came ? "pressed" : "held back") + ", the gate " +
			                            (allowed ? "takes " : "refuses ") + editor_request_kind_token(probe.kind);
			CHECK(false, message.c_str());
		}
	}
}

// Whether a request of `kind` is among `requests`.
bool has(const std::vector<EditorRequest> &requests, EditorRequestKind kind) {
	return std::any_of(requests.begin(), requests.end(), [kind](const EditorRequest &request) { return request.kind == kind; });
}

} // namespace

void test_windows_show_the_gate() {
	editor_test::TempProjectDir dir("opennova_editor_ui_gate_test");
	// The active menu with an unsaved edit, an undo and a redo (two edits, one undone); a second
	// menu with an unsaved edit.
	const std::shared_ptr<MnuDocument> a = menu_at(dir, "a.mnu", "menus/a.mnu");
	const std::shared_ptr<MnuDocument> b = menu_at(dir, "b.mnu", "menus/b.mnu");
	CHECK(rename_title(*a, "HEADING") && rename_title(*a, "CAPTION") && rename_title(*b, "HEADING"), "the edits");
	a->undo();
	CHECK(a->dirty() && a->can_undo() && a->can_redo() && b->dirty(), "unsaved, an undo and a redo");
	SessionView v = menu_view(a);
	v.project.root = "C:/mods/Gate";
	v.documents.open = {a, b};
	editor_test::own(v.project.scan).entries = {file_entry("a.mnu", a->path(), AssetKind::Menu), file_entry("b.mnu", b->path(), AssetKind::Menu),
	                  file_entry("logo.png", "art/logo.png", AssetKind::ImageSource)};
	editor_test::own(v.project.scan).index();
	ImportedSource logo;
	logo.source = "art/logo.png";
	logo.outputs = {".opennova/imported/0a1b/logo.pcx"};
	v.project.imports = std::make_shared<const std::vector<ImportedSource>>(
			std::vector<ImportedSource>{ logo });
	v.project.recent_projects = {"C:/mods/Gate", "C:/mods/Other"};
	v.project.retail_directory = "C:/games/Joint Operations";
	v.activity.has_build = true;
	editor_test::own(v.activity.last_build).ok = true;
	editor_test::own(v.activity.last_build).build_dir =
			"C:/mods/Gate/.opennova/build/play/0123456789abcdef";
	RequirementRow gametext;
	gametext.role = "gametext";
	gametext.name = "gametext.bin";
	gametext.required = true;
	gametext.expected_kind = AssetKind::Strings;
	gametext.state = RequirementState::Missing;
	editor_test::own(v.project.requirements).rows = {gametext};
	editor_test::own(v.project.requirements).required_total = 1;
	editor_test::own(v.project.requirements).required_missing = 1;
	Diagnostic lacking = editor_test::finding_of(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	lacking.subject = RequirementSubject{"gametext", "gametext.bin"};
	v.findings.diagnostics = {lacking};

	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	ui.focus("Files");
	ui.away();
	ui.drain();
	const ImGuiID bar = menu_bar_id();
	const ImGuiID files = Ui::window_id("Files");
	const ImGuiID table = item_id(files, {"files"});
	const ImGuiID combo = ImHashStr("##Combo_00");
	const ImGuiID tab = document_tab_id(a->path());
	const ImGuiID summary = item_id(Ui::window_id("Problems"), {v.project.root.c_str(), "required"});

	// Where a right click opens a file's menu: a.mnu's row and logo.png's.
	const ImGuiWindow *files_window = ImGui::FindWindowByName("Files");
	CHECK(files_window != nullptr, "the Files window");
	if (!files_window) return;
	const float x = files_window->Pos.x + files_window->Size.x * 0.3f;
	const float top = files_window->Pos.y, bottom = files_window->Pos.y + files_window->Size.y;
	ImVec2 a_row, logo_row;
	CHECK(hover_find(ui, item_id(table, {"menus", "menus/a.mnu", "##row"}), x, top, bottom, a_row), "a.mnu's row");
	CHECK(hover_find(ui, item_id(table, {"art", "art/logo.png", "##row"}), x, top, bottom, logo_row), "logo.png's row");
	ui.away();

	const auto menu = [&ui](const char *name, std::initializer_list<const char *> items, EditorRequestKind raised) {
		return [&ui, name, items = std::vector<const char *>(items), raised]() {
			ui.activate(item_id(menu_bar_id(), {name}));
			int depth = 0;
			for (const char *item : items) {
				char popup[16];
				std::snprintf(popup, sizeof(popup), "##Menu_%02d", depth++);
				ui.activate(item_id(ImHashStr(popup), {item}));
			}
			return has(ui.drain(), raised);
		};
	};
	const auto pressed = [&ui](ImGuiID id, EditorRequestKind raised) {
		return [&ui, id, raised]() {
			ui.activate(id);
			return has(ui.drain(), raised);
		};
	};
	// A modal a control opens, closed again by its Cancel: whether it opened.
	const auto opens = [&ui](const char *modal) {
		const bool open = modal_open(modal);
		if (open) ui.activate(item_id(ImHashStr(modal), {"Cancel"}));
		ui.frames(2);
		ui.drain();
		return open;
	};
	// An item of a Files combo ("##import", "##new"), pressed.
	const auto listed = [&ui, files, combo](const char *list, ImGuiID item, EditorRequestKind raised) {
		return [&ui, files, list, item, raised]() {
			ui.activate(item_id(files, {list}));
			ui.activate(item);
			return has(ui.drain(), raised);
		};
	};
	// An item of a file's menu (a right click on its row), pressed.
	const auto file_menu = [&ui, table](ImVec2 row, const char *folder, const char *path, const char *item,
	                                    EditorRequestKind raised) {
		return [&ui, table, row, folder, path, item, raised]() {
			ui.mouse(row.x, row.y);
			ui.button(true, 1);
			ui.button(false, 1);
			ui.activate(popup_item(item_id(table, {folder, path, "file_menu"}), item));
			ui.away();
			return has(ui.drain(), raised);
		};
	};
	const auto running_cancellable = [](const SessionView &view) {
		return view.activity.operation.running() && view.activity.operation.cancellable;
	};

	using K = EditorRequestKind;
	std::vector<Probe> probes = {
	        {"File > New project...", K::NewProject,
	         [&] {
		         menu("File", {"New project..."}, K::NewProject)();
		         return opens("New project");
	         },
	         nullptr},
	        {"File > Open project...", K::OpenProject, menu("File", {"Open project..."}, K::PickDirectory), nullptr},
	        {"File > Open recent", K::OpenProject, menu("File", {"Open recent", "C:/mods/Other"}, K::OpenProject), nullptr},
	        {"File > Save", K::Save, menu("File", {"Save"}, K::Save), nullptr},
	        {"File > Save All", K::SaveAll, menu("File", {"Save All"}, K::SaveAll), nullptr},
	        {"File > Close file", K::CloseDocument, menu("File", {"Close file"}, K::CloseDocument), nullptr},
	        {"File > Import files...", K::PreviewImport, menu("File", {"Import files..."}, K::PickFile), nullptr},
	        {"File > Import from the game data...", K::PreviewInstallImport,
	         menu("File", {"Import from the game data..."}, K::PreviewInstallImport), nullptr},
	        {"File > Show project folder", K::RevealPath, menu("File", {"Show project folder"}, K::RevealPath), nullptr},
	        {"File > Close project", K::CloseProject, menu("File", {"Close project"}, K::CloseProject), nullptr},
	        {"File > Quit", K::Quit, menu("File", {"Quit"}, K::Quit), nullptr},
	        {"Edit > Undo", K::Undo, menu("Edit", {"Undo"}, K::Undo), nullptr},
	        {"Edit > Redo", K::Redo, menu("Edit", {"Redo"}, K::Redo), nullptr},
	        {"Build > Build", K::Build, menu("Build", {"Build"}, K::Build), nullptr},
	        {"Build > Play", K::Play, menu("Build", {"Play"}, K::Play), nullptr},
	        {"Build > Play in the game install", K::ApplyProjectSettings,
	         menu("Build", {"Play in the game install"}, K::ApplyProjectSettings), nullptr},
	        {"Build > Show build folder", K::RevealPath, menu("Build", {"Show build folder"}, K::RevealPath), nullptr},
	        {"the bar's Build", K::Build, pressed(item_id(bar, {"status", "Build"}), K::Build), nullptr},
	        {"the bar's Play", K::Play, pressed(item_id(bar, {"status", "Play"}), K::Play), nullptr},
	        {"the bar's Cancel", K::CancelOperation, pressed(item_id(bar, {"status", "Cancel"}), K::CancelOperation),
	         running_cancellable},
	        {"the unsaved list's file", K::OpenDocument,
	         [&] {
		         ui.activate(item_id(bar, {"status", "##unsaved"}));
		         ui.activate(popup_item(item_id(bar, {"status", "unsaved"}), b->path().c_str()));
		         return has(ui.drain(), K::OpenDocument);
	         },
	         nullptr},
	        {"the unsaved list's Save all", K::SaveAll,
	         [&] {
		         ui.activate(item_id(bar, {"status", "##unsaved"}));
		         ui.activate(popup_item(item_id(bar, {"status", "unsaved"}), "Save all"));
		         return has(ui.drain(), K::SaveAll);
	         },
	         nullptr},
	        {"the toolbar's Reload", K::ReloadDocument, pressed(item_id(tab, {"Reload"}), K::ReloadDocument), nullptr},
	        {"the toolbar's Undo", K::Undo, pressed(item_id(tab, {"Undo"}), K::Undo), nullptr},
	        {"the toolbar's Redo", K::Redo, pressed(item_id(tab, {"Redo"}), K::Redo), nullptr},
	        {"Files' Refresh", K::Rescan, pressed(item_id(files, {"Refresh"}), K::Rescan), nullptr},
	        {"Files' Import > Files...", K::PreviewImport, listed("##import", item_id(combo, {"Files..."}), K::PickFile),
	         nullptr},
	        {"Files' Import > From the game data...", K::PreviewInstallImport,
	         listed("##import", item_id(combo, {"From the game data..."}), K::PreviewInstallImport), nullptr},
	        {"Files' Import > Reimport all", K::Reimport, listed("##import", item_id(combo, {"Reimport all"}), K::Reimport),
	         nullptr},
	        {"Files' New > weapon.def", K::CreateFile,
	         listed("##new", item_id(pushed(combo, factory_index("weapon_def", AssetKind::WeaponDefs)), {"weapon.def"}),
	                K::CreateFile),
	         nullptr},
	        {"Files' New > Menu...", K::CreateFile,
	         [&] {
		         listed("##new", item_id(pushed(combo, factory_index("", AssetKind::Menu)), {"Menu..."}), K::CreateFile)();
		         ui.frames(2);
		         return opens("New file");
	         },
	         nullptr},
	        {"a file's Open", K::OpenDocument, file_menu(a_row, "menus", "menus/a.mnu", "Open", K::OpenDocument), nullptr},
	        {"a file's Rename...", K::RenameAsset,
	         [&] {
		         file_menu(a_row, "menus", "menus/a.mnu", "Rename...", K::RenameAsset)();
		         ui.frames(2);
		         return logged_frame(ui).find("Rename a.mnu to") != std::string::npos;
	         },
	         nullptr},
	        {"a file's Show in folder", K::RevealPath,
	         file_menu(a_row, "menus", "menus/a.mnu", "Show in folder", K::RevealPath), nullptr},
	        {"logo.png's Import again", K::Reimport, file_menu(logo_row, "art", "art/logo.png", "Import again", K::Reimport),
	         nullptr},
	        {"Problems' Create", K::CreateMissing,
	         [&] {
		         ui.activate(item_id(pushed(summary, static_cast<int>(K::CreateMissing)), {"###fix"}));
		         ui.frames(2);
		         return opens("Apply fixes");
	         },
	         nullptr},
	};
	std::vector<OperationStatus> statuses = {OperationStatus()};
	for (size_t k = 0; k < kOperationKindCount; ++k)
		for (const bool cancellable : {true, false}) statuses.push_back(status_of(static_cast<OperationKind>(k), cancellable));
	for (const OperationStatus &status : statuses) check_probes(ui, v, status, probes);

	// The confirmation is weighed again while it is open: an operation started since holds its
	// Apply back, and once it is gone Apply raises the fixes.
	v.activity.operation = OperationStatus();
	v.revisions.touch(ViewConcern::Operation);
	ui.frames(2);
	ui.activate(item_id(pushed(summary, static_cast<int>(K::CreateMissing)), {"###fix"}));
	ui.frames(2);
	CHECK(modal_open("Apply fixes"), "the confirmation asks");
	v.activity.operation = status_of(OperationKind::Build, true);
	v.revisions.touch(ViewConcern::Operation);
	ui.frames(2);
	ui.activate(item_id(ImHashStr("Apply fixes"), {"Apply"}));
	CHECK(!has(ui.drain(), K::CreateMissing) && modal_open("Apply fixes"), "Apply held back while a build packs");
	v.activity.operation = OperationStatus();
	v.revisions.touch(ViewConcern::Operation);
	ui.frames(2);
	ui.activate(item_id(ImHashStr("Apply fixes"), {"Apply"}));
	CHECK(has(ui.drain(), K::CreateMissing), "Apply once the build is done");
	ui.frames(2);

	// The import dialog (a modal: its own pass), its Import, its check box and its Cancel. An
	// Import or a Cancel closes it; the view still previewing, it opens again a frame later.
	const ImGuiID dialog = ImHashStr("Import files");
	const auto in_dialog = [&ui](ImGuiID id, EditorRequestKind raised) {
		return [&ui, id, raised]() {
			ui.frames(3);
			ui.activate(id);
			return has(ui.drain(), raised);
		};
	};
	const std::vector<Probe> import_probes = {
	        {"the import dialog's Import", K::ImportFiles, in_dialog(item_id(dialog, {"###import"}), K::ImportFiles), nullptr},
	        {"the import dialog's check box", K::SetImportDependencies,
	         in_dialog(item_id(import_body_id(), {"###needs"}), K::SetImportDependencies), nullptr},
	        {"the import dialog's Cancel", K::CancelImport, in_dialog(item_id(dialog, {"Cancel"}), K::CancelImport), nullptr},
	};
	v.dialogs.import_preview = planned_import("C:/assets");
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	CHECK(modal_open("Import files"), "the import dialog opens");
	for (const OperationStatus &status : statuses) {
		check_probes(ui, v, status, import_probes);
		ui.frames(3); // an Import or a Cancel closed it: the view still previews, so it opens again
	}
	v.dialogs.import_preview = DialogsView::ImportPreview();
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	CHECK(!modal_open("Import files"), "the import dialog closes");

	// Rename everywhere (a modal), asked on the screen's name: its Rename.
	v.dialogs.rename_preview.serial = 1;
	v.dialogs.rename_preview.symbol = true;
	v.dialogs.rename_preview.kind = ReferenceKind::MenuScreen;
	v.dialogs.rename_preview.path = a->path();
	v.dialogs.rename_preview.locator = "OPTIONS";
	v.dialogs.rename_preview.field = "name";
	v.dialogs.rename_preview.old_name = "OPTIONS";
	v.dialogs.rename_preview.new_name = v.dialogs.rename_preview.requested = "SETTINGS";
	RenameSite site;
	site.file = a->path();
	site.field = "name";
	site.before = "OPTIONS";
	site.after = "SETTINGS";
	v.dialogs.rename_preview.sites =
			std::make_shared<const std::vector<RenameSite>>(std::vector<RenameSite>{ site });
	const ImGuiID rename = ImHashStr("Rename everywhere");
	for (const OperationStatus &status : statuses) {
		post_event(v, ViewEventKind::AskRename, a->path(), NodeAddress(), "name", false,
				v.dialogs.rename_preview.serial);
		ui.frames(3);
		CHECK(modal_open("Rename everywhere"), "Rename everywhere opens");
		check_probes(ui, v, status,
		             {{"Rename everywhere's Rename", K::RenameSymbol, pressed(item_id(rename, {"Rename"}), K::RenameSymbol),
		               nullptr}});
		if (modal_open("Rename everywhere")) ui.activate(item_id(rename, {"Cancel"}));
		ui.frames(2);
	}
	v.dialogs.rename_preview = DialogsView::RenamePreview();

	// The game running: Stop, from the menu and the bar.
	v.activity.play_state = PlayState::Running;
	v.revisions.touch(ViewConcern::Run);
	const std::vector<Probe> stop_probes = {
	        {"Build > Stop", K::StopPlay, menu("Build", {"Stop"}, K::StopPlay), nullptr},
	        {"the bar's Stop", K::StopPlay, pressed(item_id(bar, {"status", "Stop"}), K::StopPlay), nullptr},
	};
	for (const OperationStatus &status : statuses) check_probes(ui, v, status, stop_probes);
	v.activity.play_state = PlayState::Stopped;
	v.revisions.touch(ViewConcern::Run);

	// No project open: the welcome view's Create project (a folder picked), its Browse... (a new
	// project's), Open a project folder..., a recent project and its Forget.
	v.project.open = false;
	v.documents.open.clear();
	v.documents.active.clear();
	for (size_t concern = 0; concern < kViewConcernCount; ++concern) v.revisions.touch(static_cast<ViewConcern>(concern));
	ui.frames(3);
	ui.windows.deliver_pick(PickPurpose::NewProjectLocation, "C:/mods/New");
	ui.frames(2);
	const ImGuiID document = Ui::window_id("Document");
	const std::vector<Probe> welcome_probes = {
	        {"the welcome view's Create project", K::NewProject, pressed(item_id(document, {"Create project"}), K::NewProject),
	         nullptr},
	        {"the welcome view's Browse...", K::NewProject, pressed(item_id(document, {"Browse...##folder"}), K::PickDirectory),
	         nullptr},
	        {"the welcome view's Open a project folder...", K::OpenProject,
	         pressed(item_id(document, {"Open a project folder..."}), K::PickDirectory), nullptr},
	        {"a recent project", K::OpenProject,
	         pressed(item_id(document, {"recent", "C:/mods/Other", "###root"}), K::OpenProject), nullptr},
	        {"a recent project's Forget", K::ForgetRecent,
	         pressed(item_id(document, {"recent", "C:/mods/Other", "Forget"}), K::ForgetRecent), nullptr},
	};
	for (const OperationStatus &status : statuses) check_probes(ui, v, status, welcome_probes);
	v.activity.operation = OperationStatus();
	ui.away();
}

void run_gate_tests() {
	test_windows_show_the_gate();
}

} // namespace editor_ui_test
