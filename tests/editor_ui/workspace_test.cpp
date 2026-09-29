// The OpenNova Editor's workspace (ADR 0046 S11d) over a null ImGui backend: its six
// windows, where they dock and the layout the editor builds (the bottom strip under
// everything, Files and the Inspector at their widths, Document beside Preview, Document
// focused and Problems the bottom's tab), a seeded project drawing every window; the Document
// window's tabs (one per open document, the unsaved mark, a click making a document the
// active one, the tab following the active document, the close button and Ctrl+W closing
// it, the click winning when the active document changes in its frame, two files of one
// name each a tab, an inactive tab closed, a view's confirmation outliving its hidden tab
// and closing with its document, thirty tabs all reachable through the tab list); the
// welcome view and File > New project...; the project settings over a real session (one
// Apply for every setting, a write that fails keeping the dialog open on every retry, a
// partial failure retried against the settings in effect, the dialog closing with its
// project and a late Browse... dropped); the File, Edit and Build menus; the menu bar's
// right end (what the session said, the unsaved files, the counts, the build or the game,
// the buttons; never over the menus); Files (its folders, the file count, the kind hidden
// until the header's menu shows it, a filter's flat list, a click and a double click, New
// and its name prompt, Rename...); the import dialog; the OS window's title.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/model_document.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_preview_state.h>
#include <editor/project/project_document.h>
#include <editor/session/project_session.h>
#include <editor/session/session_view.h>
#include <editor/ui/editor_windows.h>
#include <editor/ui/files_window.h>
#include <editor/ui/model_preview_pane.h>
#include <editor/ui/preview_window.h>
#include "../editor/anim_test_support.h"
#include "../editor/editor_test_support.h"
#include "common/file_io.h"
#include "common/test_paths.h"
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

bool near(float a, float b, float slack = 4.0f) { return std::fabs(a - b) <= slack; }

// The requests the workspace raised, served by a real session as the shell's pump serves
// them (a picker, the shell's own, is the test's to answer); two frames drawn after.
std::vector<EditorRequest> serve(Ui &ui, ProjectSession &session) {
	std::vector<EditorRequest> requests = ui.drain();
	for (const EditorRequest &request : requests) session.handle(request);
	ui.frames(2);
	return requests;
}

// Text typed into a field: activated for input, its text selected and deleted, the text
// typed.
void type_into(Ui &ui, ImGuiID field, const char *text) {
	ImGui::ActivateItemByID(field);
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	ui.key(ImGuiMod_Ctrl, true);
	ui.key(ImGuiKey_A, true);
	ui.key(ImGuiKey_A, false);
	ui.key(ImGuiMod_Ctrl, false);
	ui.key(ImGuiKey_Backspace, true);
	ui.key(ImGuiKey_Backspace, false);
	ImGui::GetIO().AddInputCharactersUTF8(text);
	ui.frames(2);
	ImGui::ClearActiveID();
	ui.frames(2);
}

bool modal_open(const char *title) {
	const ImGuiWindow *window = ImGui::FindWindowByName(title);
	return window && window->Active;
}

ImGuiTabBar *document_tabs() { return GImGui->TabBars.GetByKey(item_id(Ui::window_id("Document"), {"documents"})); }

// Where a tab of the bar is: its row, from its offset and width.
ImRect tab_rect(const ImGuiTabBar &bar, const std::string &path) {
	const ImGuiTabItem *tab = ImGui::TabBarFindTabByID(const_cast<ImGuiTabBar *>(&bar), document_tab_id(path));
	if (!tab) return ImRect();
	const float x = bar.BarRect.Min.x + IM_TRUNC(tab->Offset - bar.ScrollingAnim);
	return ImRect(ImVec2(x, bar.BarRect.Min.y), ImVec2(x + tab->Width, bar.BarRect.Max.y));
}

// A tab's close button: at its right end, a font size square inside the frame padding.
ImVec2 close_button(const ImGuiTabBar &bar, const std::string &path) {
	const ImRect rect = tab_rect(bar, path);
	const ImGuiStyle &style = ImGui::GetStyle();
	const float size = ImGui::GetFontSize();
	return ImVec2(rect.Max.x - style.FramePadding.x - size * 0.5f, rect.Min.y + style.FramePadding.y + size * 0.5f);
}

// The point in [top, bottom] along `x` where the mouse hovers the item `id` (a row of a
// list), found moving the mouse down; false when none does.
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

// The factory at its index in the blank factories' table, the index the New menu's entry
// is pushed with.
int factory_index(const char *role, AssetKind kind) {
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory *factory = blank_factory_at(i);
		if (std::string(factory->role) == role && factory->kind == kind) return static_cast<int>(i);
	}
	return -1;
}

std::shared_ptr<MnuDocument> edited(std::shared_ptr<MnuDocument> document) {
	Edit rename;
	rename.address = named(*document, "TITLE");
	rename.field = "name";
	rename.value = std::string("HEADING");
	Diagnostic error;
	CHECK(document->apply(rename, error) && document->dirty(), "a menu with unsaved changes");
	return document;
}

SessionView seeded_view() {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/My Game";
	v.document.title = "My Game";
	v.scan.entries.push_back(file_entry("main.mnu", "menus/main.mnu", AssetKind::Menu));
	v.scan.index();
	RequirementRow row;
	row.role = "main_menu";
	row.name = "main.mnu";
	row.required = true;
	row.state = RequirementState::Present;
	v.requirements.rows.push_back(row);
	RequirementRow missing;
	missing.role = "gametext";
	missing.name = "gametext.bin";
	missing.required = true;
	missing.expected_kind = AssetKind::Strings;
	missing.state = RequirementState::Missing;
	v.requirements.rows.push_back(missing);
	v.requirements.required_total = 2;
	v.requirements.required_missing = 1;
	Diagnostic lacking = make_diagnostic(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	lacking.role = missing.role;
	lacking.target = missing.name;
	v.diagnostics.push_back(lacking);
	v.output = {"Opened My Game", "Build started."};
	v.recent_projects = {"C:/mods/My Game", "C:/mods/Other"};
	v.status = "Opened My Game.";
	return v;
}

// The windows, where each docks and lists; the layout the editor builds at 1280x720 (the
// bottom strip first, across the whole width, a quarter of the height; Files 260 pixels on
// the left and the Inspector 320 on the right above it; the centre 37.5% Document, 62.5%
// Preview), Document focused and Problems the bottom's tab. Unattached nothing draws; a
// seeded project draws every window (a catalog's tab among them) and raises nothing, the
// import dialog too.
void test_workspace_layout() {
	NullBackend backend;
	EditorWindows windows;
	const devtools::ImGuiPass &pass = windows.pass();
	CHECK(pass.window_count() == 6, "six windows: Files, Document, Preview, Inspector, Problems, Output");
	using Placement = devtools::InitialDockPlacement;
	const std::pair<const char *, Placement> placed[] = {
	        {"Files", Placement::Left},      {"Document", Placement::Center}, {"Preview", Placement::CenterRight},
	        {"Inspector", Placement::Right}, {"Problems", Placement::Bottom}, {"Output", Placement::Bottom}};
	for (const auto &[title, placement] : placed) {
		const devtools::Window *window = find_window(pass, title);
		CHECK(window && window->initial_dock_placement() == placement, title);
		CHECK(window && window->menu_group() == devtools::MenuGroup::Workspace, title);
	}
	for (const char *gone : {"Project", "Project files", "Catalog", "Strings", "Styles", "Menu", "Models", "Animations",
	                         "Menu preview", "Model preview"})
		CHECK(find_window(pass, gone) == nullptr, gone);
	CHECK(!find_window(pass, "Document")->is_closeable(), "Document is the workspace's home: never closed");
	CHECK(pass.dock_layout().dockspace == "OpenNovaEditorWorkspace.v2" && pass.dock_layout().bottom_full_width,
	      "the editor's own dockspace: a layout saved before these windows is not found");
	CHECK(pass.is_open(), "the workspace is open from construction");

	CHECK(!frame(windows, 1), "no draw before attach");
	CHECK(windows.pass().attach_imgui(backend.context, test_alloc, test_free, nullptr), "attach");
	for (uint64_t i = 2; i < 8; ++i) frame(windows, i);
	CHECK(ImGui::GetDrawData() != nullptr && ImGui::GetDrawData()->TotalVtxCount > 0, "the home layout draws");
	ImGuiWindow *files = ImGui::FindWindowByName("Files");
	ImGuiWindow *document = ImGui::FindWindowByName("Document");
	ImGuiWindow *preview = ImGui::FindWindowByName("Preview");
	ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
	ImGuiWindow *problems = ImGui::FindWindowByName("Problems");
	ImGuiWindow *output = ImGui::FindWindowByName("Output");
	CHECK(files && document && preview && inspector && problems && output, "every window drew");
	if (!files || !document || !preview || !inspector || !problems || !output) return;
	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	const ImVec2 at = viewport->WorkPos, size = viewport->WorkSize;
	// (The layout is built on the first frame, before the menu bar takes its line: a quarter
	// of the whole height.)
	CHECK(near(problems->Pos.x, at.x) && near(problems->Size.x, size.x) &&
	              near(problems->Size.y, (at.y + size.y - viewport->Pos.y) * 0.25f),
	      "Problems along the whole bottom, a quarter of the height");
	CHECK(output->DockId == problems->DockId, "Output beside it");
	CHECK(near(files->Pos.x, at.x) && near(files->Pos.y, at.y) && near(files->Size.x, 260.0f) &&
	              files->Pos.y + files->Size.y <= problems->Pos.y,
	      "Files: 260 pixels on the left, above the strip");
	CHECK(near(inspector->Pos.x + inspector->Size.x, at.x + size.x) && near(inspector->Size.x, 320.0f) &&
	              inspector->Pos.y + inspector->Size.y <= problems->Pos.y,
	      "the Inspector: 320 pixels on the right, above the strip");
	const float centre = inspector->Pos.x - (files->Pos.x + files->Size.x);
	CHECK(near(document->Pos.x, files->Pos.x + files->Size.x) && near(document->Size.x, centre * 0.375f, 6.0f),
	      "Document: 37.5% of the centre");
	CHECK(preview->Pos.x >= document->Pos.x + document->Size.x && near(preview->Size.x, centre * 0.625f, 6.0f),
	      "Preview beside it, the rest of the centre");
	CHECK(GImGui->NavWindow == document, "Document has the focus");
	CHECK(problems->DockNode && problems->DockNode->TabBar && problems->DockNode->TabBar->SelectedTabId == problems->TabId,
	      "Problems is the bottom's tab");
	const std::string home = logged_frame(windows, 8);
	CHECK(home.find("New project") != std::string::npos, "no project: the welcome view");
	CHECK(home.find("Open a menu, a model or an animation to preview it.") != std::string::npos, "nothing to preview yet");

	// A seeded project with a catalog open: a missing model reference and a line the game
	// ignores draw the inspector's Missing badge and the catalog's dropped-lines notice.
	SessionView v = seeded_view();
	editor_test::TempProjectDir dir("opennova_catalog_ui_test");
	CHECK(editor_test::write_text(dir.file("items.def"), "begin \"Marker\"\nid 100001\ntype marker\ngraphic nowhere\nsubtype Ruins\nend\n"),
	      "catalog fixture");
	auto catalog = std::make_shared<DefCatalogDocument>();
	Diagnostic error;
	CHECK(catalog->load(dir.file("items.def"), "items.def", AssetKind::ItemDefs, "jo", error), "catalog loads");
	CHECK(!catalog->blocked() && catalog->ignored_lines() == 1, "an ignored line does not block");
	v.scan.entries.push_back(file_entry("items.def", "items.def", AssetKind::ItemDefs));
	v.scan.index();
	v.documents.push_back(catalog);
	v.active_document = catalog->path();
	v.selection = {catalog->rows()[0]->id, node_kind(opennova::def::DefRecordKind::Item), 0};
	windows.set_view(&v);
	for (uint64_t i = 9; i < 15; ++i) frame(windows, i);
	CHECK(ImGui::GetDrawData()->TotalVtxCount > 0, "the project layout draws");
	const std::string text = logged_frame(windows, 15);
	CHECK(in_order(text, {"items.def", "Reload", "line(s) the game ignores", "Marker"}), "the catalog's tab: its toolbar, notice and records");
	CHECK(windows.pending_requests() == 0, "drawing raises no request by itself");
	v.import_preview = planned_import("C:/assets");
	v.revisions.touch(ViewConcern::Dialogs);
	for (uint64_t i = 16; i < 19; ++i) frame(windows, i);
	CHECK(modal_open("Import files"), "the import dialog draws from the workspace");
	CHECK(windows.pending_requests() == 0, "previewing an import writes nothing");
	windows.pass().detach_imgui();
}

// The Document window's tabs over three open menus, b.mnu with unsaved changes: a tab each,
// the unsaved one marked; a click on a tab makes its document the active one (once); the
// tab follows the active document when it changes elsewhere, raising nothing; the close
// button and Ctrl+W close a document, its tab staying until the session closes it.
void test_document_tabs() {
	editor_test::TempProjectDir dir("opennova_editor_ui_tabs_test");
	const auto a = menu_at(dir, "a.mnu", "menus/a.mnu");
	const auto b = edited(menu_at(dir, "b.mnu", "menus/b.mnu"));
	const auto c = menu_at(dir, "c.mnu", "menus/c.mnu");
	SessionView v = menu_view(a);
	v.scan.entries = {file_entry("a.mnu", a->path(), AssetKind::Menu), file_entry("b.mnu", b->path(), AssetKind::Menu),
	                  file_entry("c.mnu", c->path(), AssetKind::Menu)};
	v.scan.index();
	v.documents = {a, b, c};
	v.active_document = a->path();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	CHECK(ui.windows.pending_requests() == 0, "drawing the tabs raises nothing");
	ImGuiTabBar *bar = document_tabs();
	CHECK(bar && bar->Tabs.Size == 3, "a tab per open document");
	if (!bar) return;
	const auto tab = [&](const std::shared_ptr<MnuDocument> &d) { return ImGui::TabBarFindTabByID(bar, document_tab_id(d->path())); };
	CHECK(tab(a) && tab(b) && tab(c), "each known by its document's path");
	CHECK(tab(b) && (tab(b)->Flags & ImGuiTabItemFlags_UnsavedDocument) && tab(a) &&
	              !(tab(a)->Flags & ImGuiTabItemFlags_UnsavedDocument),
	      "the file with unsaved changes has its tab marked");
	CHECK(bar->SelectedTabId == document_tab_id(a->path()), "the active document's tab is the one shown");
	CHECK(in_order(logged_frame(ui), {"Reload", "OPTIONS"}), "a.mnu's view: its toolbar, its screens");

	// A click on c.mnu's tab: its document becomes the active one, one request.
	ui.click(tab_rect(*bar, c->path()).GetCenter());
	ui.frames(2);
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == c->path(), "a click on a tab opens its document");
	CHECK(logged_frame(ui).find("OPTIONS") == std::string::npos, "its view waits until it is the active document");
	v.active_document = c->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(ui.drain().empty() && bar->SelectedTabId == document_tab_id(c->path()), "made active, its tab stays, nothing more");
	// The active document changed elsewhere (a Problems row, the editor MCP): its tab is
	// selected, and no request goes back.
	v.active_document = b->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(ui.drain().empty() && bar->SelectedTabId == document_tab_id(b->path()), "the tab follows the active document");

	// b.mnu's close button: CloseDocument for it (the session asks first: it has unsaved
	// changes); the tab stays while the document is open.
	ui.click(close_button(*bar, b->path()));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CloseDocument) && requests[0].path == b->path(), "the close button closes its document");
	ui.away();
	ui.frames(2);
	CHECK(tab(b) != nullptr && bar->Tabs.Size == 3, "a close not made (yet, or cancelled) leaves its tab");
	requests = ui.chord({ImGuiMod_Ctrl, ImGuiKey_W});
	CHECK(one(requests, EditorRequestKind::CloseDocument) && requests[0].path == b->path(), "Ctrl+W closes the active document");
	// Closed by the session: its tab goes, the new active document's shows.
	v.documents = {a, c};
	v.active_document = c->path();
	v.revisions.touch(ViewConcern::Documents);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(bar->Tabs.Size == 2 && !tab(b) && bar->SelectedTabId == document_tab_id(c->path()), "the tab went with its document");
	CHECK(ui.drain().empty(), "nothing else");
}

// The user's choice of tab wins: the active document changes elsewhere (b, the editor MCP)
// on the very frame the mouse goes down on a later tab (c), and c becomes the active
// document; the clicks after it are not held back. Two files of one name in different
// folders get a tab each, labelled by the path, both reachable. An inactive tab's close
// button closes its document without showing it.
void test_document_tab_choices() {
	editor_test::TempProjectDir dir("opennova_editor_ui_tab_choices");
	const auto a = menu_at(dir, "a.mnu", "menus/a.mnu");
	const auto b = menu_at(dir, "b.mnu", "menus/b.mnu");
	const auto c = menu_at(dir, "c.mnu", "menus/c.mnu");
	SessionView v = menu_view(a);
	v.documents = {a, b, c};
	v.active_document = a->path();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	ImGuiTabBar *bar = document_tabs();
	CHECK(bar && bar->Tabs.Size == 3, "a tab each");
	if (!bar) return;
	const ImVec2 at_c = tab_rect(*bar, c->path()).GetCenter();
	ui.mouse(at_c.x, at_c.y);
	v.active_document = b->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.button(true);
	ui.button(false);
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == c->path(),
	      "the click in the frame b became active: c opens");
	v.active_document = c->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(ui.drain().empty() && bar->SelectedTabId == document_tab_id(c->path()), "c shown and active, nothing more");
	ui.away();
	ui.click(tab_rect(*bar, a->path()).GetCenter());
	ui.frames(2);
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == a->path(), "the next click opens a");
	v.active_document = a->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	ui.drain();

	// Two x.mnu, in a/ and in b/: two tabs, each labelled by its path, each reachable.
	const auto x1 = menu_at(dir, "x1.mnu", "a/x.mnu");
	const auto x2 = menu_at(dir, "x2.mnu", "b/x.mnu");
	v.documents = {a, c, x1, x2};
	v.active_document = x1->path();
	v.revisions.touch(ViewConcern::Documents);
	v.revisions.touch(ViewConcern::Selection);
	ui.away();
	ui.frames(3);
	ImGuiTabItem *first = ImGui::TabBarFindTabByID(bar, document_tab_id(x1->path()));
	ImGuiTabItem *second = ImGui::TabBarFindTabByID(bar, document_tab_id(x2->path()));
	ImGuiTabItem *plain = ImGui::TabBarFindTabByID(bar, document_tab_id(a->path()));
	CHECK(bar->Tabs.Size == 4 && first && second && plain && first != second, "a tab each");
	if (!first || !second || !plain) return;
	CHECK(std::string(ImGui::TabBarGetTabName(bar, first)).rfind("a/x.mnu###", 0) == 0 &&
	              std::string(ImGui::TabBarGetTabName(bar, second)).rfind("b/x.mnu###", 0) == 0 &&
	              std::string(ImGui::TabBarGetTabName(bar, plain)).rfind("a.mnu###", 0) == 0,
	      "labelled by the path where the name is shared, by the name elsewhere");
	CHECK(ui.drain().empty() && bar->SelectedTabId == document_tab_id(x1->path()), "a/x.mnu shown");
	ui.click(tab_rect(*bar, x2->path()).GetCenter());
	ui.frames(2);
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == x2->path(), "b/x.mnu reachable");
	v.active_document = x2->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	ui.away();
	ui.click(tab_rect(*bar, x1->path()).GetCenter());
	ui.frames(2);
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == x1->path(), "and a/x.mnu");
	v.active_document = x1->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	ui.away();
	ui.drain();

	// c's close button, c inactive (and saved): CloseDocument for it; x1 stays shown.
	ui.click(close_button(*bar, c->path()));
	ui.frames(2);
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CloseDocument) && requests[0].path == c->path(), "an inactive tab closes");
	CHECK(bar->SelectedTabId == document_tab_id(x1->path()), "without being shown");
	v.documents = {a, x1, x2};
	v.revisions.touch(ViewConcern::Documents);
	ui.away();
	ui.frames(3);
	CHECK(ui.drain().empty() && !ImGui::TabBarFindTabByID(bar, document_tab_id(c->path())) &&
	              bar->SelectedTabId == document_tab_id(x1->path()),
	      "closed by the session: its tab goes, the shown one stays");
}

// A view's own confirmation (a menu's Remove screen...) is drawn by the workspace: when the
// active document changes elsewhere while it asks, its tab hidden, it still shows (a modal
// no frame draws would hold the input), and its Remove goes to the menu it asked about. It
// is that document's, the instance: the menu read again (a new instance at its path) or
// closed while it asks, it closes, removing nothing.
void test_view_prompt_outlives_its_tab() {
	editor_test::TempProjectDir dir("opennova_editor_ui_view_prompt");
	constexpr NodeKind kScreen = node_kind(MenuKind::Screen);
	// A menu with two screens: a screen goes only while another is left.
	const auto two_screens = [&dir, kScreen](const char *file, const char *relative) {
		const auto menu = menu_at(dir, file, relative);
		Edit copy;
		copy.operation = EditOperation::Duplicate;
		copy.address = {menu->rows()[0]->id, kScreen, 0};
		copy.position = 1;
		Diagnostic error;
		CHECK(menu->apply(copy, error) && menu->rows().size() == 2, "a menu with two screens");
		return menu;
	};
	const auto a = two_screens("a.mnu", "menus/a.mnu");
	const auto b = menu_at(dir, "b.mnu", "menus/b.mnu");
	if (a->rows().size() != 2) return;
	const NodeId second = a->rows()[1]->id;
	SessionView v = menu_view(a);
	v.documents = {a, b};
	select_in(v, {second, kScreen, 0});
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	// Remove screen... in the active menu's tab: the prompt open, nothing raised yet.
	const auto ask = [&ui](const std::shared_ptr<MnuDocument> &menu) {
		ui.activate(item_id(document_tab_id(menu->path()), {"Remove screen..."}));
		ui.frames(2);
		return modal_open("Remove screen?") && ui.drain().empty();
	};
	CHECK(ask(a), "Remove screen... asks first");
	v.active_document = b->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(document_tabs() && document_tabs()->SelectedTabId == document_tab_id(b->path()) && modal_open("Remove screen?"),
	      "its tab hidden, it still asks");
	ui.activate(item_id(ImHashStr("Remove screen?"), {"Remove"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::EditRecord) && requests[0].path == a->path() &&
	              requests[0].edit.operation == EditOperation::Remove && requests[0].edit.address.row == second,
	      "and removes the screen of the menu it asked about");

	// Asked again; a.mnu read again meanwhile (a new instance, two screens again): the prompt
	// closes, removing nothing.
	v.active_document = a->path();
	select_in(v, {second, kScreen, 0});
	ui.frames(3);
	CHECK(ask(a), "asked again");
	const auto reread = two_screens("a.mnu", "menus/a.mnu");
	v.documents = {reread, b};
	select_in(v, {reread->rows()[1]->id, kScreen, 0});
	ui.frames(3);
	CHECK(!modal_open("Remove screen?") && ui.drain().empty(), "its menu read again: the prompt closes");
	// Asked about the new one, which then closes: likewise.
	CHECK(ask(reread), "asked about the menu read again");
	v.documents = {b};
	v.active_document = b->path();
	v.select_only({});
	v.revisions.touch(ViewConcern::Documents);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(!modal_open("Remove screen?") && ui.drain().empty(), "its menu closed: the prompt closes");
}

// Thirty open documents: more tabs than the bar shows, the tab list lists every one, and
// the last, chosen from it, becomes the active document.
void test_thirty_tabs() {
	editor_test::TempProjectDir dir("opennova_editor_ui_thirty_tabs");
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Thirty";
	v.document.title = "Thirty";
	std::vector<std::shared_ptr<MnuDocument>> menus;
	for (int i = 0; i < 30; ++i) {
		char file[16];
		std::snprintf(file, sizeof(file), "m%02d.mnu", i);
		menus.push_back(menu_at(dir, file, (std::string("menus/") + file).c_str()));
		v.scan.entries.push_back(file_entry(file, menus.back()->path(), AssetKind::Menu));
		v.documents.push_back(menus.back());
	}
	v.scan.index();
	v.active_document = menus.front()->path();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	ImGuiTabBar *bar = document_tabs();
	CHECK(bar && bar->Tabs.Size == 30 && bar->WidthAllTabs > bar->BarRect.GetWidth(), "thirty tabs, more than the bar shows");
	if (!bar) return;
	ui.activate(item_id(bar->ID, {"##v"}));
	const std::string text = logged_frame(ui);
	bool every = true;
	for (int i = 0; i < 30; ++i) {
		char file[16];
		std::snprintf(file, sizeof(file), "m%02d.mnu", i);
		every = every && text.find(file) != std::string::npos;
	}
	CHECK(every, "the tab list lists all thirty");
	ui.activate(ImHashStr(("###" + menus.back()->path()).c_str(), 0, ImHashStr("##Combo_00")));
	ui.frames(2);
	const std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == menus.back()->path(),
	      "the last tab, chosen from the list, opens");
}

// No project: the Document window is the welcome view (new, open, recent, what happened).
// Create waits for a folder, the shell's pick fills it, Create raises NewProject; File > New
// project... shows the same form in a modal; File > Open recent opens one.
void test_welcome_view() {
	SessionView v;
	v.recent_projects = {"C:/mods/Armory", "C:/mods/Other"};
	v.status = "No project open.";
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	CHECK(in_order(logged_frame(ui), {"New project", "Create project", "Open project", "Open a project folder...", "Recent",
	                                  "C:/mods/Armory", "C:/mods/Other", "No project open."}),
	      "the welcome view");
	const ImGuiID document = Ui::window_id("Document");
	ui.activate(item_id(document, {"Create project"}));
	CHECK(ui.drain().empty(), "no folder yet: nothing to create");
	ui.activate(item_id(document, {"Browse...##folder"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PickDirectory) && requests[0].purpose == PickPurpose::NewProjectLocation,
	      "Browse... asks the shell for a folder");
	ui.windows.deliver_pick(PickPurpose::NewProjectLocation, "C:/mods/New");
	ui.frames(2);
	ui.activate(item_id(document, {"Create project"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::NewProject) && requests[0].path == "C:/mods/New" && requests[0].text == "My Game",
	      "Create: the folder picked, the name typed");
	ui.activate(item_id(document, {"Open a project folder..."}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PickDirectory) && requests[0].purpose == PickPurpose::OpenProject, "Open...");

	CHECK(choose(ui, "File", {"New project..."}).empty() && modal_open("New project"), "File > New project... asks");
	CHECK(logged_frame(ui).find("C:/mods/New") != std::string::npos, "with the same form");
	ui.activate(item_id(ImHashStr("New project"), {"Create project"}));
	requests = ui.drain();
	ui.frames(2);
	CHECK(one(requests, EditorRequestKind::NewProject) && requests[0].path == "C:/mods/New" && !modal_open("New project"),
	      "its Create, and the modal closes");
	requests = choose(ui, "File", {"Open recent", "C:/mods/Other"});
	CHECK(one(requests, EditorRequestKind::OpenProject) && requests[0].path == "C:/mods/Other", "File > Open recent");
}

// File > Project settings... over a real session: the fields are the dialog's until Apply,
// which raises one ApplyProjectSettings naming every setting, and the session writes what
// differs from the settings in effect. A setting it could not write keeps the dialog open
// saying why, on every Apply that fails again; one it wrote is in effect from then on, so a
// name changed back after a partial failure is written back (the dialog never compares with
// what it opened with); once none fails it closes. A Browse... fills the field it asked for.
// The dialog is its project's: another project opened while it is open closes it, raising
// nothing, and a Browse... answered after that is dropped. Cancel changes nothing; a source
// run shows the runtime it drives, no field.
void test_project_settings() {
	editor_test::TempProjectDir dir("opennova_editor_ui_settings");
	NoProcess platform;
	const std::string settings_file = dir.file("settings/editor.json");
	ProjectSession session(platform, settings_file);
	CHECK(session.handle(make_request(EditorRequestKind::NewProject, dir.file("Armory"), "Armory")), "a project");
	const SessionView &v = session.view();
	const std::string armory = v.project_root;
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	serve(ui, session);
	const ImGuiID dialog = ImHashStr("Project settings");
	CHECK(choose(ui, "File", {"Project settings..."}).empty() && modal_open("Project settings"), "the dialog opens");
	CHECK(in_order(logged_frame(ui), {"Armory", "Name", "Missions", "Multiplayer", "Game install folder", "OpenNova runtime",
	                                  "Play in the game install", "Apply", "Cancel"}),
	      "its fields");
	ui.activate(item_id(dialog, {"Apply"}));
	std::vector<EditorRequest> requests = serve(ui, session);
	const EditorRequest *apply = one(requests, EditorRequestKind::ApplyProjectSettings);
	CHECK(apply && apply->settings.title == std::optional<std::string>("Armory") &&
	              apply->settings.mission == std::optional<bool>(false) &&
	              apply->settings.multiplayer == std::optional<bool>(false) &&
	              apply->settings.retail_directory == std::optional<std::string>("") &&
	              apply->settings.runtime_executable == std::optional<std::string>("") &&
	              apply->settings.play_retail == std::optional<bool>(false),
	      "Apply: one request naming every setting as the dialog holds it");
	CHECK(!modal_open("Project settings") && v.status == "No setting changed.", "nothing changed: written nothing, closed");

	// A runtime typed; the editor's settings cannot be written (a folder stands where their
	// file is written first): the dialog stays, saying why, however often it is applied.
	std::error_code ec;
	std::filesystem::create_directories(settings_file + ".tmp", ec);
	CHECK(choose(ui, "File", {"Project settings..."}).empty() && modal_open("Project settings"), "opened again");
	type_into(ui, item_id(dialog, {"OpenNova runtime"}), "C:/tools/opennova.exe");
	CHECK(ui.drain().empty(), "a field is the dialog's until Apply");
	for (int attempt = 0; attempt < 2; ++attempt) {
		ui.activate(item_id(dialog, {"Apply"}));
		requests = serve(ui, session);
		CHECK(one(requests, EditorRequestKind::ApplyProjectSettings) && v.settings_result.serial == requests[0].settings.serial &&
		              v.settings_result.failures.size() == 1 && v.runtime_setting.empty(),
		      "the runtime is not written");
		CHECK(modal_open("Project settings") && logged_frame(ui).find("cannot create") != std::string::npos,
		      "the dialog stays open, saying why, on every Apply that fails");
	}
	// A new name too: the name is written, the runtime is not. The name changed back is
	// written back: the name in effect is Harbor now.
	type_into(ui, item_id(dialog, {"Name"}), "Harbor");
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	ProjectDocument on_disk;
	Diagnostic error;
	CHECK(v.document.title == "Harbor" && v.runtime_setting.empty() && modal_open("Project settings") &&
	              open_project(armory, on_disk, error) && on_disk.title == "Harbor",
	      "a partial failure: the name written, the runtime not, the dialog open");
	type_into(ui, item_id(dialog, {"Name"}), "Armory");
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(v.document.title == "Armory" && open_project(armory, on_disk, error) && on_disk.title == "Armory" &&
	              modal_open("Project settings"),
	      "the name changed back is written back");
	// The settings writable again: the runtime is written, and the dialog closes.
	std::filesystem::remove_all(settings_file + ".tmp", ec);
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(!modal_open("Project settings") && v.runtime_setting == "C:/tools/opennova.exe" && v.document.title == "Armory" &&
	              v.settings_result.failures.empty(),
	      "none failed: done");

	// Browse...: the shell's answer fills the field it asked for (an answer for another field
	// is dropped).
	choose(ui, "File", {"Project settings..."});
	ui.activate(item_id(dialog, {"Browse...##retail"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PickDirectory) && requests[0].purpose == PickPurpose::RetailDirectory,
	      "Browse... asks the shell for a folder");
	// The install picked is a path this platform calls absolute ("C:/..." is relative on
	// Linux), so the session keeps it as it is.
	const std::string install = dir.file("Joint Operations");
	ui.windows.deliver_pick(PickPurpose::RuntimeExecutable, "C:/elsewhere/other.exe");
	ui.windows.deliver_pick(PickPurpose::RetailDirectory, install);
	ui.frames(2);
	std::string text = logged_frame(ui);
	CHECK(text.find(install) != std::string::npos && text.find("C:/elsewhere") == std::string::npos,
	      "the folder in its field, nothing in the other");
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(!modal_open("Project settings") && v.retail_directory == install, "the game install set");

	// Open in Armory with a new name typed and a Browse... pending, then the editor MCP opens
	// another project: the dialog closes, raising nothing; the answer that comes after, in
	// the dialog opened on the other project, is dropped.
	choose(ui, "File", {"Project settings..."});
	type_into(ui, item_id(dialog, {"Name"}), "Renamed");
	ui.activate(item_id(dialog, {"Browse...##runtime"}));
	CHECK(one(ui.drain(), EditorRequestKind::PickFile) != nullptr, "a Browse... pending");
	CHECK(session.handle(make_request(EditorRequestKind::NewProject, dir.file("Harbor"), "Harbor")) && v.project_root != armory,
	      "the editor MCP opens another project");
	ui.frames(3);
	CHECK(!modal_open("Project settings") && ui.drain().empty(), "the dialog closes with its project, raising nothing");
	choose(ui, "File", {"Project settings..."});
	ui.windows.deliver_pick(PickPurpose::RuntimeExecutable, "C:/late/opennova.exe");
	ui.frames(2);
	CHECK(logged_frame(ui).find("C:/late/opennova.exe") == std::string::npos, "the answer asked for in Armory is dropped");
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(!modal_open("Project settings") && v.document.title == "Harbor" && v.runtime_setting == "C:/tools/opennova.exe" &&
	              open_project(armory, on_disk, error) && on_disk.title == "Armory",
	      "neither project renamed, the runtime as it was");

	// Cancel raises nothing; on a source run the runtime is the checkout's, not a field.
	PlayLauncher launcher;
	launcher.source_run = true;
	launcher.executable = "C:/checkout/godot.exe";
	session.set_launcher(launcher);
	choose(ui, "File", {"Project settings..."});
	ui.activate(item_id(dialog, {"Multiplayer"}));
	text = logged_frame(ui);
	CHECK(text.find("A source run") != std::string::npos && text.find("C:/checkout/godot.exe") != std::string::npos,
	      "a source run: the runtime Play drives, no field");
	ui.activate(item_id(dialog, {"Cancel"}));
	ui.frames(2);
	CHECK(!modal_open("Project settings") && ui.drain().empty() && !v.document.features.multiplayer, "Cancel changes nothing");
}

// The File, Edit and Build menus raise their requests (a Save, a Close and an Undo naming
// the active document); what cannot run now is disabled.
void test_menus() {
	editor_test::TempProjectDir dir("opennova_editor_ui_menus_test");
	const std::shared_ptr<MnuDocument> document = edited(load_menu(dir));
	SessionView v = menu_view(document);
	v.recent_projects = {"C:/mods/Menus", "C:/mods/Other"};
	v.retail_directory = "C:/games/Joint Operations";
	v.has_build = true;
	v.last_build.ok = true;
	v.last_build.build_dir = "C:/mods/Menus/.opennova/build/play/1";
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	const auto raised = [&](const char *menu, std::initializer_list<const char *> items, EditorRequestKind kind) {
		const std::vector<EditorRequest> requests = choose(ui, menu, items);
		return requests.size() == 1 && requests[0].kind == kind ? requests[0] : EditorRequest();
	};
	EditorRequest r = raised("File", {"Open project..."}, EditorRequestKind::PickDirectory);
	CHECK(r.kind == EditorRequestKind::PickDirectory && r.purpose == PickPurpose::OpenProject, "File > Open project...");
	r = raised("File", {"Save"}, EditorRequestKind::Save);
	CHECK(r.kind == EditorRequestKind::Save && r.path == document->path(), "File > Save: the active document");
	r = raised("File", {"Save All"}, EditorRequestKind::SaveAll);
	CHECK(r.kind == EditorRequestKind::SaveAll, "File > Save All");
	r = raised("File", {"Close file"}, EditorRequestKind::CloseDocument);
	CHECK(r.kind == EditorRequestKind::CloseDocument && r.path == document->path(), "File > Close file");
	r = raised("File", {"Import files..."}, EditorRequestKind::PickFile);
	CHECK(r.kind == EditorRequestKind::PickFile && r.purpose == PickPurpose::ImportFiles, "File > Import files...");
	r = raised("File", {"Import from the game data..."}, EditorRequestKind::PreviewRetailImport);
	CHECK(r.kind == EditorRequestKind::PreviewRetailImport && r.names.empty() && r.flag == v.import_dependencies,
	      "File > Import from the game data...: every file to choose from, planned as the setting says");
	r = raised("File", {"Show project folder"}, EditorRequestKind::RevealPath);
	CHECK(r.kind == EditorRequestKind::RevealPath && r.path == v.project_root, "File > Show project folder");
	r = raised("File", {"Close project"}, EditorRequestKind::CloseProject);
	CHECK(r.kind == EditorRequestKind::CloseProject, "File > Close project");
	r = raised("File", {"Quit"}, EditorRequestKind::Quit);
	CHECK(r.kind == EditorRequestKind::Quit, "File > Quit");
	r = raised("Edit", {"Undo"}, EditorRequestKind::Undo);
	CHECK(r.kind == EditorRequestKind::Undo && r.path == document->path(), "Edit > Undo");
	r = raised("Build", {"Build"}, EditorRequestKind::Build);
	CHECK(r.kind == EditorRequestKind::Build, "Build > Build");
	r = raised("Build", {"Play"}, EditorRequestKind::Play);
	CHECK(r.kind == EditorRequestKind::Play, "Build > Play");
	CHECK(choose(ui, "Build", {"Stop"}).empty(), "Build > Stop: nothing runs");
	r = raised("Build", {"Play in the game install"}, EditorRequestKind::ApplyProjectSettings);
	CHECK(r.kind == EditorRequestKind::ApplyProjectSettings && r.settings.play_retail == std::optional<bool>(true) &&
	              !r.settings.title && !r.settings.mission && !r.settings.retail_directory && !r.settings.runtime_executable,
	      "Build > Play in the game install: that setting alone");
	r = raised("Build", {"Show build folder"}, EditorRequestKind::RevealPath);
	CHECK(r.kind == EditorRequestKind::RevealPath && r.path == v.last_build.build_dir, "Build > Show build folder");
	v.play_state = PlayState::Running;
	v.revisions.touch(ViewConcern::Run);
	ui.frames(2);
	r = raised("Build", {"Stop"}, EditorRequestKind::StopPlay);
	CHECK(r.kind == EditorRequestKind::StopPlay, "Build > Stop while the game runs");
	CHECK(choose(ui, "Build", {"Play"}).empty(), "no Play while it runs");
	v.retail_directory.clear();
	v.revisions.touch(ViewConcern::Preferences);
	CHECK(choose(ui, "File", {"Import from the game data..."}).empty(), "no game install: nothing to import from");
}

// Along the main menu bar, a pixel in two, the leftmost and rightmost x at which each of
// `ids` is hovered ({FLT_MAX, -FLT_MAX} for one never hovered).
std::vector<std::pair<float, float>> hover_spans(Ui &ui, const std::vector<ImGuiID> &ids) {
	std::vector<std::pair<float, float>> spans(ids.size(), {FLT_MAX, -FLT_MAX});
	const ImGuiWindow *bar = ImGui::FindWindowByName("##MainMenuBar");
	if (!bar) return spans;
	const float y = bar->Pos.y + bar->Size.y * 0.5f;
	for (float x = bar->Pos.x + 1.0f; x < bar->Pos.x + bar->Size.x; x += 2.0f) {
		ui.mouse(x, y);
		for (size_t i = 0; i < ids.size(); ++i)
			if (GImGui->HoveredId == ids[i]) spans[i] = {std::min(spans[i].first, x), std::max(spans[i].second, x)};
	}
	return spans;
}

// The menu bar's right end: what the session last said (cut to the room left), the unsaved
// files (a click lists them: one made the active document, Save all), the error and warning
// counts (a click shows Problems), the build or the game, and Build / Play / Stop, each
// enabled when it can run; a narrow window leaves parts out from the left, what was said
// first, and at every width the right end starts after the menus and Stop ends inside the
// bar.
void test_menu_bar_status() {
	editor_test::TempProjectDir dir("opennova_editor_ui_status_test");
	const auto a = menu_at(dir, "a.mnu", "menus/a.mnu");
	const auto b = edited(menu_at(dir, "b.mnu", "menus/b.mnu"));
	const auto c = edited(menu_at(dir, "c.mnu", "menus/c.mnu"));
	SessionView v = menu_view(a);
	v.documents = {a, b, c};
	for (int i = 0; i < 7; ++i) v.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Error, "catalog.x", "An error."));
	for (int i = 0; i < 5; ++i) v.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning, "catalog.y", "A warning."));
	v.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Info, "catalog.z", "A note."));
	v.build_running = true;
	v.build_done = 3;
	v.build_total = 12;
	v.build_step = "Packing menus";
	v.status = "menus/a.mnu has no changes to save.";
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	std::string text = logged_frame(ui);
	CHECK(in_order(text, {"File", "Edit", "Build", "Windows", "menus/a.mnu has no changes to save.", "2 unsaved", "7", "5",
	                      "Building 3/12", "Build", "Play", "Stop"}),
	      "after the menus: what was said, 2 unsaved, 7 errors, 5 warnings, the build's progress, the buttons");
	// A long line is cut to the room it has, whole in its tooltip.
	v.status = "Opened A Project With A Very Long Name (C:/Users/someone/Documents/OpenNova projects/A Project With A "
	           "Very Long Name That Goes On)";
	v.revisions.touch(ViewConcern::Output);
	text = logged_frame(ui);
	CHECK(text.find("Opened A Project") != std::string::npos && text.find("That Goes On)") == std::string::npos,
	      "a long line cut");
	v.status = "menus/a.mnu has no changes to save.";
	v.revisions.touch(ViewConcern::Output);
	const ImGuiID bar = menu_bar_id();
	const ImGuiID unsaved = item_id(bar, {"status", "unsaved"});
	ui.activate(item_id(bar, {"status", "##unsaved"}));
	text = logged_frame(ui);
	CHECK(in_order(text, {b->path().c_str(), c->path().c_str(), "Save all"}), "the unsaved files listed, then Save all");
	ui.activate(popup_item(unsaved, c->path().c_str()));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == c->path(), "one of them made the active document");
	ui.activate(item_id(bar, {"status", "##unsaved"}));
	ui.activate(popup_item(unsaved, "Save all"));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::SaveAll) != nullptr, "Save all");
	ui.activate(item_id(bar, {"status", "##problems"}));
	ui.frames(3);
	CHECK(GImGui->NavWindow == ImGui::FindWindowByName("Problems"), "a click on the counts shows Problems");
	ui.activate(item_id(bar, {"status", "Build"}));
	CHECK(ui.drain().empty(), "no Build while one packs");

	v.build_running = false;
	v.play_state = PlayState::Running;
	v.play_pid = 4242;
	v.revisions.touch(ViewConcern::Operation);
	v.revisions.touch(ViewConcern::Run);
	ui.frames(2);
	CHECK(in_order(logged_frame(ui), {"2 unsaved", "Game running", "Build", "Play", "Stop"}), "the game running");
	ui.activate(item_id(bar, {"status", "Play"}));
	CHECK(ui.drain().empty(), "no Play while it runs");
	ui.activate(item_id(bar, {"status", "Stop"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::StopPlay) != nullptr, "Stop");
	v.play_state = PlayState::Stopped;
	v.has_build = true;
	v.last_build.ok = true;
	v.revisions.touch(ViewConcern::Run);
	v.revisions.touch(ViewConcern::Operation);
	ui.frames(2);
	CHECK(in_order(logged_frame(ui), {"2 unsaved", "Built", "Build", "Play", "Stop"}), "built");
	ui.activate(item_id(bar, {"status", "Build"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::Build) != nullptr, "Build");
	ui.activate(item_id(bar, {"status", "Play"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::Play) != nullptr, "Play");

	// A window too narrow for all of it: the parts on the left go first, what was said the
	// first of them; the buttons stay.
	ImGui::GetIO().DisplaySize = ImVec2(580.0f, 700.0f);
	ui.frames(3);
	text = logged_frame(ui);
	CHECK(text.find("menus/a") == std::string::npos && in_order(text, {"Windows", "2 unsaved", "Built", "Stop"}),
	      "narrower: what was said left out first");
	ImGui::GetIO().DisplaySize = ImVec2(360.0f, 700.0f);
	ui.frames(3);
	text = logged_frame(ui);
	CHECK(text.find("2 unsaved") == std::string::npos && in_order(text, {"Windows", "Stop"}), "narrow: the unsaved count left out");

	// At every width the right end starts after the last menu and Stop ends inside the bar.
	const ImGuiID bar_id = menu_bar_id();
	const std::vector<ImGuiID> menus = {item_id(bar_id, {"File"}), item_id(bar_id, {"Edit"}), item_id(bar_id, {"Build"}),
	                                    item_id(bar_id, {"Windows"})};
	const std::vector<ImGuiID> parts = {item_id(bar_id, {"status", "##unsaved"}), item_id(bar_id, {"status", "##problems"}),
	                                    item_id(bar_id, {"status", "Build"}), item_id(bar_id, {"status", "Play"}),
	                                    item_id(bar_id, {"status", "Stop"})};
	std::vector<ImGuiID> ids = menus;
	ids.insert(ids.end(), parts.begin(), parts.end());
	for (const float width : {1000.0f, 520.0f, 360.0f}) {
		ImGui::GetIO().DisplaySize = ImVec2(width, 700.0f);
		ui.frames(3);
		const std::vector<std::pair<float, float>> spans = hover_spans(ui, ids);
		float menus_end = -FLT_MAX, parts_start = FLT_MAX;
		for (size_t i = 0; i < menus.size(); ++i) menus_end = std::max(menus_end, spans[i].second);
		for (size_t i = menus.size(); i < ids.size(); ++i) parts_start = std::min(parts_start, spans[i].first);
		const std::pair<float, float> &stop = spans.back();
		char message[128];
		std::snprintf(message, sizeof(message), "at %.0f px: the menus end at %.0f, the right end starts at %.0f, Stop ends at %.0f",
		              width, menus_end, parts_start, stop.second);
		CHECK(menus_end > 0.0f && parts_start < width && menus_end < parts_start, message);
		CHECK(stop.first < stop.second && stop.second < width - 4.0f, message);
	}
	ui.away();
}

// Files: how many files the project has; the scan's folders (sorted, each over its files,
// an imported file under its source's folder) with each file's error and warning counts
// and its size, the name taking most of the width, the kind hidden until the header's menu
// shows it (and in a row's tooltip); a filter lists its matches flat; one click selects a
// file, a double click opens it when the editor edits its kind; New makes a file the game
// reads by name at once (not offered while the project has it) and asks a free-form kind's
// name in a modal checked as it is typed; a file's menu renames it.
void test_files_window() {
	editor_test::TempProjectDir dir("opennova_editor_ui_files_test");
	const auto main_menu = edited(menu_at(dir, "main.mnu", "menus/main.mnu"));
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Files";
	v.document.title = "Files";
	v.scan.entries = {file_entry("items.def", "defs/items.def", AssetKind::ItemDefs),
	                  file_entry("main.mnu", "menus/main.mnu", AssetKind::Menu),
	                  file_entry("options.mnu", "menus/sub/options.mnu", AssetKind::Menu),
	                  file_entry("logo.png", "art/logo.png", AssetKind::ImageSource),
	                  file_entry("logo.pcx", ".opennova/imported/0a1b/logo.pcx", AssetKind::Texture),
	                  file_entry("readme.txt", "readme.txt", AssetKind::Text)};
	v.scan.entries[4].imported_from = "art/logo.png";
	v.scan.entries[0].size_bytes = 3 * 1024;
	v.scan.index();
	v.documents = {main_menu};
	v.active_document = main_menu->path();
	v.diagnostics = {make_diagnostic(DiagnosticSeverity::Error, "catalog.a", "One.", "defs/items.def"),
	                 make_diagnostic(DiagnosticSeverity::Error, "catalog.b", "Two.", "defs/items.def"),
	                 make_diagnostic(DiagnosticSeverity::Warning, "catalog.c", "Three.", "defs/items.def")};
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	const auto *window = dynamic_cast<const FilesWindow *>(find_window(ui.windows.pass(), "Files"));
	CHECK(window != nullptr, "the Files window");
	if (!window) return;
	const ImGuiID files = Ui::window_id("Files");
	const ImGuiID table = item_id(files, {"files"});
	// (Files draws first; Document's tab, with its Reload, after.)
	const auto files_text = [&ui]() {
		const std::string text = logged_frame(ui);
		return text.substr(0, text.find("Reload"));
	};
	std::string text = files_text();
	// A folder's files in the scan's order, by name (logo.pcx before logo.png).
	CHECK(in_order(text, {"Refresh", "6 files", "Name", "Size", "art", "logo.pcx", "logo.png",
			"defs", "items.def", "2", "1", "3.0 KB", "menus", "sub", "options.mnu", "main.mnu",
			"readme.txt"}),
	      "the file count; the folders sorted, each over its files; an import beside its source; the counts after a name");
	CHECK(text.find("Kind") == std::string::npos && text.find("Item defin") == std::string::npos, "the kind hidden");
	ImGuiTable *files_table = ImGui::TableFindByID(table);
	CHECK(files_table && files_table->ColumnsCount == 3 && !files_table->Columns[1].IsEnabled &&
	              files_table->Columns[0].WidthGiven > 2.0f * files_table->Columns[2].WidthGiven &&
	              files_table->Columns[2].WidthGiven >= ImGui::CalcTextSize("999.9 KB").x - 1.0f,
	      "the name has most of the width; the size is as wide as 999.9 KB");
	if (!files_table) return;
	// The kind shown through the header's menu (a right click on a header).
	const ImGuiTableColumn &size_column = files_table->Columns[2];
	ui.mouse((size_column.MinX + size_column.MaxX) * 0.5f, files_table->OuterRect.Min.y + ImGui::GetFontSize() * 0.5f + 1.0f);
	ui.button(true, 1);
	ui.button(false, 1);
	ui.activate(popup_item(ImHashStr("##ContextMenu", 0, files_table->ID), "Kind"));
	ImGui::ClosePopupsExceptModals();
	ui.away();
	ui.frames(2);
	text = files_text();
	CHECK(files_table->Columns[1].IsEnabled && in_order(text, {"Name", "Kind", "Size", "items.def", "Item defin", "3.0 KB"}),
	      "the kind shown from the header's menu");

	// A filter: its matches, flat.
	type_into(ui, item_id(files, {"##filter"}), "mnu");
	text = files_text();
	CHECK(in_order(text, {"main.mnu", "options.mnu"}) && text.find("items.def") == std::string::npos &&
	              text.find("readme.txt") == std::string::npos && text.find("menus") == std::string::npos,
	      "a filter lists its matches flat");
	type_into(ui, item_id(files, {"##filter"}), "zzz");
	CHECK(logged_frame(ui).find("No file matches the filter.") != std::string::npos, "no match");
	type_into(ui, item_id(files, {"##filter"}), "");
	ui.drain();

	// One click selects a file; a double click opens one the editor edits. A row's tooltip
	// says its path, its kind and its size.
	const ImGuiWindow *files_window = ImGui::FindWindowByName("Files");
	const float x = files_window->Pos.x + files_window->Size.x * 0.3f;
	const float top = files_window->Pos.y, bottom = files_window->Pos.y + files_window->Size.y;
	ImVec2 row;
	CHECK(hover_find(ui, item_id(table, {"defs", "defs/items.def", "##row"}), x, top, bottom, row) &&
	              logged_frame(ui).find("Item definitions, 3.0 KB") != std::string::npos,
	      "items.def's tooltip: its kind and size");
	CHECK(hover_find(ui, item_id(table, {"menus", "menus/main.mnu", "##row"}), x, top, bottom, row), "main.mnu's row");
	ui.button(true);
	ui.button(false);
	CHECK(window->selected() == "menus/main.mnu" && ui.drain().empty(), "a click selects it");
	ui.button(true);
	ui.button(false);
	ui.button(true);
	ui.button(false);
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == "menus/main.mnu", "a double click opens it");
	CHECK(hover_find(ui, item_id(table, {"readme.txt", "##row"}), x, top, bottom, row), "readme.txt's row");
	ui.button(true);
	ui.button(false);
	ui.button(true);
	ui.button(false);
	CHECK(window->selected() == "readme.txt" && ui.drain().empty(), "a text file is selected, not opened");

	// New: weapon.def made at once; items.def, which the project has, not offered; a menu's
	// name asked first, checked as it is typed.
	const ImGuiID combo = ImHashStr("##Combo_00");
	ui.activate(item_id(files, {"##new"}));
	ui.activate(item_id(pushed(combo, factory_index("items_def", AssetKind::ItemDefs)), {"items.def"}));
	CHECK(ui.drain().empty(), "items.def is in the project: not offered");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
	ui.activate(item_id(files, {"##new"}));
	ui.activate(item_id(pushed(combo, factory_index("weapon_def", AssetKind::WeaponDefs)), {"weapon.def"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateFile) && requests[0].path == "weapon.def" && requests[0].text == "weapon_defs",
	      "weapon.def made at once from its factory");
	ui.activate(item_id(files, {"##new"}));
	ui.activate(item_id(pushed(combo, factory_index("", AssetKind::Menu)), {"Menu..."}));
	ui.frames(2);
	CHECK(modal_open("New file") && ui.drain().empty(), "a menu's name asked first");
	const ImGuiID prompt = ImHashStr("New file");
	type_into(ui, item_id(prompt, {"Name"}), "main.mnu");
	CHECK(logged_frame(ui).find("The project has a file named main.mnu already.") != std::string::npos, "a name taken");
	ui.activate(item_id(prompt, {"Create"}));
	CHECK(ui.drain().empty() && modal_open("New file"), "Create waits for a name the project can take");
	type_into(ui, item_id(prompt, {"Name"}), "extra.mnu");
	ui.activate(item_id(prompt, {"Create"}));
	requests = ui.drain();
	ui.frames(2);
	CHECK(one(requests, EditorRequestKind::CreateFile) && requests[0].path == "extra.mnu" && requests[0].text == "menu" &&
	              !modal_open("New file"),
	      "Create: the name and the kind, the prompt closed");
	// S11h: a texture is the placeholder the texture factory makes, for a name it takes only.
	ui.activate(item_id(files, {"##new"}));
	ui.activate(item_id(pushed(combo, factory_index("", AssetKind::Texture)), {"Texture..."}));
	ui.frames(2);
	CHECK(modal_open("New file") && ui.drain().empty(), "a texture's name asked first");
	type_into(ui, item_id(prompt, {"Name"}), "badge.png");
	CHECK(logged_frame(ui).find("A placeholder texture is a .tga, .mdt, .pcx or .dds file") != std::string::npos,
	      "no placeholder is a .png");
	ui.activate(item_id(prompt, {"Create"}));
	CHECK(ui.drain().empty() && modal_open("New file"), "Create waits for a name the factory takes");
	type_into(ui, item_id(prompt, {"Name"}), "skin.tga");
	ui.activate(item_id(prompt, {"Create"}));
	requests = ui.drain();
	ui.frames(2);
	CHECK(one(requests, EditorRequestKind::CreateFile) && requests[0].path == "skin.tga" && requests[0].text == "texture" &&
	              !modal_open("New file"),
	      "Create: the placeholder texture");

	// A file's menu: Rename..., the new name typed, Rename.
	CHECK(hover_find(ui, item_id(table, {"defs", "defs/items.def", "##row"}), x, top, bottom, row), "items.def's row");
	ui.button(true, 1);
	ui.button(false, 1);
	const ImGuiID menu = item_id(table, {"defs", "defs/items.def", "file_menu"});
	ui.activate(popup_item(menu, "Rename..."));
	ui.frames(2);
	const ImGuiID rename = item_id(files, {"Rename"});
	type_into(ui, popup_item(rename, "##name"), "things.def");
	ui.activate(popup_item(rename, "Rename"));
	requests = ui.drain();
	const EditorRequest *renamed = only(requests, EditorRequestKind::RenameAsset);
	CHECK(renamed && renamed->path == "defs/items.def" && renamed->text == "things.def",
	      "Rename...: every file naming it rewritten, or refused");
	// S12 D9: what it would rewrite asked of the session as the name is typed (PreviewRename).
	CHECK(std::any_of(requests.begin(), requests.end(),
	                  [](const EditorRequest &request) {
		                  return request.kind == EditorRequestKind::PreviewRename && request.path == "defs/items.def" &&
		                         request.edit.field.empty() && std::get<std::string>(request.edit.value) == "things.def";
	                  }),
	      "Rename... previews the rename as the name is typed");

	// S12: a ShowInFiles ask (the view's reveal_file, by its serial) selects the file and clears
	// a filter that hides it; with the ask's rename, Rename... opens on it.
	ui.frames(2);
	type_into(ui, item_id(files, {"##filter"}), "defs");
	CHECK(files_text().find("options.mnu") == std::string::npos, "the filter hides options.mnu");
	v.reveal_file = "menus/sub/options.mnu";
	++v.reveal_file_serial;
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(window->selected() == "menus/sub/options.mnu" && files_text().find("options.mnu") != std::string::npos,
	      "the file asked for selected, the filter that hid it cleared");
	v.reveal_file_rename = true;
	++v.reveal_file_serial;
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(logged_frame(ui).find("Rename options.mnu to") != std::string::npos, "Rename... asked on it");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
}

// Whether `second` follows `first` in `text` on the same logged line.
bool same_line(const std::string &text, const char *first, const char *second) {
	const size_t a = text.find(first);
	if (a == std::string::npos) return false;
	const size_t b = text.find(second, a);
	return b != std::string::npos && text.find('\n', a) > b;
}

// The import dialog over a plan (S11g): "Include the files these need" with the found count,
// then the rows, the chosen files first and the files they need after them in the plan's
// order, each with its kind, what needs it and where it was found; under them the files not
// found and what needs each, the file found in two places, the kinds not followed, the cap's
// notice and the finding. Import raises ImportFiles with exactly the checked rows' sources
// (walk.o3a's two outputs one source); a row unchecked drops out, and unchecking one of a
// converter's outputs takes the other with it; the row the project cannot take stays
// unchecked. The check box raises the setting's request. A listing's choices raise PlanImport
// with the files chosen; a changed plan says so; a file with unsaved edits holds nothing, and
// the unsaved prompt an Import raises takes the dialog's place until it is answered (S12).
void test_import_dialog() {
	SessionView v = seeded_view();
	v.import_preview = planned_import("C:/assets");
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	CHECK(modal_open("Import files"), "the dialog opens");
	// Wide enough that no cell is cut (the bounds sweep checks a narrow one).
	ImGui::SetWindowSize("Import files", ImVec2(1700.0f, 1000.0f));
	ui.frames(2);
	std::string text = logged_frame(ui);
	CHECK(in_order(text, {"Include the files these need (3 found)", "menu.mnu", "CHECK.adm", "walk.bad", "arial99.fnt", "Font",
	                      "menu.mnu: MAIN/TITLE font.name", "the folder assets", "logo.tga", "a_long_texture_name.tga",
	                      "Not found (1)", "gone.tga", "menu.mnu: MAIN/KEEP/Appearance 1 value",
	                      "arial99.fnt: found in both the folder C:/assets and the game install; using the folder C:/assets",
	                      "The files these kinds name are not looked for yet: Terrain.",
	                      "References that name no file are not followed: screen.", "The plan stopped at 1000 files",
	                      "broken.mnu: The file could not be read.", "Replace existing files", "Import 5 files", "Cancel"}),
	      "the plan: the rows, then what is not found, found twice, not followed, the cap, the finding");
	CHECK(same_line(text, "menu.mnu", "chosen") && same_line(text, "CHECK.adm", "made from walk.o3a, the folder assets"),
	      "a chosen file says so; a converter's output, what it is made from");
	const ImGuiID dialog = ImHashStr("Import files");
	const auto sources = [](const EditorRequest &request) {
		std::vector<std::string> out;
		for (const ImportSource &source : request.imports) out.push_back(source.path);
		return out;
	};
	using Paths = std::vector<std::string>;
	ui.activate(item_id(dialog, {"###import"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && !requests[0].flag &&
	              sources(requests[0]) == Paths({"C:/assets/menu.mnu", "C:/assets/walk.o3a", "C:/assets/arial99.fnt",
	                                              "C:/assets/logo.tga"}) &&
	              requests[0].imports[2].native && !requests[0].imports[0].native,
	      "Import: the checked rows' sources, each once, the found ones native");
	// Opened again (the view still previews): logo.tga unchecked, CHECK.adm unchecked with
	// walk.bad, the long name's row disabled.
	ui.frames(3);
	ui.activate(import_table_item("import_plan", 5, "##take"));
	ui.activate(import_table_item("import_plan", 1, "##take"));
	ui.activate(import_table_item("import_plan", 6, "##take"));
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Import 2 files") != std::string::npos, "two rows left checked");
	ui.activate(item_id(dialog, {"###import"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) &&
	              sources(requests[0]) == Paths({"C:/assets/menu.mnu", "C:/assets/arial99.fnt"}),
	      "Import: exactly the rows kept, a converter's outputs together, the row that cannot be taken never");
	// "Include the files these need": the setting's request, off.
	ui.frames(3);
	ui.activate(item_id(import_body_id(), {"###needs"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::SetImportDependencies) && !requests[0].flag, "the check box asks for the setting");

	// An archive's members to choose from: a choice checked plans the chosen files again.
	v.import_preview.choices = {{"C:/assets/data.pff", "main.mnu", false, false}, {"C:/assets/data.pff", "stat.mnu", false, false}};
	++v.import_preview.serial;
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"Choose the files to import from the archive data.pff:", "main.mnu", "stat.mnu",
	                      "Include the files these need"}),
	      "the list to choose from above the plan");
	ui.activate(import_table_item("import_choices", 1, "###pick"));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PlanImport) && requests[0].flag &&
	              sources(requests[0]) == Paths({"C:/assets/menu.mnu", "C:/assets/walk.o3a", "C:/assets/data.pff"}) &&
	              requests[0].imports[2].entry == "stat.mnu",
	      "a choice checked: the files chosen planned again");
	ui.activate(item_id(import_body_id(), {"Select shown"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PlanImport) && requests[0].imports.size() == 4, "Select shown: every listed file");

	// The files changed since the preview: said above the plan.
	v.import_preview.choices.clear();
	v.import_preview.changed = true;
	++v.import_preview.serial;
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	ui.away();
	CHECK(logged_frame(ui).find("The files changed since the preview") != std::string::npos, "a changed plan says so");
	// S12: a file with unsaved edits holds nothing here: Import goes to the session, whose guard
	// asks to save only a file the import writes over. While that prompt is open the dialog gives
	// way to it, and comes back as it was, Replace existing files still checked.
	editor_test::TempProjectDir dir("opennova_editor_ui_import_dirty");
	v.documents.push_back(edited(menu_at(dir, "a.mnu", "menus/a.mnu")));
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(2);
	ui.drain();
	ui.activate(item_id(dialog, {"Replace existing files"}));
	ui.activate(item_id(dialog, {"###import"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && requests[0].flag,
	      "unsaved edits: Import goes to the session, with Replace existing files");
	v.unsaved_prompt.open = true;
	v.unsaved_prompt.action = EditorRequestKind::ImportFiles;
	v.unsaved_prompt.files = {"menus/a.mnu"};
	v.unsaved_prompt.can_discard = false;
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	CHECK(modal_open("Unsaved changes") && !modal_open("Import files") &&
	              logged_frame(ui).find("Save all and import") != std::string::npos,
	      "the unsaved prompt, and not the dialog, while it is open");
	v.unsaved_prompt = SessionView::UnsavedPrompt();
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	ui.drain();
	CHECK(modal_open("Import files") && !modal_open("Unsaved changes"), "the dialog back once the prompt is answered");
	ui.activate(item_id(dialog, {"###import"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && requests[0].flag, "Replace existing files kept");
	// The session closed the preview: the dialog closes.
	v.import_preview = SessionView::ImportPreview();
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(2);
	CHECK(!modal_open("Import files"), "the dialog closed");
}

// A chosen file the project cannot take stays checked, and Import waits, saying why, until it
// is unchecked: a menu and a texture whose name runs past what the archives store, both
// chosen. Unchecked, Import takes the menu alone; the texture's check box, now off, cannot be
// checked again.
void test_import_dialog_problem_root() {
	SessionView v = seeded_view();
	SessionView::ImportPreview &preview = v.import_preview;
	preview.open = true;
	preview.serial = 1;
	ImportPlanRow menu;
	menu.state = ImportPlanRow::State::Selected;
	menu.selected = true;
	menu.source = {"C:/art/hud.mnu", "", false, false};
	menu.name = "hud.mnu";
	menu.kind = AssetKind::Menu;
	menu.destination = "menus/hud.mnu";
	menu.found_in = "the folder C:/art";
	ImportPlanRow texture = menu;
	texture.source = {"C:/art/a_very_long_texture_name.tga", "", false, false};
	texture.name = "a_very_long_texture_name.tga";
	texture.kind = AssetKind::Texture;
	texture.destination = "a_very_long_texture_name.tga";
	texture.problem = "The name is longer than the 16 characters the game's archives store.";
	preview.roots = {menu.source, texture.source};
	preview.plan.rows = {menu, texture};
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	ImGui::SetWindowSize("Import files", ImVec2(1400.0f, 800.0f));
	ui.frames(2);
	const ImGuiID dialog = ImHashStr("Import files");
	CHECK(logged_frame(ui).find("Import 2 files") != std::string::npos, "a chosen file with a problem stays checked");
	ui.activate(item_id(dialog, {"###import"}));
	CHECK(only(ui.drain(), EditorRequestKind::ImportFiles) == nullptr, "Import waits while it is checked");
	// Hovered, the blocked Import says why (it follows Replace existing files on its line).
	const ImGuiWindow *window = ImGui::FindWindowByName("Import files");
	const ImGuiStyle &style = ImGui::GetStyle();
	ImVec2 at;
	const bool hovered = window &&
	                     hover_find(ui, item_id(dialog, {"###import"}),
	                                window->Pos.x + style.WindowPadding.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
	                                        ImGui::CalcTextSize("Replace existing files").x + style.ItemSpacing.x + 12.0f,
	                                window->Pos.y + window->Size.y - 120.0f, window->Pos.y + window->Size.y, at);
	CHECK(hovered && logged_frame(ui).find("a_very_long_texture_name.tga cannot be imported") != std::string::npos,
	      "the blocked Import says why");
	ui.away();
	ui.activate(import_table_item("import_plan", 1, "##take"));
	CHECK(logged_frame(ui).find("Import 1 file") != std::string::npos, "unchecked: one file left");
	ui.activate(import_table_item("import_plan", 1, "##take"));
	CHECK(logged_frame(ui).find("Import 1 file") != std::string::npos, "it cannot be checked again");
	ui.activate(item_id(dialog, {"###import"}));
	const std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && requests[0].imports.size() == 1 &&
	              requests[0].imports[0].path == "C:/art/hud.mnu",
	      "Import: the menu alone");
}

std::string lowered(std::string text) {
	for (char &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

// The Preview window over a real session as the shell drives it: after each frame the
// requests the windows raised go to the session (kept, for a test to take) and the model
// pane's device follows the view.
struct PreviewRun {
	ProjectSession &session;
	ModelDevice &device;
	Ui &ui;
	std::vector<EditorRequest> raised;

	void pump() {
		for (const EditorRequest &request : ui.drain()) {
			raised.push_back(request);
			session.handle(request);
		}
		device.held.follow(session.view());
	}
	void settle() {
		for (int i = 0; i < 3; ++i) {
			pump();
			ui.frames(1);
		}
		pump();
	}
	// A document made the active one (as the editor MCP makes it), and the frame after, lower case.
	std::string open(const char *path) {
		session.handle(make_request(EditorRequestKind::OpenDocument, path));
		settle();
		return lowered(logged_frame(ui));
	}
	std::vector<EditorRequest> take() {
		std::vector<EditorRequest> out;
		out.swap(raised);
		return out;
	}
	// The Preview window's line, hovered: its tooltip in the frame's text.
	std::string header_tooltip() {
		const ImGuiWindow *preview = ImGui::FindWindowByName("Preview");
		if (!preview) return std::string();
		ui.mouse(preview->DC.CursorStartPos.x + 8.0f, preview->DC.CursorStartPos.y + 4.0f);
		const std::string text = logged_frame(ui);
		ui.away();
		return text;
	}
};

size_t count_of_kind(const std::vector<EditorRequest> &requests, EditorRequestKind kind) {
	size_t count = 0;
	for (const EditorRequest &request : requests) count += request.kind == kind ? 1 : 0;
	return count;
}

// One field of a document set through the session, as the Inspector sets it.
void set_field(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field, Value value) {
	EditorRequest set = make_request(EditorRequestKind::EditRecord, document.path());
	set.edit.address = address;
	set.edit.field = field;
	set.edit.value = std::move(value);
	session.handle(set);
}

// The Preview window over a real session: nothing to preview says what to open; the pane of
// the active document's family (main.mnu, its stylesheet and a string table the menu pane;
// the animation table and a lone clip the model pane, each named on the model it plays on);
// a catalog keeps the pane shown, and before the window showed anything it would take the
// menu's; a family with nothing to show gives way to the other (the table closed: the
// menu's; the menu closed: the clip's; a string table with no menu open: the clip's); with
// neither, what to open again. A model newly previewed never takes the focus. The line
// naming a menu or a model says when that file has unsaved changes.
void test_preview_follows() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_follows");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	ModelDevice device;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_model_preview_viewport(&device);
	PreviewRun run{session, device, ui, {}};
	const char *const kNothing = "open a menu, a model or an animation to preview it.";
	run.settle();
	CHECK(lowered(logged_frame(ui)).find(kNothing) != std::string::npos, "nothing to preview: what to open");

	std::string text = run.open("main.mnu");
	CHECK(text.find("main.mnu - startup") != std::string::npos && text.find("no preview renderer is attached.") != std::string::npos,
	      "a menu: the menu pane, its screen named");
	text = run.open("anims/SKIN.adm");
	CHECK(text.find("skin.adm on skinned.3di") != std::string::npos && text.find("main.mnu - startup") == std::string::npos,
	      "the table: the model pane, the table on the model it plays on");
	text = run.open("anims/walk.bad");
	CHECK(text.find("walk.bad on skinned.3di") != std::string::npos, "a lone clip: on the model its table plays on");
	text = run.open("menu_style.mns");
	CHECK(text.find("main.mnu - startup") != std::string::npos, "the stylesheet: the screen it styles");
	text = run.open("gametext.bin");
	CHECK(text.find("main.mnu - startup") != std::string::npos, "a string table: the menu pane too");
	run.open("anims/SKIN.adm");
	text = run.open("items.def");
	CHECK(text.find("skin.adm on skinned.3di") != std::string::npos, "a catalog keeps the pane shown");
	CHECK(preview_family(v, PreviewFamily::None) == PreviewFamily::Menu, "before it showed anything, with both: the menu's");

	// A family with nothing to show gives way to the other.
	session.handle(make_request(EditorRequestKind::CloseDocument, "anims/SKIN.adm"));
	run.settle();
	CHECK(lowered(logged_frame(ui)).find("main.mnu - startup") != std::string::npos, "the table closed: the menu's pane");
	run.open("anims/walk.bad");
	run.open("main.mnu");
	run.open("items.def");
	const Document *main_menu = session.document_for("main.mnu");
	CHECK(main_menu != nullptr, "the menu open");
	if (!main_menu) return;
	session.handle(make_request(EditorRequestKind::CloseDocument, main_menu->path()));
	run.settle();
	CHECK(lowered(logged_frame(ui)).find("walk.bad on skinned.3di") != std::string::npos, "the menu closed: the clip's pane");
	text = run.open("gametext.bin");
	CHECK(text.find("walk.bad on skinned.3di") != std::string::npos, "a string table with no menu open: the clip's pane");
	session.handle(make_request(EditorRequestKind::CloseDocument, "anims/walk.bad"));
	run.settle();
	CHECK(lowered(logged_frame(ui)).find(kNothing) != std::string::npos, "neither: what to open");

	// A model opened while Document has the focus: the model pane shows, the focus stays.
	ui.focus("Document");
	text = run.open("models/armory.3di");
	CHECK(text.find("armory.3di") != std::string::npos && GImGui->NavWindow == ImGui::FindWindowByName("Document"),
	      "a model newly previewed takes no focus");

	// The model edited (a user point moved): its line says it is unsaved; the menu's likewise.
	const auto *armory = dynamic_cast<const ModelDocument *>(session.document_for("models/armory.3di"));
	CHECK(armory && armory->model_row(), "armory open");
	if (!armory || !armory->model_row()) return;
	CHECK(run.header_tooltip().find("Unsaved changes") == std::string::npos, "saved: no mark");
	const ModelRow &row = *armory->model_row();
	const NodeAddress point{row.id, node_kind(ModelKind::UserPoint), row.collections[3][0]};
	Value x;
	CHECK(armory->get(point, "position.x", x) && std::holds_alternative<double>(x), "the user point's x");
	set_field(session, *armory, point, "position.x", std::get<double>(x) + 1.0);
	run.settle();
	CHECK(armory->dirty() && in_order(run.header_tooltip(), {"armory.3di", "models/armory.3di", "Unsaved changes"}),
	      "an unsaved model: its line says so");
	run.open("main.mnu");
	Document *menu = session.document_for("main.mnu");
	NodeAddress title;
	CHECK(menu && menu->find("TITLE", title), "the menu's title");
	if (!menu) return;
	set_field(session, *menu, title, "name", std::string("HEADING"));
	run.settle();
	CHECK(menu->dirty() && in_order(run.header_tooltip(), {"main.mnu - STARTUP", menu->path().c_str(), "Unsaved changes"}),
	      "an unsaved menu: its line says so");
}

// The model pane's gestures over a real session: F frames the selected marker while the pane
// shows and does nothing to its camera while the menu pane shows; a drag of the selected
// marker ends once, for the model, when the menu becomes the active document mid-drag, and
// letting go raises nothing.
void test_preview_model_gestures() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_model_gestures");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	ModelDevice device;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_model_preview_viewport(&device);
	PreviewRun run{session, device, ui, {}};
	run.open("main.mnu");
	run.open("models/armory.3di");
	const auto *armory = dynamic_cast<const ModelDocument *>(session.document_for("models/armory.3di"));
	CHECK(armory && armory->model_row() && device.held.status() == ModelPreviewStatus::Ready, "armory previewed");
	if (!armory || !armory->model_row()) return;
	const ModelRow &row = *armory->model_row();
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, armory->path());
	select.edit.address = {row.id, node_kind(ModelKind::UserPoint), row.collections[3][0]};
	session.handle(select);
	ui.focus("Preview");
	run.settle();

	// F: the selected marker framed while the pane shows; nothing while the menu's shows.
	const auto press_f = [&]() {
		ui.key(ImGuiKey_F, true);
		ui.key(ImGuiKey_F, false);
		run.pump();
	};
	device.held.camera().distance = 40.0f;
	press_f();
	CHECK(device.held.camera().distance != 40.0f, "F frames the selected marker");
	run.open("main.mnu");
	ui.focus("Preview");
	device.held.camera().distance = 40.0f;
	press_f();
	CHECK(device.held.camera().distance == 40.0f, "the model pane hidden: F leaves its camera");

	// A drag of the selected marker (Alt: placed freely), the menu made active mid-drag.
	run.open("models/armory.3di");
	ui.focus("Preview");
	run.settle();
	press_f(); // the marker framed, in the middle of the picture
	run.settle();
	run.take();
	const ModelOverlay *marker = nullptr;
	const std::vector<ModelOverlay> overlays = device.held.overlays();
	for (const ModelOverlay &overlay : overlays)
		if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == 0) marker = &overlay;
	float x = 0.0f, y = 0.0f;
	CHECK(marker && device.held.camera().project(marker->at, device.held.device_width(), device.held.device_height(), x, y),
	      "the marker on the picture");
	if (!marker) return;
	const ImVec2 at(device.origin.x + x, device.origin.y + y);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
	ui.mouse(at.x, at.y);
	ui.button(true);
	ui.mouse(at.x + 30.0f, at.y + 10.0f);
	run.pump();
	std::vector<EditorRequest> requests = run.take();
	const EditorRequest *step = only(requests, EditorRequestKind::EditRecord);
	CHECK(step && step->path == armory->path() && count_of_kind(requests, EditorRequestKind::EndEdit) == 0,
	      "the drag's first step");
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	run.settle();
	requests = run.take();
	const EditorRequest *end = only(requests, EditorRequestKind::EndEdit);
	CHECK(end && end->path == armory->path() && count_of_kind(requests, EditorRequestKind::EditRecord) == 0,
	      "the menu made active mid-drag: the drag's one end, for the model");
	ui.mouse(at.x + 60.0f, at.y + 20.0f);
	ui.button(false);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
	run.settle();
	CHECK(run.take().empty(), "letting go raises nothing");
}

// The OS window's title: the product, the project's name before it, a bullet while a file has
// unsaved changes.
void test_window_title() {
	SessionView v;
	CHECK(editor_window_title(v) == "OpenNova Editor", "no project");
	v.project_open = true;
	v.document.title = "Armory";
	CHECK(editor_window_title(v) == "Armory - OpenNova Editor", "a project");
	editor_test::TempProjectDir dir("opennova_editor_ui_title_test");
	const auto menu = load_menu(dir);
	v.documents = {menu};
	CHECK(editor_window_title(v) == "Armory - OpenNova Editor", "every file saved");
	edited(menu);
	CHECK(editor_window_title(v) == "Armory \xE2\x97\x8F - OpenNova Editor", "a file with unsaved changes");
}

} // namespace

// S13 D1: Files makes its tree and each file's counts again only when what they read moves (the
// files, the findings): a line of Output, the status line and every step of a build leave them
// as they were, and a file the scan finds anew makes them again.
void test_files_tree_kept() {
	editor_test::TempProjectDir dir("opennova_editor_ui_files_kept");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Kept"));
	editor_test::create_missing_files(session);
	Ui ui;
	ui.windows.set_view(&session.view());
	ui.frames(6);
	const auto *window = dynamic_cast<const FilesWindow *>(find_window(ui.windows.pass(), "Files"));
	CHECK(window != nullptr, "the Files window");
	if (!window) return;
	const size_t made = window->rebuilds();
	CHECK(made >= 1, "the tree made");
	session.handle(make_request(EditorRequestKind::ClearOutput));
	// Nothing to save: the status line alone.
	session.handle(make_request(EditorRequestKind::SaveAll));
	ui.frames(3);
	CHECK(window->rebuilds() == made, "Output and the status line: the tree kept");
	session.handle(make_request(EditorRequestKind::Build)); // its refresh reads the files again
	ui.frames(2);
	const size_t building = window->rebuilds();
	size_t steps = 0;
	while (session.build_running() && steps < 100) {
		session.poll();
		ui.frames(1);
		if (!session.build_running()) break;
		++steps;
		CHECK(window->rebuilds() == building, "a build's step: the tree kept");
	}
	CHECK(steps > 1 && session.view().has_build, "the build stepped");
	const std::string readme = session.view().project_root + "/notes/readme.txt";
	CHECK(editor_test::write_text(readme, "x"), "a file written");
	session.handle(make_request(EditorRequestKind::Rescan));
	ui.frames(2);
	CHECK(window->rebuilds() > building && logged_frame(ui).find("readme.txt") != std::string::npos,
	      "a file found anew: the tree made again");
}

void run_workspace_tests() {
	test_files_tree_kept();
	test_workspace_layout();
	test_document_tabs();
	test_document_tab_choices();
	test_view_prompt_outlives_its_tab();
	test_thirty_tabs();
	test_welcome_view();
	test_project_settings();
	test_menus();
	test_menu_bar_status();
	test_files_window();
	test_import_dialog();
	test_import_dialog_problem_root();
	test_preview_follows();
	test_preview_model_gestures();
	test_window_title();
}

} // namespace editor_ui_test
