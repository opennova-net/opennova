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
// the buttons; never over the menus); a canvas whose picture fills it read through ImGui (the
// right button apart from a press, the keys a camera flies by, a line of text drawn); Files (its folders, the file count, the kind hidden
// until the header's menu shows it, a filter's flat list, a click and a double click, New
// and its name prompt, Rename...); the import dialog; the OS window's title; and the view
// events' mailboxes (each event held until its window draws, taken once). The mission's view
// in the Document window (ADR 0046 S14): its toolbar raises SetViewports alone, a marquee over
// its picture one SelectRecord across the four pools, which the Inspector shows as one shared
// form whose change is one batch over every record.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <editor/blank/blank_factory.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/field_text.h>
#include <editor/preview/mission_palette.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/model_viewport.h>
#include <editor/preview/viewports.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_run.h>
#include <editor/session/file_preferences_store.h>
#include <editor/session/original_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_window.h>
#include <editor/ui/editor_windows.h>
#include <editor/ui/files_window.h>
#include <editor/ui/preview_window.h>
#include <editor/ui/viewport_canvas.h>
#include <editor/ui/view_event_mailbox.h>
#include "../editor/anim_test_support.h"
#include "../editor/editor_test_support.h"
#include "common/file_io.h"
#include "common/test_paths.h"
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

// The requests but the SelectFiles a click in Files raises (ADR 0046 S18: the selection is the
// session's too).
std::vector<EditorRequest> without_selects(std::vector<EditorRequest> requests) {
	requests.erase(std::remove_if(requests.begin(), requests.end(),
	                              [](const EditorRequest &r) { return r.kind == EditorRequestKind::SelectFile; }),
	               requests.end());
	return requests;
}

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
	v.project.open = true;
	v.project.root = "C:/mods/My Game";
	editor_test::own(v.project.document).title = "My Game";
	editor_test::own(v.project.scan)
			.entries.push_back(file_entry("main.mnu", "menus/main.mnu", AssetKind::Menu));
	editor_test::own(v.project.scan).index();
	RequirementRow row;
	row.role = "main_menu";
	row.name = "main.mnu";
	row.required = true;
	row.state = RequirementState::Present;
	editor_test::own(v.project.requirements).rows.push_back(row);
	RequirementRow missing;
	missing.role = "gametext";
	missing.name = "gametext.bin";
	missing.required = true;
	missing.expected_kind = AssetKind::Strings;
	missing.state = RequirementState::Missing;
	editor_test::own(v.project.requirements).rows.push_back(missing);
	editor_test::own(v.project.requirements).required_total = 2;
	editor_test::own(v.project.requirements).required_missing = 1;
	Diagnostic lacking = editor_test::finding_of(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	lacking.subject = RequirementSubject{missing.role, missing.name};
	v.findings.diagnostics.push_back(lacking);
	v.activity.output.append("Opened My Game");
	v.activity.output.append("Build started.");
	v.project.recent_projects = {"C:/mods/My Game", "C:/mods/Other"};
	v.activity.status = "Opened My Game.";
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
	// A project open with nothing in it: every window draws (with none open, every window but Document
	// stands aside for the welcome page, below).
	SessionView opened;
	opened.project.open = true;
	opened.project.root = "C:/mods/Layout";
	windows.set_view(&opened);
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
	CHECK(logged_frame(windows, 8).find("Open a menu, a model or an animation to preview it, or select a texture in Files.") != std::string::npos,
	      "nothing to preview yet");
	// No project open (the UX round's project lane): the welcome page, the workspace's whole; the other windows
	// stand aside, open in the Windows menu.
	SessionView none;
	windows.set_view(&none);
	for (uint64_t i = 100; i < 104; ++i) frame(windows, i);
	const std::string home = logged_frame(windows, 104);
	CHECK(home.find("New project") != std::string::npos && home.find("OpenNova Editor") != std::string::npos,
	      "no project: the welcome page");
	CHECK(!files->Active && !preview->Active && !inspector->Active && !problems->Active && !output->Active &&
	              find_window(pass, "Files")->open,
	      "the other windows stand aside, still open");
	CHECK(near(document->Size.x, size.x, 2.0f * ImGui::GetStyle().DockingSeparatorSize + 1.0f),
	      "the welcome page the whole width");

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
	editor_test::own(v.project.scan)
			.entries.push_back(file_entry("items.def", "items.def", AssetKind::ItemDefs));
	editor_test::own(v.project.scan).index();
	v.documents.open.push_back(catalog);
	v.documents.active = catalog->path();
	v.documents.selection.primary = { catalog->rows()[0]->id, node_kind(opennova::def::DefRecordKind::Item),
		0 };
	windows.set_view(&v);
	for (uint64_t i = 9; i < 15; ++i) frame(windows, i);
	CHECK(ImGui::GetDrawData()->TotalVtxCount > 0, "the project layout draws");
	const std::string text = logged_frame(windows, 15);
	CHECK(in_order(text, {"items.def", "Reload", "line(s) the game ignores", "Marker"}), "the catalog's tab: its toolbar, notice and records");
	CHECK(windows.pending_requests() == 0, "drawing raises no request by itself");
	v.dialogs.import_preview = planned_import("C:/assets");
	v.revisions.touch(ViewConcern::Dialogs);
	for (uint64_t i = 16; i < 19; ++i) frame(windows, i);
	CHECK(modal_open("Import files"), "the import dialog draws from the workspace");
	CHECK(windows.pending_requests() == 0, "previewing an import writes nothing");
	windows.pass().detach_imgui();
}

// The docked windows keep their share of the window through the OS window maximized and restored, and
// minimized and restored (S17: the Document dock was left a 20 px strip after a maximize and a restore).
void test_dock_survives_resizes() {
	Ui ui;
	SessionView opened; // a project open: every window docked shows
	opened.project.open = true;
	opened.project.root = "C:/mods/Resizes";
	ui.windows.set_view(&opened);
	ImGui::GetIO().DisplaySize = ImVec2(1600.0f, 900.0f);
	ui.frames(6);
	const char *const titles[] = {"Files", "Document", "Preview", "Inspector", "Problems"};
	const auto sizes = [&] {
		std::vector<ImVec2> out;
		for (const char *title : titles) {
			const ImGuiWindow *window = ImGui::FindWindowByName(title);
			out.push_back(window ? window->Size : ImVec2(0.0f, 0.0f));
		}
		return out;
	};
	const std::vector<ImVec2> before = sizes();
	for (const ImVec2 &s : before) CHECK(s.x > 100.0f && s.y > 100.0f, "every docked window has room at first");
	const ImVec2 sequences[][2] = {{ImVec2(2560.0f, 1400.0f), ImVec2(1600.0f, 900.0f)},
	                               {ImVec2(0.0f, 0.0f), ImVec2(1600.0f, 900.0f)},
	                               {ImVec2(160.0f, 28.0f), ImVec2(1600.0f, 900.0f)}};
	for (int s = 0; s < int(std::size(sequences)) + 2; ++s) {
		ImVec2 sequence[2] = {ImVec2(0.0f, 0.0f), ImVec2(1600.0f, 900.0f)};
		if (s < int(std::size(sequences))) {
			sequence[0] = sequences[s][0];
			for (const ImVec2 &size : sequences[s]) {
				ImGui::GetIO().DisplaySize = size;
				ui.frames(4);
			}
		} else {
			// A drag of the window's corner: a step a frame down to a small window and back up.
			const float low = s == int(std::size(sequences)) ? 640.0f : 120.0f;
			sequence[0] = ImVec2(low, low * 0.5625f);
			for (float w = 1600.0f; w > low; w -= 40.0f) {
				ImGui::GetIO().DisplaySize = ImVec2(w, w * 0.5625f);
				ui.frames(1);
			}
			for (float w = low; w <= 1600.0f; w += 40.0f) {
				ImGui::GetIO().DisplaySize = ImVec2(w, w * 0.5625f);
				ui.frames(1);
			}
			ImGui::GetIO().DisplaySize = ImVec2(1600.0f, 900.0f);
			ui.frames(4);
		}
		const std::vector<ImVec2> after = sizes();
		for (size_t i = 0; i < after.size(); ++i) {
			char message[160];
			std::snprintf(message, sizeof(message), "%s after %.0fx%.0f and back: %.0fx%.0f, was %.0fx%.0f", titles[i],
			              sequence[0].x, sequence[0].y, after[i].x, after[i].y, before[i].x, before[i].y);
			CHECK(std::fabs(after[i].x - before[i].x) <= before[i].x * 0.1f + 2.0f &&
			              std::fabs(after[i].y - before[i].y) <= before[i].y * 0.1f + 2.0f,
			      message);
		}
	}
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
	editor_test::own(v.project.scan).entries = {file_entry("a.mnu", a->path(), AssetKind::Menu), file_entry("b.mnu", b->path(), AssetKind::Menu),
	                  file_entry("c.mnu", c->path(), AssetKind::Menu)};
	editor_test::own(v.project.scan).index();
	v.documents.open = {a, b, c};
	v.documents.active = a->path();
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
	v.documents.active = c->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(ui.drain().empty() && bar->SelectedTabId == document_tab_id(c->path()), "made active, its tab stays, nothing more");
	// The active document changed elsewhere (a Problems row, the editor MCP): its tab is
	// selected, and no request goes back.
	v.documents.active = b->path();
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
	v.documents.open = {a, c};
	v.documents.active = c->path();
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
	v.documents.open = {a, b, c};
	v.documents.active = a->path();
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
	v.documents.active = b->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.button(true);
	ui.button(false);
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == c->path(),
	      "the click in the frame b became active: c opens");
	v.documents.active = c->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(ui.drain().empty() && bar->SelectedTabId == document_tab_id(c->path()), "c shown and active, nothing more");
	ui.away();
	ui.click(tab_rect(*bar, a->path()).GetCenter());
	ui.frames(2);
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == a->path(), "the next click opens a");
	v.documents.active = a->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	ui.drain();

	// Two x.mnu, in a/ and in b/: two tabs, each labelled by its path, each reachable.
	const auto x1 = menu_at(dir, "x1.mnu", "a/x.mnu");
	const auto x2 = menu_at(dir, "x2.mnu", "b/x.mnu");
	v.documents.open = {a, c, x1, x2};
	v.documents.active = x1->path();
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
	v.documents.active = x2->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	ui.away();
	ui.click(tab_rect(*bar, x1->path()).GetCenter());
	ui.frames(2);
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == x1->path(), "and a/x.mnu");
	v.documents.active = x1->path();
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
	v.documents.open = {a, x1, x2};
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
	v.documents.open = {a, b};
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
	v.documents.active = b->path();
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	CHECK(document_tabs() && document_tabs()->SelectedTabId == document_tab_id(b->path()) && modal_open("Remove screen?"),
	      "its tab hidden, it still asks");
	ui.activate(item_id(ImHashStr("Remove screen?"), {"Remove"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::EditRecord) && requests[0].path == a->path() && requests[0].edits.size() == 1 &&
	              requests[0].edits[0].operation == EditOperation::Remove && requests[0].edits[0].address.row == second,
	      "and removes the screen of the menu it asked about");

	// Asked again; a.mnu read again meanwhile (a new instance, two screens again): the prompt
	// closes, removing nothing.
	v.documents.active = a->path();
	select_in(v, {second, kScreen, 0});
	ui.frames(3);
	CHECK(ask(a), "asked again");
	const auto reread = two_screens("a.mnu", "menus/a.mnu");
	v.documents.open = {reread, b};
	select_in(v, {reread->rows()[1]->id, kScreen, 0});
	ui.frames(3);
	CHECK(!modal_open("Remove screen?") && ui.drain().empty(), "its menu read again: the prompt closes");
	// Asked about the new one, which then closes: likewise.
	CHECK(ask(reread), "asked about the menu read again");
	v.documents.open = {b};
	v.documents.active = b->path();
	v.documents.selection.select_only(v.documents.active, NodeAddress());
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
	v.project.open = true;
	v.project.root = "C:/mods/Thirty";
	editor_test::own(v.project.document).title = "Thirty";
	std::vector<std::shared_ptr<MnuDocument>> menus;
	for (int i = 0; i < 30; ++i) {
		char file[16];
		std::snprintf(file, sizeof(file), "m%02d.mnu", i);
		menus.push_back(menu_at(dir, file, (std::string("menus/") + file).c_str()));
		editor_test::own(v.project.scan)
				.entries.push_back(file_entry(file, menus.back()->path(), AssetKind::Menu));
		v.documents.open.push_back(menus.back());
	}
	editor_test::own(v.project.scan).index();
	v.documents.active = menus.front()->path();
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

// No project: the Document window is the welcome page, the workspace's whole (the UX round's project
// lane): open (a folder, the recent projects by their titles, their games and their folders, one whose
// folder holds no project said so), new, what happened. Create waits for a folder, the shell's pick fills
// it, Create raises NewProject; File > New project... shows the same form in a modal; File > Open recent
// opens one.
void test_welcome_view() {
	SessionView v;
	v.project.recent_projects = {"C:/mods/Armory", "C:/mods/Other", "C:/mods/Gone"};
	v.project.recent_details = {{"C:/mods/Armory", true, "Armory Mod", "Joint Operations", "jxm", "jox01"},
	                            {"C:/mods/Other", true, "Other", "Joint Operations", "", ""},
	                            {"C:/mods/Gone", false, "", "", "", ""}};
	v.activity.status = "No project open.";
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	CHECK(in_order(logged_frame(ui), {"OpenNova Editor", "Open a project", "Open a project folder...", "Armory Mod",
	                                  "Joint Operations, as the expansion jxm on jox01", "C:/mods/Armory", "Other",
	                                  "No project here now", "C:/mods/Gone", "New project", "Name", "Folder", "Game install",
	                                  "No game install chosen", "Create project", "No project open."}),
	      "the welcome page");
	const ImGuiID document = item_id(Ui::window_id("Document"), {"welcome"});
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
	CHECK(one(requests, EditorRequestKind::NewProject) && requests[0].dir == "C:/mods/New" && requests[0].title == "My Game",
	      "Create: the folder picked, the name typed");
	ui.activate(item_id(document, {"Open a project folder..."}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PickDirectory) && requests[0].purpose == PickPurpose::OpenProject, "Open...");

	CHECK(choose(ui, "File", {"New project..."}).empty() && modal_open("New project"), "File > New project... asks");
	CHECK(logged_frame(ui).find("C:/mods/New") != std::string::npos, "with the same form");
	ui.activate(item_id(ImHashStr("New project"), {"Create project"}));
	requests = ui.drain();
	ui.frames(2);
	CHECK(one(requests, EditorRequestKind::NewProject) && requests[0].dir == "C:/mods/New" && !modal_open("New project"),
	      "its Create, and the modal closes");
	requests = choose(ui, "File", {"Open recent", "###C:/mods/Other"});
	CHECK(one(requests, EditorRequestKind::OpenProject) && requests[0].dir == "C:/mods/Other", "File > Open recent");
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
	FilePreferencesStore preferences(settings_file);
	ProjectSession session(platform, preferences);
	CHECK(session.handle(request::new_project(dir.file("Armory"), "Armory")), "a project");
	session.run_operations();
	const SessionView &v = session.view();
	const std::string armory = v.project.root;
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
	              apply->settings.game_install == std::optional<std::string>("") &&
	              apply->settings.runtime_executable == std::optional<std::string>("") &&
	              apply->settings.play_in_install == std::optional<bool>(false),
	      "Apply: one request naming every setting as the dialog holds it");
	CHECK(!modal_open("Project settings") && v.activity.status == "No setting changed.", "nothing changed: written nothing, closed");

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
		CHECK(one(requests, EditorRequestKind::ApplyProjectSettings) &&
						newest_event(v, ViewEventKind::SettingsApplied).tag ==
								requests[0].settings.serial &&
						v.project.settings_result.failures.size() == 1 &&
						v.project.runtime_setting.empty(),
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
	CHECK(v.project.document->title == "Harbor" && v.project.runtime_setting.empty() && modal_open("Project settings") &&
	              open_project(armory, on_disk, error) && on_disk.title == "Harbor",
	      "a partial failure: the name written, the runtime not, the dialog open");
	type_into(ui, item_id(dialog, {"Name"}), "Armory");
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(v.project.document->title == "Armory" && open_project(armory, on_disk, error) && on_disk.title == "Armory" &&
	              modal_open("Project settings"),
	      "the name changed back is written back");
	// The settings writable again: the runtime is written, and the dialog closes.
	std::filesystem::remove_all(settings_file + ".tmp", ec);
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(!modal_open("Project settings") && v.project.runtime_setting == "C:/tools/opennova.exe" && v.project.document->title == "Armory" &&
	              v.project.settings_result.failures.empty(),
	      "none failed: done");

	// Browse...: the shell's answer fills the field it asked for (an answer for another field
	// is dropped).
	choose(ui, "File", {"Project settings..."});
	ui.activate(item_id(dialog, {"Browse...##install"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PickDirectory) && requests[0].purpose == PickPurpose::GameInstall,
	      "Browse... asks the shell for a folder");
	// The install picked is a path this platform calls absolute ("C:/..." is relative on
	// Linux), so the session keeps it as it is.
	const std::string install = dir.file("Joint Operations");
	ui.windows.deliver_pick(PickPurpose::RuntimeExecutable, "C:/elsewhere/other.exe");
	ui.windows.deliver_pick(PickPurpose::GameInstall, install);
	ui.frames(2);
	std::string text = logged_frame(ui);
	CHECK(text.find(install) != std::string::npos && text.find("C:/elsewhere") == std::string::npos,
	      "the folder in its field, nothing in the other");
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(!modal_open("Project settings") && v.project.retail_directory == install,
			"the game install set");

	// Open in Armory with a new name typed and a Browse... pending, then the editor MCP opens
	// another project: the dialog closes, raising nothing; the answer that comes after, in
	// the dialog opened on the other project, is dropped.
	choose(ui, "File", {"Project settings..."});
	type_into(ui, item_id(dialog, {"Name"}), "Renamed");
	ui.activate(item_id(dialog, {"Browse...##runtime"}));
	CHECK(one(ui.drain(), EditorRequestKind::PickFile) != nullptr, "a Browse... pending");
	CHECK(session.handle(request::new_project(dir.file("Harbor"), "Harbor")) && v.project.root != armory,
	      "the editor MCP opens another project");
	session.run_operations();
	ui.frames(3);
	CHECK(!modal_open("Project settings") && ui.drain().empty(), "the dialog closes with its project, raising nothing");
	choose(ui, "File", {"Project settings..."});
	ui.windows.deliver_pick(PickPurpose::RuntimeExecutable, "C:/late/opennova.exe");
	ui.frames(2);
	CHECK(logged_frame(ui).find("C:/late/opennova.exe") == std::string::npos, "the answer asked for in Armory is dropped");
	ui.activate(item_id(dialog, {"Apply"}));
	serve(ui, session);
	CHECK(!modal_open("Project settings") && v.project.document->title == "Harbor" && v.project.runtime_setting == "C:/tools/opennova.exe" &&
	              open_project(armory, on_disk, error) && on_disk.title == "Armory",
	      "neither project renamed, the runtime as it was");

	// Cancel raises nothing; on a source run the runtime is the checkout's, not a field.
	PlayLauncher launcher;
	launcher.source_run = true;
	launcher.executable = "C:/checkout/godot.exe";
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	choose(ui, "File", {"Project settings..."});
	ui.activate(item_id(dialog, {"Multiplayer"}));
	text = logged_frame(ui);
	CHECK(text.find("A source run") != std::string::npos && text.find("C:/checkout/godot.exe") != std::string::npos,
	      "a source run: the runtime Play drives, no field");
	ui.activate(item_id(dialog, {"Cancel"}));
	ui.frames(2);
	CHECK(!modal_open("Project settings") && ui.drain().empty() &&
					!v.project.document->features.multiplayer,
			"Cancel changes nothing");
}

// Two Applies answered before the settings dialog draws, its own and another client's after it
// (the editor MCP's apply_project_settings): the dialog goes by its own answer, the flag its
// event carries, never the later Apply's result. Its Apply failed and the other did not: it
// stays open saying a setting failed. Its Apply wrote and the other failed: it closes, saying
// nothing of the other's failure.
void test_project_settings_two_applies() {
	editor_test::TempProjectDir dir("opennova_editor_ui_settings_two_applies");
	NoProcess platform;
	const std::string settings_file = dir.file("settings/editor.json");
	FilePreferencesStore preferences(settings_file);
	ProjectSession session(platform, preferences);
	CHECK(session.handle(request::new_project(dir.file("Armory"), "Armory")), "a project");
	session.run_operations();
	const SessionView &v = session.view();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	serve(ui, session);
	const ImGuiID dialog = ImHashStr("Project settings");
	// The editor's settings cannot be written (a folder stands where their file is written
	// first): a runtime typed cannot be set.
	std::error_code ec;
	std::filesystem::create_directories(settings_file + ".tmp", ec);
	CHECK(choose(ui, "File", {"Project settings..."}).empty() && modal_open("Project settings"),
			"the dialog opens");
	type_into(ui, item_id(dialog, {"OpenNova runtime"}), "C:/tools/opennova.exe");
	ui.activate(item_id(dialog, {"Apply"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ApplyProjectSettings) != nullptr, "the dialog's Apply");
	for (const EditorRequest &request : requests)
		session.handle(request);
	ProjectSettingsChange name_only;
	name_only.title = std::string("Harbor");
	session.handle(request::apply_project_settings(name_only));
	CHECK(v.project.document->title == "Harbor" && v.project.settings_result.failures.empty(),
			"the other Apply, after the dialog's, wrote the name");
	ui.frames(2);
	CHECK(modal_open("Project settings") &&
					logged_frame(ui).find("A setting could not be saved") != std::string::npos,
			"its own Apply failed: it stays open, saying a setting failed");

	// The runtime cleared (as in effect: its Apply writes the name back and nothing it cannot);
	// the other Apply after it asks a runtime, which fails.
	type_into(ui, item_id(dialog, {"OpenNova runtime"}), "");
	ui.activate(item_id(dialog, {"Apply"}));
	requests = ui.drain();
	for (const EditorRequest &request : requests)
		session.handle(request);
	ProjectSettingsChange runtime;
	runtime.runtime_executable = std::string("C:/tools/other.exe");
	session.handle(request::apply_project_settings(runtime));
	CHECK(v.project.document->title == "Armory" && v.project.settings_result.failures.size() == 1,
			"the dialog's Apply wrote the name; the other failed");
	ui.frames(2);
	CHECK(!modal_open("Project settings"), "its own Apply wrote: it closes");
	std::filesystem::remove_all(settings_file + ".tmp", ec);
}

// A window's mailbox holds at most as many events as the view keeps: one more drops the
// oldest; taken, the rest come out oldest first, once.
void test_view_event_mailbox_cap() {
	ViewEventMailbox<> mailbox;
	for (uint64_t seq = 1; seq <= ViewEvents::kKept + 6; ++seq) {
		ViewEvent event;
		event.seq = seq;
		mailbox.post(event);
	}
	CHECK(mailbox.held() == ViewEvents::kKept, "at most kKept held");
	const std::vector<ViewEvent> taken = mailbox.take();
	CHECK(taken.size() == ViewEvents::kKept && taken.front().seq == 7 &&
					taken.back().seq == ViewEvents::kKept + 6 && mailbox.held() == 0,
			"the oldest dropped; the rest taken once, oldest first");
}

// The File, Edit and Build menus raise their requests (a Save, a Close and an Undo naming
// the active document); what cannot run now is disabled.
void test_menus() {
	editor_test::TempProjectDir dir("opennova_editor_ui_menus_test");
	const std::shared_ptr<MnuDocument> document = edited(load_menu(dir));
	SessionView v = menu_view(document);
	v.project.recent_projects = {"C:/mods/Menus", "C:/mods/Other"};
	v.project.retail_directory = "C:/games/Joint Operations";
	v.activity.has_build = true;
	editor_test::own(v.activity.last_build).ok = true;
	editor_test::own(v.activity.last_build).build_dir = "C:/mods/Menus/.opennova/build/play/1";
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
	r = raised("File", {"Import from the game data..."}, EditorRequestKind::PreviewInstallImport);
	CHECK(r.kind == EditorRequestKind::PreviewInstallImport && r.names.empty() && r.with_dependencies == v.project.import_dependencies,
	      "File > Import from the game data...: every file to choose from, planned as the setting says");
	r = raised("File", {"Show project folder"}, EditorRequestKind::RevealPath);
	CHECK(r.kind == EditorRequestKind::RevealPath && r.path == v.project.root, "File > Show project folder");
	r = raised("File", {"Close project"}, EditorRequestKind::CloseProject);
	CHECK(r.kind == EditorRequestKind::CloseProject, "File > Close project");
	r = raised("File", {"Quit"}, EditorRequestKind::Quit);
	CHECK(r.kind == EditorRequestKind::Quit, "File > Quit");
	r = raised("Edit", {"Undo"}, EditorRequestKind::Undo);
	CHECK(r.kind == EditorRequestKind::Undo && r.path == document->path(), "Edit > Undo");
	r = raised("Build", {"Build"}, EditorRequestKind::Build);
	CHECK(r.kind == EditorRequestKind::Build, "Build > Build");
	r = raised("Build", {"Play"}, EditorRequestKind::Play);
	CHECK(r.kind == EditorRequestKind::Play && r.mission.empty(), "Build > Play: the game at its menu");
	// S14: Play mission starts the game in the active document's mission (play_mission_for): none
	// for a menu; with a mission active (the project holds it), that mission.
	CHECK(choose(ui, "Build", {"Play mission"}).empty(), "Build > Play mission: the active document is no mission's");
	{
		const std::string menu = v.documents.active;
		editor_test::own(v.project.scan).entries.push_back(file_entry("First.bms", "missions/First.bms", AssetKind::Mission));
		editor_test::own(v.project.scan).index();
		v.documents.active = "missions/First.bms";
		v.revisions.touch(ViewConcern::Files);
		v.revisions.touch(ViewConcern::Documents);
		ui.frames(2);
		ui.drain();
		r = raised("Build", {"Play mission"}, EditorRequestKind::Play);
		CHECK(r.kind == EditorRequestKind::Play && r.mission == "First.bms", "Build > Play mission: the active mission");
		v.documents.active = menu;
		v.revisions.touch(ViewConcern::Documents);
		ui.frames(2);
		ui.drain();
	}
	CHECK(choose(ui, "Build", {"Stop"}).empty(), "Build > Stop: nothing runs");
	r = raised("Build", {"Play in the game install"}, EditorRequestKind::ApplyProjectSettings);
	CHECK(r.kind == EditorRequestKind::ApplyProjectSettings && r.settings.play_in_install == std::optional<bool>(true) &&
	              !r.settings.title && !r.settings.mission && !r.settings.game_install && !r.settings.runtime_executable,
	      "Build > Play in the game install: that setting alone");
	r = raised("Build", {"Show build folder"}, EditorRequestKind::RevealPath);
	CHECK(r.kind == EditorRequestKind::RevealPath && r.path == v.activity.last_build->build_dir, "Build > Show build folder");
	v.activity.play_state = PlayState::Running;
	v.revisions.touch(ViewConcern::Run);
	ui.frames(2);
	r = raised("Build", {"Stop"}, EditorRequestKind::StopPlay);
	CHECK(r.kind == EditorRequestKind::StopPlay, "Build > Stop while the game runs");
	CHECK(choose(ui, "Build", {"Play"}).empty(), "no Play while it runs");
	v.project.retail_directory.clear();
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
// counts (a click shows Problems), the running operation with its Cancel or the game, and Build
// / Play / Stop, each enabled when the busy gate would not refuse it (a Build while one packs
// joins it); a narrow window leaves parts out from the left, what was said first, and at every
// width the right end starts after the menus and Stop ends inside the bar.
void test_menu_bar_status() {
	editor_test::TempProjectDir dir("opennova_editor_ui_status_test");
	const auto a = menu_at(dir, "a.mnu", "menus/a.mnu");
	const auto b = edited(menu_at(dir, "b.mnu", "menus/b.mnu"));
	const auto c = edited(menu_at(dir, "c.mnu", "menus/c.mnu"));
	SessionView v = menu_view(a);
	v.documents.open = {a, b, c};
	for (int i = 0; i < 7; ++i)
		v.findings.diagnostics.push_back(
				editor_test::finding_of(DiagnosticSeverity::Error, "catalog.name_empty", "An error."));
	for (int i = 0; i < 5; ++i)
		v.findings.diagnostics.push_back(
				editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.name_duplicate", "A warning."));
	v.findings.diagnostics.push_back(
			editor_test::finding_of(DiagnosticSeverity::Info, "catalog.item_identity", "A note."));
	v.activity.operation.id = 7;
	v.activity.operation.kind = OperationKind::Build;
	v.activity.operation.done = 3 * 1024 * 1024;
	v.activity.operation.total = 12 * 1024 * 1024;
	v.activity.operation.unit = OperationUnit::Bytes;
	v.activity.operation.label = "Packing localres.pff";
	v.activity.operation.cancellable = true;
	v.activity.operation.reads = HoldsFiles;
	v.activity.operation.writes = HoldsSlot;
	v.activity.status = "menus/a.mnu has no changes to save.";
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	std::string text = logged_frame(ui);
	CHECK(in_order(text, {"File", "Edit", "Build", "Windows", "menus/a.mnu has no changes to save.", "2 unsaved", "7", "5",
	                      "Building 25%", "Cancel", "Build", "Play", "Stop"}),
	      "after the menus: what was said, 2 unsaved, 7 errors, 5 warnings, the build's progress and its Cancel, the buttons");
	// A long line is cut to the room it has, whole in its tooltip.
	v.activity.status = "Opened A Project With A Very Long Name (C:/Users/someone/Documents/OpenNova projects/A Project With A "
	           "Very Long Name That Goes On)";
	v.revisions.touch(ViewConcern::Output);
	text = logged_frame(ui);
	CHECK(text.find("Opened A Project") != std::string::npos && text.find("That Goes On)") == std::string::npos,
	      "a long line cut");
	v.activity.status = "menus/a.mnu has no changes to save.";
	v.revisions.touch(ViewConcern::Output);
	const ImGuiID bar = menu_bar_id();
	const ImGuiID unsaved = item_id(bar, {"status", "unsaved"});
	ui.activate(item_id(bar, {"status", "##unsaved"}));
	text = logged_frame(ui);
	CHECK(in_order(text, {b->path().c_str(), c->path().c_str(), "Save all"}), "the unsaved files listed, then Save all");
	ui.activate(popup_item(unsaved, c->path().c_str()));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == c->path(), "one of them made the active document");
	// The gate's refusals are disabled: no Save all while the build reads the files (an edit goes
	// on).
	CHECK(busy_refuses(EditorRequestKind::SaveAll, v.activity.operation) && !busy_refuses(EditorRequestKind::EditRecord, v.activity.operation),
	      "Save All refused while the build reads the files; an edit is not");
	ui.activate(item_id(bar, {"status", "##unsaved"}));
	ui.activate(popup_item(unsaved, "Save all"));
	CHECK(ui.drain().empty(), "no Save all while the build packs");
	ui.activate(item_id(bar, {"status", "##problems"}));
	ui.frames(3);
	CHECK(GImGui->NavWindow == ImGui::FindWindowByName("Problems"), "a click on the counts shows Problems");
	ui.activate(item_id(bar, {"status", "Build"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::Build) != nullptr, "a Build while one packs: the session joins it");
	ui.activate(item_id(bar, {"status", "Cancel"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CancelOperation) != nullptr, "Cancel: the running operation");

	v.activity.operation = OperationStatus();
	v.revisions.touch(ViewConcern::Operation);
	ui.frames(2);
	ui.activate(item_id(bar, {"status", "##unsaved"}));
	ui.activate(popup_item(unsaved, "Save all"));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::SaveAll) != nullptr, "Save all, once the build is done");
	v.activity.play_state = PlayState::Running;
	v.activity.play_pid = 4242;
	v.revisions.touch(ViewConcern::Operation);
	v.revisions.touch(ViewConcern::Run);
	ui.frames(2);
	CHECK(in_order(logged_frame(ui), {"2 unsaved", "Game running", "Build", "Play", "Stop"}), "the game running");
	ui.activate(item_id(bar, {"status", "Play"}));
	CHECK(ui.drain().empty(), "no Play while it runs");
	ui.activate(item_id(bar, {"status", "Stop"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::StopPlay) != nullptr, "Stop");
	v.activity.play_state = PlayState::Stopped;
	v.activity.has_build = true;
	editor_test::own(v.activity.last_build).ok = true;
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
	v.project.open = true;
	v.project.root = "C:/mods/Files";
	editor_test::own(v.project.document).title = "Files";
	editor_test::own(v.project.scan).entries = { file_entry("items.def", "defs/items.def",
														 AssetKind::ItemDefs),
		file_entry("main.mnu", "menus/main.mnu", AssetKind::Menu),
		file_entry("options.mnu", "menus/sub/options.mnu", AssetKind::Menu),
		file_entry("logo.png", "art/logo.png", AssetKind::ImportSource),
		file_entry("logo.pcx", ".opennova/imported/0a1b/logo.pcx", AssetKind::Texture),
		file_entry("readme.txt", "readme.txt", AssetKind::Text) };
	editor_test::own(v.project.scan).entries[4].imported_from = "art/logo.png";
	editor_test::own(v.project.scan).entries[0].size_bytes = 3 * 1024;
	editor_test::own(v.project.scan).index();
	v.documents.open = {main_menu};
	v.documents.active = main_menu->path();
	v.findings.diagnostics = { editor_test::finding_of(DiagnosticSeverity::Error, "catalog.name_empty", "One.",
									   "defs/items.def"),
		editor_test::finding_of(DiagnosticSeverity::Error, "catalog.item_type", "Two.", "defs/items.def"),
		editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.name_duplicate", "Three.", "defs/items.def") };
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
	// The UX round's project lane: the kind shown, a column of its own.
	CHECK(in_order(text, {"Name", "Kind", "Size", "items.def", "Item def", "3.0 KB"}), "the kind shown");
	// ADR 0046 S15: a file that is the game's own data has its findings counted apart, as Problems
	// counts them: none after its name (its tooltip says them); the modder's again once it is not.
	const OriginalData shipped = editor_test::originals_of(v.findings.diagnostics, {"defs/items.def"});
	v.findings.originals = std::make_shared<const OriginalData>(shipped);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	text = files_text();
	const size_t items_at = text.find("items.def"), size_at = text.find("3.0 KB");
	CHECK(items_at != std::string::npos && size_at != std::string::npos && size_at > items_at &&
	              text.substr(items_at + 9, size_at - items_at - 9).find_first_of("0123456789") == std::string::npos,
	      "the game's own data: no counts after its name");
	v.findings.originals.reset();
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	text = files_text();
	ImGuiTable *files_table = ImGui::TableFindByID(table);
	CHECK(files_table && files_table->ColumnsCount == 3 && files_table->Columns[1].IsEnabled &&
	              files_table->Columns[0].WidthGiven > 2.0f * files_table->Columns[2].WidthGiven &&
	              files_table->Columns[2].WidthGiven >= ImGui::CalcTextSize("999.9 KB").x - 1.0f,
	      "the name has most of the width; the size is as wide as 999.9 KB");
	if (!files_table) return;
	// The kind hidden through the header's menu (a right click on a header).
	const ImGuiTableColumn &size_column = files_table->Columns[2];
	ui.mouse((size_column.MinX + size_column.MaxX) * 0.5f, files_table->OuterRect.Min.y + ImGui::GetFontSize() * 0.5f + 1.0f);
	ui.button(true, 1);
	ui.button(false, 1);
	ui.activate(popup_item(ImHashStr("##ContextMenu", 0, files_table->ID), "Kind"));
	ImGui::ClosePopupsExceptModals();
	ui.away();
	ui.frames(2);
	text = files_text();
	CHECK(!files_table->Columns[1].IsEnabled && text.find("Item def") == std::string::npos,
	      "the kind hidden from the header's menu");

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
	std::vector<EditorRequest> selects = ui.drain();
	// The selection is the session's too (ADR 0046 S18): a click raises its SelectFile.
	CHECK(window->selected() == "menus/main.mnu" && one(selects, EditorRequestKind::SelectFile) &&
	              selects[0].path == "menus/main.mnu",
	      "a click selects it");
	ui.button(true);
	ui.button(false);
	ui.button(true);
	ui.button(false);
	std::vector<EditorRequest> requests = without_selects(ui.drain());
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == "menus/main.mnu", "a double click opens it");
	// A text opens too (S13 D9: a text document); an image source, which no document type opens, is
	// selected, not opened.
	CHECK(hover_find(ui, item_id(table, {"readme.txt", "##row"}), x, top, bottom, row), "readme.txt's row");
	ui.button(true);
	ui.button(false);
	ui.button(true);
	ui.button(false);
	requests = without_selects(ui.drain());
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].path == "readme.txt", "a text file opens");
	CHECK(hover_find(ui, item_id(table, {"art", "art/logo.png", "##row"}), x, top, bottom, row), "logo.png's row");
	ui.button(true);
	ui.button(false);
	ui.button(true);
	ui.button(false);
	CHECK(window->selected() == "art/logo.png" && without_selects(ui.drain()).empty(), "an image source is selected, not opened");

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
	CHECK(one(requests, EditorRequestKind::CreateFile) && requests[0].path == "weapon.def" && requests[0].file_kind == "weapon_defs",
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
	CHECK(one(requests, EditorRequestKind::CreateFile) && requests[0].path == "extra.mnu" && requests[0].file_kind == "menu" &&
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
	CHECK(one(requests, EditorRequestKind::CreateFile) && requests[0].path == "skin.tga" && requests[0].file_kind == "texture" &&
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
	CHECK(renamed && renamed->path == "defs/items.def" && renamed->new_name == "things.def",
	      "Rename...: every file naming it rewritten, or refused");
	// S12 D9: what it would rewrite asked of the session as the name is typed (PreviewRename).
	CHECK(std::any_of(requests.begin(), requests.end(),
	                  [](const EditorRequest &request) {
		                  return request.kind == EditorRequestKind::PreviewRename && request.path == "defs/items.def" &&
		                         request.field.empty() && request.new_name == "things.def";
	                  }),
	      "Rename... previews the rename as the name is typed");

	// S12: a ShowInFiles ask (a RevealFile view event, S13 V4) selects the file and clears a
	// filter that hides it; with the ask's rename, Rename... opens on it.
	ui.frames(2);
	type_into(ui, item_id(files, {"##filter"}), "defs");
	CHECK(files_text().find("options.mnu") == std::string::npos, "the filter hides options.mnu");
	post_event(v, ViewEventKind::RevealFile, "menus/sub/options.mnu");
	ui.frames(3);
	CHECK(window->selected() == "menus/sub/options.mnu" && files_text().find("options.mnu") != std::string::npos,
	      "the file asked for selected, the filter that hid it cleared");
	post_event(v, ViewEventKind::RevealFile, "menus/sub/options.mnu", NodeAddress(), std::string(),
			true);
	ui.frames(3);
	CHECK(logged_frame(ui).find("Rename options.mnu to") != std::string::npos, "Rename... asked on it");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);

	// S14: New > Mission... asks what its blank takes beside its name: a title, and its terrain and
	// its environment among the project's files. With none in the project it says to import one;
	// Create waits for both, then raises create_file with the values by their tokens.
	type_into(ui, item_id(files, {"##filter"}), "");
	ui.drain();
	ui.activate(item_id(files, {"##new"}));
	ui.activate(item_id(pushed(combo, factory_index("", AssetKind::Mission)), {"Mission..."}));
	ui.frames(2);
	CHECK(modal_open("New file") && ui.drain().empty(), "a mission's name asked first");
	text = logged_frame(ui);
	CHECK(in_order(text, {"New file: Mission", "Title", "Terrain", "The project has no terrain: import one first",
	                      "Environment", "The project has no environment: import one first", "Create"}),
	      "the mission's title, terrain and environment asked; none to choose yet");
	editor_test::own(v.project.scan).entries.push_back(file_entry("island.trn", "terrain/island.trn", AssetKind::Terrain));
	editor_test::own(v.project.scan).entries.push_back(file_entry("day.env", "day.env", AssetKind::Environment));
	editor_test::own(v.project.scan).index();
	v.revisions.touch(ViewConcern::Files);
	ui.frames(2);
	type_into(ui, item_id(prompt, {"Name"}), "first.bms");
	ui.activate(item_id(prompt, {"Create"}));
	CHECK(ui.drain().empty() && modal_open("New file"), "Create waits for the terrain and the environment");
	// A combo's list is a window of its own (the first combo open, whatever it is opened from).
	ui.activate(item_id(pushed(prompt, 1), {"Terrain"}));
	ui.activate(item_id(combo, {"island.trn"}));
	ui.frames(2);
	ui.activate(item_id(prompt, {"Create"}));
	CHECK(ui.drain().empty() && modal_open("New file"), "Create still waits for the environment");
	ui.activate(item_id(pushed(prompt, 2), {"Environment"}));
	ui.activate(item_id(combo, {"day.env"}));
	ui.frames(2);
	type_into(ui, item_id(pushed(prompt, 0), {"Title"}), "The first");
	ui.activate(item_id(prompt, {"Create"}));
	requests = ui.drain();
	ui.frames(2);
	using Values = std::vector<std::pair<std::string, std::string>>;
	CHECK(one(requests, EditorRequestKind::CreateFile) && requests[0].path == "first.bms" && requests[0].file_kind == "mission" &&
	              requests[0].values == Values({{"environment", "day.env"}, {"terrain", "island.trn"}, {"title", "The first"}}) &&
	              !modal_open("New file"),
	      "Create: the mission's name and its values by their tokens, sorted as the wire reads them (review F10)");
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
	v.dialogs.import_preview = planned_import("C:/assets");
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
	const std::string stopped = "The plan stopped at " + grouped(kImportPlanFileCap) + " files";
	// S14: the plan in short first, its files and bytes, then each kind with its count and size, the
	// largest first (alike: by token), each a toggle; then the rows, each with its size, by what they
	// come for (the UX round's project lane): each chosen file, the kinds of the files it brings under it.
	CHECK(in_order(text, {"Include the files these need (3 found)", "6 files, 0 B:", "Texture 2 (0 B)", "Animation 1 (0 B)",
	                      "Animation map 1 (0 B)", "Font 1 (0 B)", "Menu 1 (0 B)", "Check shown", "Uncheck shown",
	                      "menu.mnu", "chosen; it brings 3 files", "Font (1 file)", "what menu.mnu names", "arial99.fnt",
	                      "Font", "0 B", "menu.mnu: MAIN/TITLE font.name", "the folder assets", "Texture (2 files)", "logo.tga",
	                      "a_long_texture_name.tga", "CHECK.adm", "walk.bad",
	                      "Not found (1)", "gone.tga", "menu.mnu: MAIN/KEEP/Appearance 1 value",
	                      "arial99.fnt: found in both the folder C:/assets and the game install; using the folder C:/assets",
	                      "The files these kinds name are not looked for yet: Terrain.",
	                      "References that name no file are not followed: sound.",
	                      "Named by the files but defined nowhere: 1 screen.", stopped.c_str(),
	                      "broken.mnu: The file could not be read.", "Replace existing files", "Import 5 files", "Cancel"}),
	      "the plan: the summary, the rows, then what is not found, found twice, not followed, the cap, the finding");
	CHECK(same_line(text, "menu.mnu", "chosen") && same_line(text, "CHECK.adm", "made from walk.o3a, the folder assets"),
	      "a chosen file says so; a converter's output, what it is made from");
	// A kind's line: one check for its files (the one the project cannot take left as it is), a click
	// on its arrow closing it.
	const ImGuiID textures = item_id(pushed(item_id(import_body_id(), {"import_plan"}), 7 + 4), {"##group"});
	ui.activate(textures);
	ui.away();
	CHECK(logged_frame(ui).find("Import 4 files") != std::string::npos, "the Texture line's check: its file left out");
	ui.activate(textures);
	ui.away();
	CHECK(logged_frame(ui).find("Import 5 files") != std::string::npos, "and back");
	ui.activate(item_id(pushed(item_id(import_body_id(), {"import_plan"}), 7 + 4), {"##open"}));
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"Texture (2 files)", "CHECK.adm"}) && text.find("logo.tga") == std::string::npos,
	      "the Texture line closed: its rows hidden");
	ui.activate(item_id(pushed(item_id(import_body_id(), {"import_plan"}), 7 + 4), {"##open"}));
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"Texture (2 files)", "logo.tga", "CHECK.adm"}), "open again");
	const ImGuiID dialog = ImHashStr("Import files");
	// A kind's toggle shows its rows alone; Uncheck shown and Check shown take the shown rows
	// together (one the project cannot take never); the toggle again shows every kind.
	ui.activate(item_id(import_body_id(), {"###kind_texture"}));
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"Texture 2 (0 B)", "logo.tga", "a_long_texture_name.tga", "Not found (1)"}) &&
	              text.find("CHECK.adm") == std::string::npos && text.find("Import 5 files") != std::string::npos,
	      "the texture toggle: the two texture rows alone, the checks kept");
	ui.activate(item_id(import_body_id(), {"Uncheck shown"}));
	ui.away();
	CHECK(logged_frame(ui).find("Import 4 files") != std::string::npos, "Uncheck shown: the shown texture left out");
	ui.activate(item_id(import_body_id(), {"Check shown"}));
	ui.away();
	CHECK(logged_frame(ui).find("Import 5 files") != std::string::npos, "Check shown: the one the project can take back");
	ui.activate(item_id(import_body_id(), {"###kind_texture"}));
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"menu.mnu", "arial99.fnt", "logo.tga", "CHECK.adm", "walk.bad"}), "the toggle again: every kind");
	const auto sources = [](const EditorRequest &request) {
		std::vector<std::string> out;
		for (const ImportChoice &source : request.imports) out.push_back(source.path);
		return out;
	};
	using Paths = std::vector<std::string>;
	ui.activate(item_id(dialog, {"###import"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && !requests[0].replace &&
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
	CHECK(one(requests, EditorRequestKind::SetImportDependencies) && !requests[0].with_dependencies,
	      "the check box asks for the setting");

	// An archive's members to choose from: a choice checked plans the chosen files again.
	v.dialogs.import_preview.choices = { { "C:/assets/data.pff", "main.mnu", false, false },
		{ "C:/assets/data.pff", "stat.mnu", false, false } };
	v.dialogs.import_preview.facts = { { AssetKind::Menu, 2048 }, { AssetKind::Menu, 512 } };
	post_event(v, ViewEventKind::ImportPlanned);
	ui.frames(3);
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"Choose the files to import from the archive data.pff (2 files):", "2 files shown, 0 chosen", "File",
	                      "Kind", "Size", "main.mnu", "Menu", "stat.mnu", "Include the files these need"}) &&
	              text.find("Source") == std::string::npos,
	      "the list to choose from above the plan, each file with its kind and size; no Source where every file "
	      "comes from one place");
	// The filter: a part of a name, or a kind's name.
	type_into(ui, item_id(import_body_id(), {"##filter"}), "stat");
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("1 of 2 files shown") != std::string::npos && text.find("[ ] main.mnu") == std::string::npos &&
	              text.find("[ ] stat.mnu") != std::string::npos,
	      "a part of a name: the files holding it");
	type_into(ui, item_id(import_body_id(), {"##filter"}), "menus");
	ui.away();
	CHECK(logged_frame(ui).find("2 files shown") != std::string::npos, "a kind's name: the files of the kind");
	type_into(ui, item_id(import_body_id(), {"##filter"}), "");
	ui.drain();
	ui.activate(import_table_item("import_choices", 1, "###pick"));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PlanImport) && requests[0].with_dependencies &&
	              sources(requests[0]) == Paths({"C:/assets/menu.mnu", "C:/assets/walk.o3a", "C:/assets/data.pff"}) &&
	              requests[0].imports[2].entry == "stat.mnu",
	      "a choice checked: the files chosen planned again");
	ui.activate(item_id(import_body_id(), {"Select shown"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PlanImport) && requests[0].imports.size() == 4, "Select shown: every listed file");

	// The files changed since the preview: said above the plan.
	v.dialogs.import_preview.choices.clear();
	v.dialogs.import_preview.changed = true;
	post_event(v, ViewEventKind::ImportPlanned, std::string(), NodeAddress(), std::string(), true);
	ui.frames(3);
	ui.away();
	CHECK(logged_frame(ui).find("The files changed since the preview") != std::string::npos, "a changed plan says so");
	// S12: a file with unsaved edits holds nothing here: Import goes to the session, whose guard
	// asks to save only a file the import writes over. While that prompt is open the dialog gives
	// way to it, and comes back as it was, Replace existing files still checked.
	editor_test::TempProjectDir dir("opennova_editor_ui_import_dirty");
	v.documents.open.push_back(edited(menu_at(dir, "a.mnu", "menus/a.mnu")));
	v.revisions.touch(ViewConcern::Documents);
	ui.frames(2);
	ui.drain();
	ui.activate(item_id(dialog, {"Replace existing files"}));
	ui.activate(item_id(dialog, {"###import"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && requests[0].replace,
	      "unsaved edits: Import goes to the session, with Replace existing files");
	v.dialogs.unsaved_prompt.open = true;
	v.dialogs.unsaved_prompt.action = EditorRequestKind::ImportFiles;
	v.dialogs.unsaved_prompt.files = {"menus/a.mnu"};
	v.dialogs.unsaved_prompt.can_discard = false;
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	CHECK(modal_open("Unsaved changes") && !modal_open("Import files") &&
	              logged_frame(ui).find("Save all and import") != std::string::npos,
	      "the unsaved prompt, and not the dialog, while it is open");
	v.dialogs.unsaved_prompt = DialogsView::UnsavedPrompt();
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	ui.drain();
	CHECK(modal_open("Import files") && !modal_open("Unsaved changes"), "the dialog back once the prompt is answered");
	ui.activate(item_id(dialog, {"###import"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && requests[0].replace, "Replace existing files kept");
	// The session closed the preview: the dialog closes.
	v.dialogs.import_preview = DialogsView::ImportPreview();
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
	DialogsView::ImportPreview &preview = v.dialogs.import_preview;
	preview.open = true;
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
	editor_test::own(preview.plan).rows = {menu, texture};
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

// ADR 0046 S14 (review F2): a chosen file the project holds already is held, unchecked by default,
// and the import takes the rest without asking to replace; the row checked alone is replaced (the
// request then replaces), and Replace existing files checks every held row.
void test_import_dialog_held_rows() {
	SessionView v = seeded_view();
	DialogsView::ImportPreview &preview = v.dialogs.import_preview;
	preview.open = true;
	ImportPlanRow fresh;
	fresh.state = ImportPlanRow::State::Selected;
	fresh.selected = true;
	fresh.source = {"C:/game", "hud.mnu", true, false};
	fresh.name = "hud.mnu";
	fresh.kind = AssetKind::Menu;
	fresh.destination = "menus/hud.mnu";
	fresh.found_in = "the game install";
	ImportPlanRow held = fresh;
	held.source = {"C:/game", "items.def", true, false};
	held.name = "items.def";
	held.kind = AssetKind::ItemDefs;
	held.destination = "defs/items.def";
	held.selected = false;
	held.held = true;
	preview.roots = {fresh.source, held.source};
	editor_test::own(preview.plan).rows = {fresh, held};
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	ImGui::SetWindowSize("Import files", ImVec2(1400.0f, 800.0f));
	ui.frames(2);
	const ImGuiID dialog = ImHashStr("Import files");
	CHECK(logged_frame(ui).find("Import 1 file") != std::string::npos && logged_frame(ui).find("the project has it") != std::string::npos,
	      "a held row is unchecked and says the project has it");
	ui.activate(item_id(dialog, {"###import"}));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && requests[0].imports.size() == 1 && requests[0].imports[0].entry == "hud.mnu" &&
	              !requests[0].replace,
	      "Import: the rest, the project's file kept, nothing replaced");
	ui.activate(import_table_item("import_plan", 1, "##take"));
	CHECK(logged_frame(ui).find("Import 2 files") != std::string::npos, "the held row checked alone");
	ui.activate(item_id(dialog, {"###import"}));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::ImportFiles) && requests[0].imports.size() == 2 && requests[0].replace,
	      "a checked held row is one asked to be replaced");
	ui.activate(import_table_item("import_plan", 1, "##take"));
	CHECK(logged_frame(ui).find("Import 1 file") != std::string::npos, "unchecked again");
	ui.activate(item_id(dialog, {"Replace existing files"}));
	CHECK(logged_frame(ui).find("Import 2 files") != std::string::npos, "Replace existing files checks every held row");
	ui.activate(item_id(dialog, {"Replace existing files"}));
	CHECK(logged_frame(ui).find("Import 1 file") != std::string::npos, "and unchecks them");
}

std::string lowered(std::string text) {
	for (char &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

// The Preview window over a real session as the Shell drives it: after each frame the requests
// the windows raised go to the session (kept, for a test to take), and the viewports' devices
// follow the view (the Shell's pump).
struct PreviewRun {
	ProjectSession &session;
	DrawnDevices &devices;
	Ui &ui;
	std::vector<EditorRequest> raised;

	void pump() {
		for (const EditorRequest &request : ui.drain()) {
			raised.push_back(request);
			session.handle(request);
		}
		devices.sync(session.viewports(), session.view());
	}
	// The model's viewport at `path` and its device (null: none).
	const ModelViewport *model(const std::string &path) const {
		return static_cast<const ModelViewport *>(session.viewports().find(path, ViewportKind::Model));
	}
	DrawnDevice *device(const std::string &path) const { return devices.held(path, ViewportKind::Model); }
	// The model's camera set through the session (as the editor MCP sets it).
	void camera(const std::string &path, const char *change) { session.handle(request::set_viewport(path, change)); }
	void settle() {
		for (int i = 0; i < 3; ++i) {
			pump();
			ui.frames(1);
		}
		pump();
	}
	// A document made the active one (as the editor MCP makes it), and the frame after, lower case.
	std::string open(const char *path) {
		session.handle(request::open_document(path));
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
	for (const EditorRequest &request : requests)
		count += request.kind == kind ? 1 : 0;
	return count;
}

// One field of a document set through the session, as the Inspector sets it.
void set_field(ProjectSession &session, const Document &document, const NodeAddress &address, const char *field, Value value) {
	EditorRequest set = request::edit_record(document.path(), Edit());
	set.edits[0].address = address;
	set.edits[0].field = field;
	set.edits[0].value = std::move(value);
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	PreviewRun run{ session, devices, ui, {} };
	const char *const kNothing = "open a menu, a model or an animation to preview it, or select a texture in files.";
	run.settle();
	CHECK(lowered(logged_frame(ui)).find(kNothing) != std::string::npos, "nothing to preview: what to open");

	std::string text = run.open("main.mnu");
	CHECK(text.find("main.mnu - startup") != std::string::npos && text.find("x -, y -") != std::string::npos,
	      "a menu: the menu's viewport, its screen named");
	text = run.open("anims/SKIN.adm");
	CHECK(text.find("skin.adm on skinned.3di") != std::string::npos && text.find("main.mnu - startup") == std::string::npos,
	      "the table: the model pane, the table on the model it plays on");
	text = run.open("anims/walk.bad");
	CHECK(text.find("walk.bad on skinned.3di") != std::string::npos, "a lone clip: on the model its table plays on");
	// What the pane holds while a document it has nothing of to show is active (the UX round's project lane:
	// the pane steps aside then, project_test.cpp's; at this width a table that feeds it too, lacking the
	// room): the kind the view keeps, shown again as such a document goes.
	const DocumentBase *shown_menu = session.document_base_for("main.mnu");
	run.open("menu_style.mns");
	CHECK(v.documents.preview_shown == ViewportKind::Menu && shown_menu &&
	              v.documents.previews[ViewportKind::Menu].path == shown_menu->path(),
	      "the stylesheet: the screen it styles");
	run.open("gametext.bin");
	CHECK(v.documents.preview_shown == ViewportKind::Menu, "a string table: the menu pane too");
	run.open("anims/SKIN.adm");
	run.open("items.def");
	CHECK(v.documents.preview_shown == ViewportKind::Model && v.documents.previews[ViewportKind::Model].path == "anims/SKIN.adm",
	      "a catalog keeps the pane shown");
	CHECK(preview_kind(v.documents, ViewportKind::kCount) == ViewportKind::Menu &&
	              v.documents.preview_shown == ViewportKind::Model,
	      "before it showed anything, with both: the menu's; after the table, still the table's");

	// A family with nothing to show gives way to the other.
	session.handle(request::close_document("anims/SKIN.adm"));
	run.settle();
	CHECK(v.documents.preview_shown == ViewportKind::Menu, "the table closed: the menu's pane");
	run.open("anims/walk.bad");
	run.open("main.mnu");
	run.open("items.def");
	const Document *main_menu = session.document_for("main.mnu");
	CHECK(main_menu != nullptr, "the menu open");
	if (!main_menu) return;
	session.handle(request::close_document(main_menu->path()));
	run.settle();
	CHECK(v.documents.preview_shown == ViewportKind::Model && v.documents.previews[ViewportKind::Model].path == "anims/walk.bad",
	      "the menu closed: the clip's pane");
	run.open("gametext.bin");
	CHECK(v.documents.preview_shown == ViewportKind::Model, "a string table with no menu open: the clip's pane");
	session.handle(request::close_document("anims/walk.bad"));
	run.settle();
	CHECK(v.documents.preview_shown == ViewportKind::kCount, "neither: nothing to show");
	session.handle(request::close_document("gametext.bin"));
	session.handle(request::close_document("items.def"));
	session.handle(request::close_document("menu_style.mns"));
	run.settle();
	CHECK(lowered(logged_frame(ui)).find(kNothing) != std::string::npos, "nothing open: what to open");

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
	const NodeAddress point{row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id};
	Value x;
	CHECK(armory->get(point, "position.x", x) && std::holds_alternative<double>(x), "the user point's x");
	set_field(session, *armory, point, "position.x", std::get<double>(x) + 1.0);
	run.settle();
	CHECK(armory->dirty() && in_order(run.header_tooltip(), {"armory.3di", "models/armory.3di", "Unsaved changes"}),
	      "an unsaved model: its line says so");
	run.open("main.mnu");
	Document *menu = session.document_for("main.mnu");
	NodeAddress title;
	CHECK(menu && find_definition(AssetGraph(), *menu, "TITLE", title), "the menu's title");
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	PreviewRun run{ session, devices, ui, {} };
	run.open("main.mnu");
	run.open("models/armory.3di");
	const auto *armory =
			dynamic_cast<const ModelDocument *>(session.document_for("models/armory.3di"));
	const ModelViewport *model = run.model("models/armory.3di");
	CHECK(armory && armory->model_row() && model && model->status() == ViewportStatus::Ready,
			"armory previewed");
	if (!armory || !armory->model_row() || !model)
		return;
	const ModelRow &row = *armory->model_row();
	EditorRequest select = request::select_record(
			armory->path(), { row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id });
	session.handle(select);
	ui.focus("Preview");
	run.settle();

	// F: the selected marker framed while the model's view shows (a SetViewport of its camera);
	// nothing while the menu's shows.
	const auto press_f = [&]() {
		ui.key(ImGuiKey_F, true);
		ui.key(ImGuiKey_F, false);
		run.pump();
	};
	run.camera(armory->path(), R"({"camera": {"distance": 40}})");
	press_f();
	CHECK(model->camera().distance != 40.0f, "F frames the selected marker");
	run.open("main.mnu");
	ui.focus("Preview");
	run.camera(armory->path(), R"({"camera": {"distance": 40}})");
	press_f();
	CHECK(model->camera().distance == 40.0f, "the model's view hidden: F leaves its camera");

	// A drag of the selected marker (Alt: placed freely), the menu made active mid-drag.
	run.open("models/armory.3di");
	ui.focus("Preview");
	run.settle();
	press_f(); // the marker framed, in the middle of the picture
	run.settle();
	run.take();
	const ModelOverlay *marker = nullptr;
	const std::vector<ModelOverlay> overlays = model->overlays(session.viewports().clock());
	for (const ModelOverlay &overlay : overlays)
		if (overlay.kind == ModelOverlayKind::UserPoint && overlay.index == 0)
			marker = &overlay;
	float x = 0.0f, y = 0.0f;
	const DrawnDevice *device = run.device(armory->path());
	CHECK(marker && device &&
					model->camera().project(marker->at, model->size().width, model->size().height, x, y),
			"the marker on the picture");
	if (!marker || !device)
		return;
	CHECK(device->width == model->size().width && device->height == model->size().height && model->canvas_sized(),
			"the viewport's size is the canvas's (its device's as drawn, reported at the pump)");
	const ImVec2 at(device->origin.x + x, device->origin.y + y);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
	ui.mouse(at.x, at.y);
	ui.button(true);
	ui.mouse(at.x + 30.0f, at.y + 10.0f);
	run.pump();
	std::vector<EditorRequest> requests = run.take();
	const EditorRequest *step = only(requests, EditorRequestKind::EditRecord);
	CHECK(step && step->path == armory->path() &&
					count_of_kind(requests, EditorRequestKind::EndEdit) == 0,
			"the drag's first step");
	session.handle(request::open_document("main.mnu"));
	run.settle();
	requests = run.take();
	const EditorRequest *end = only(requests, EditorRequestKind::EndEdit);
	CHECK(end && end->path == armory->path() &&
					count_of_kind(requests, EditorRequestKind::EditRecord) == 0,
			"the menu made active mid-drag: the drag's one end, for the model");
	ui.mouse(at.x + 60.0f, at.y + 20.0f);
	ui.button(false);
	run.settle();
	CHECK(run.take().empty(), "letting go raises nothing");
}

// The model's view's pointer through ImGui over a real session, each gesture a SetViewport of its
// camera and nothing else: the middle button drags the camera's target (a pan), a wheel notch over
// the picture dollies it (kModelWheelDolly of the distance), a double-click frames the selected
// marker.
void test_preview_model_pane_input() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_model_input");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	PreviewRun run{ session, devices, ui, {} };
	run.open("models/armory.3di");
	const auto *armory =
			dynamic_cast<const ModelDocument *>(session.document_for("models/armory.3di"));
	const ModelViewport *model = run.model("models/armory.3di");
	CHECK(armory && armory->model_row() && model && model->status() == ViewportStatus::Ready,
			"armory previewed");
	if (!armory || !armory->model_row() || !model)
		return;
	const ModelRow &row = *armory->model_row();
	EditorRequest select = request::select_record(
			armory->path(), { row.id, node_kind(ModelKind::UserPoint), row.ids.lists[3][0].id });
	session.handle(select);
	ui.focus("Preview");
	run.settle();
	run.take();
	const DrawnDevice *device = run.device(armory->path());
	CHECK(device != nullptr, "the model's device");
	if (!device)
		return;
	// A point on the picture away from every marker: its top-left corner.
	const ImVec2 corner(device->origin.x + 12.0f, device->origin.y + 12.0f);

	// The middle button drags the camera's target.
	const PreviewVec3 target = model->camera().target;
	ui.mouse(corner.x, corner.y);
	ui.button(true, 2);
	ui.mouse(corner.x + 40.0f, corner.y + 10.0f);
	ui.mouse(corner.x + 80.0f, corner.y + 20.0f);
	ui.button(false, 2);
	run.settle();
	const PreviewVec3 panned = model->camera().target;
	CHECK(panned.x != target.x || panned.y != target.y || panned.z != target.z,
			"the middle button pans the camera");

	// A wheel notch over the picture dollies the camera toward its target.
	const float distance = model->camera().distance;
	ui.mouse(corner.x, corner.y);
	ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
	ui.frames(2);
	run.settle();
	CHECK(std::fabs(model->camera().distance - distance * kModelWheelDolly) < 1e-3f,
			"a wheel notch dollies the camera");

	// A double-click frames the selected marker.
	run.camera(armory->path(), R"({"camera": {"distance": 40}})");
	ui.mouse(corner.x, corner.y);
	ui.button(true);
	ui.button(false);
	ui.button(true);
	ui.button(false);
	run.settle();
	CHECK(model->camera().distance != 40.0f, "a double-click frames the selected marker");
	const std::vector<EditorRequest> raised = run.take();
	CHECK(!raised.empty() && count_of_kind(raised, EditorRequestKind::SetViewport) == raised.size(),
			"the camera's gestures raise SetViewports of it, nothing else");
}

// The mission's view in the Document window over a real session, its devices the Shell's (ADR 0046
// S14): the toolbar's Frame and Top each raise one EditInViewport (the session plans the camera's
// SetViewport over its own context, as for Ground) and nothing else; a marquee over the picture (the areas' marks off) is one SelectRecord of every entity in the box, across the
// pools, which the session holds and the Inspector shows as their shared form (E9), where a change
// of Team is one batch over every one of them, ended once.
void test_mission_view_input() {
	editor_test::TempProjectDir dir("opennova_editor_ui_mission_view_input");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/missions/synth_logic.bms",
	                               test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")),
	      "the mission written");
	session.handle(request::rescan());
	session.run_operations();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	PreviewRun run{ session, devices, ui, {} };
	run.open("missions/synth_logic.bms");
	const Document *mission = session.document_for("missions/synth_logic.bms");
	CHECK(mission != nullptr, "the mission open");
	if (!mission) return;
	const std::string path = mission->path();
	ui.focus("Document");
	run.settle();
	run.take();
	const auto *viewport = static_cast<const MissionViewport *>(session.viewports().find(path, ViewportKind::Mission));
	const DrawnDevice *device = devices.held(path, ViewportKind::Mission);
	CHECK(viewport && viewport->status() == ViewportStatus::Ready && device && device->draws > 0 && device->width > 0,
	      "the mission drawn in its tab through its device");
	if (!viewport || !device) return;
	// The viewport's column beside the outline, whose items the toolbar's buttons are.
	const ImGuiWindow *column = nullptr;
	for (const ImGuiWindow *window : GImGui->Windows)
		if (window->Active && !window->Hidden && std::strstr(window->Name, "/viewport_column_")) column = window;
	CHECK(column != nullptr, "the viewport column beside the outline, drawn");
	if (!column) return;
	const auto press = [&](const char *button) {
		ui.activate(item_id(column->ID, { button }));
		run.settle();
		return run.take();
	};
	run.camera(path, R"({"kind": "mission", "camera": {"distance": 5000}})");
	std::vector<EditorRequest> raised = press("Frame");
	CHECK(raised.size() == 1 && raised[0].kind == EditorRequestKind::EditInViewport && viewport->camera().distance < 5000.0f,
	      "Frame: one EditInViewport the session plans (a SetViewport of the camera), the entities framed");
	raised = press("Top");
	CHECK(raised.size() == 1 && raised[0].kind == EditorRequestKind::EditInViewport &&
	              viewport->camera().pitch >= kOrbitPitchLimit - 1e-4f,
	      "Top: one EditInViewport, the camera straight down");
	raised = press("Frame");
	CHECK(raised.size() == 1 && raised[0].kind == EditorRequestKind::EditInViewport, "framed again");
	// The areas' marks off: a box over the whole picture takes the entities alone.
	run.camera(path, R"({"kind": "mission", "options": {"marks": {"areas": false}}})");
	run.settle();
	run.take();
	const ImVec2 from(device->origin.x + 2.0f, device->origin.y + 2.0f);
	const ImVec2 to(device->origin.x + float(device->width) - 2.0f, device->origin.y + float(device->height) - 2.0f);
	ui.mouse(from.x, from.y);
	ui.button(true);
	ui.mouse((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f);
	ui.mouse(to.x, to.y);
	ui.button(false);
	run.settle();
	raised = run.take();
	const EditorRequest *boxed = only(raised, EditorRequestKind::SelectRecord);
	std::set<NodeKind> kinds;
	if (boxed)
		for (const NodeAddress &record : boxed->records) kinds.insert(record.kind);
	CHECK(boxed && boxed->records.size() >= 4 && kinds.size() >= 2 && count_of_kind(raised, EditorRequestKind::EditRecord) == 0,
	      "a marquee over the picture: one SelectRecord of the entities in it, across the pools, no edit");
	if (!boxed) return;
	const size_t selected = v.documents.selection.records.size();
	CHECK(selected == boxed->records.size() && v.documents.selection.document == path, "the session holds them");
	// The Inspector's shared form over them: Team changed there is one batch over every record.
	ui.focus("Inspector");
	ui.frames(3);
	const std::string text = logged_frame(ui);
	CHECK(text.find("records selected (") != std::string::npos && text.find("Team") != std::string::npos,
	      "the Inspector's shared form over the pools");
	run.take();
	const ImGuiID team = item_id(Ui::window_id("Inspector"), { "", "fields", "team", "##value" });
	ImGui::ActivateItemByID(team);
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	CHECK(GImGui->ActiveId == team, "the Team field has the keyboard");
	ImGui::GetIO().AddInputCharactersUTF8("1");
	ui.frames(2);
	ui.key(ImGuiKey_Enter, true);
	ui.key(ImGuiKey_Enter, false);
	run.settle();
	raised = run.take();
	const EditorRequest *batch = nullptr;
	for (const EditorRequest &request : raised)
		if (request.kind == EditorRequestKind::EditRecord && request.edits.size() == selected) batch = &request;
	bool teams = batch != nullptr;
	for (size_t i = 0; batch && i < batch->edits.size(); ++i)
		teams = teams && batch->edits[i].field == "team" && v.documents.selection.holds(batch->edits[i].address);
	CHECK(batch && teams && count_of_kind(raised, EditorRequestKind::EndEdit) >= 1 && mission->dirty(),
	      "a change of Team: one batch over every selected record, ended, applied");
}

// The mission's view over a device that answers a ground (z = 3 + x / 10; S14 review M2, M8), the
// session's viewports given the same devices: the canvas reads the device from its first frame, so a
// drag of an entity with Stick on (the default) keeps its height over that ground where it goes, and
// an area's mark stands on the ground, where a click selects it; the toolbar's Ground, an entity
// selected and lifted off the ground, is one EditInViewport the session plans over its own context,
// the entity set down on the ground.
void test_mission_view_ground() {
	editor_test::TempProjectDir dir("opennova_editor_ui_mission_view_ground");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/missions/synth_logic.bms",
	                               test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")),
	      "the mission written");
	session.handle(request::rescan());
	session.run_operations();
	DrawnDevices devices;
	session.viewports().set_devices(&devices.cache);
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	PreviewRun run{ session, devices, ui, {} };
	run.open("missions/synth_logic.bms");
	const Document *mission = session.document_for("missions/synth_logic.bms");
	CHECK(mission != nullptr, "the mission open");
	if (!mission) return;
	const std::string path = mission->path();
	ui.focus("Document");
	run.settle();
	DrawnDevice *device = devices.held(path, ViewportKind::Mission);
	CHECK(device != nullptr, "the mission's device");
	if (!device) return;
	const auto ground = [](double x, double) { return 3.0 + x / 10.0; };
	device->ground = ground;
	run.settle();
	run.take();
	const auto *viewport = static_cast<const MissionViewport *>(session.viewports().find(path, ViewportKind::Mission));
	CHECK(viewport && viewport->ground() && device->width > 0, "the device's ground reported, the picture drawn");
	if (!viewport) return;
	const auto over = [&](const MissionEntityMark &entity) { return entity.z - ground(entity.x, entity.y); };
	// An item's mark the canvas takes at its pixel (the marks as the device's ground places them).
	std::vector<MissionMark> marks = viewport->marks(device->width, device->height, device);
	int item = -1;
	for (size_t i = 0; i < marks.size() && item < 0; ++i)
		if (marks[i].shown && std::string(marks[i].kind) == "item" && pick_mission_mark(marks, viewport->camera(), device->width, device->height, marks[i].x, marks[i].y, nullptr, MissionPick::Press) == int(i))
			item = int(i);
	CHECK(item >= 0, "an item's mark on the picture");
	if (item < 0) return;
	const NodeAddress record = marks[size_t(item)].record;
	const double clearance = over(*viewport->scene().entity(record.row)), x0 = viewport->scene().entity(record.row)->x;
	// Dragged 60 pixels across, Stick on.
	const ImVec2 from(device->origin.x + marks[size_t(item)].x, device->origin.y + marks[size_t(item)].y);
	ui.mouse(from.x, from.y);
	ui.button(true);
	ui.mouse(from.x + 20.0f, from.y);
	ui.mouse(from.x + 40.0f, from.y);
	ui.mouse(from.x + 60.0f, from.y);
	ui.button(false);
	run.settle();
	run.take();
	const MissionEntityMark *moved = viewport->scene().entity(record.row);
	CHECK(moved && std::fabs(moved->x - x0) > 1.0 && std::fabs(over(*moved) - clearance) < 1e-3,
	      "a drag with Stick: the entity's height over the device's ground kept where it went");
	// An area's mark on the ground at its middle: a click there selects it.
	marks = viewport->marks(device->width, device->height, device);
	bool area = false;
	for (size_t i = 0; i < marks.size() && !area; ++i) {
		if (marks[i].area < 0 || !marks[i].shown || pick_mission_mark(marks, viewport->camera(), device->width, device->height, marks[i].x, marks[i].y, nullptr, MissionPick::Press) != int(i)) continue;
		ui.mouse(device->origin.x + marks[i].x, device->origin.y + marks[i].y);
		ui.button(true);
		ui.button(false);
		run.settle();
		run.take();
		CHECK(v.documents.selection.primary == marks[i].record, "a click at the area's mark on the ground selects it");
		area = true;
	}
	CHECK(area, "an area's mark on the picture");
	// Ground: the item selected and lifted 25 m, the toolbar's Ground sets it down.
	session.handle(request::select_record(path, record));
	set_field(session, *mission, record, "z", 25.0 + ground(moved->x, moved->y));
	run.settle();
	run.take();
	const ImGuiWindow *column = nullptr;
	for (const ImGuiWindow *window : GImGui->Windows)
		if (window->Active && !window->Hidden && std::strstr(window->Name, "/viewport_column_")) column = window;
	CHECK(column != nullptr, "the viewport column beside the outline, drawn");
	if (!column) return;
	ui.activate(item_id(column->ID, { "Ground" }));
	run.settle();
	const std::vector<EditorRequest> raised = run.take();
	CHECK(count_of_kind(raised, EditorRequestKind::EditInViewport) == 1, "Ground: one EditInViewport");
	const MissionEntityMark *grounded = viewport->scene().entity(record.row);
	CHECK(grounded && std::fabs(grounded->z - ground(grounded->x, grounded->y)) < 1e-3, "the entity set down on the ground");
}

// Placing and tweaking in the mission's view (ADR 0046 S15), over a real session: Place opens the
// palette beside the picture (the project's item by name in its group), a row picked there and a
// click on the picture place one of it (one EditInViewport, the new entity selected), the tool kept
// for the next; Esc goes back to Select; the line under the picture says what a click does; the
// primary's place typed in the East field moves it there (one batch); the right button's click on a
// mark opens the menu of what applies, whose Select same item selects every entity of its item.
void test_mission_view_placing() {
	editor_test::TempProjectDir dir("opennova_editor_ui_mission_view_placing");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/missions/synth_logic.bms",
	                               test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")),
	      "the mission written");
	session.handle(request::rescan());
	session.run_operations();
	DrawnDevices devices;
	session.viewports().set_devices(&devices.cache);
	Ui ui;
	// A wide display, so the lines this test reads are read whole (the Preview steps aside for the
	// mission: the Document tab has the centre).
	ImGui::GetIO().DisplaySize = ImVec2(4096.0f, 1600.0f);
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	PreviewRun run{ session, devices, ui, {} };
	run.open("missions/synth_logic.bms");
	const Document *mission = session.document_for("missions/synth_logic.bms");
	CHECK(mission != nullptr, "the mission open");
	if (!mission) return;
	const std::string path = mission->path();
	ui.focus("Document");
	run.settle();
	run.take();
	const auto *viewport = static_cast<const MissionViewport *>(session.viewports().find(path, ViewportKind::Mission));
	const auto column_window = [] {
		const ImGuiWindow *column = nullptr;
		for (const ImGuiWindow *window : GImGui->Windows)
			if (window->Active && !window->Hidden && std::strstr(window->Name, "/viewport_column_")) column = window;
		return column;
	};
	const ImGuiWindow *column = column_window();
	CHECK(viewport && column, "the mission's view drawn");
	if (!viewport || !column) return;
	std::string text = logged_frame(ui);
	CHECK(text.find("Click a mark to select it") != std::string::npos, "the line under the picture: what a click does");
	// Place: the palette beside the picture, the project's item by name in its group.
	ui.activate(item_id(column->ID, { "Place" }));
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("Buildings (1)") != std::string::npos && text.find("Skinned Thing") != std::string::npos,
	      "the palette: Buildings, Skinned Thing");
	CHECK(text.find("Place: pick an item") != std::string::npos, "the line: pick an item first");
	const ImGuiWindow *items = nullptr;
	for (const ImGuiWindow *window : GImGui->Windows)
		if (window->Active && !window->Hidden && std::strstr(window->Name, "/palette_items")) items = window;
	CHECK(items != nullptr, "the palette's rows drawn");
	if (!items) return;
	ui.activate(item_id(pushed(pushed(items->ID, int(MissionPaletteGroup::Buildings)), 0), { "###item" }));
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("Place Skinned Thing: click the ground") != std::string::npos, "the line: placing the picked item");
	// A click on the picture places one there, the new building selected; the tool stays.
	DrawnDevice *device = devices.held(path, ViewportKind::Mission);
	CHECK(device != nullptr && device->width > 0, "the picture drawn");
	if (!device) return;
	const size_t buildings = viewport->scene().count(MissionPool::Building);
	run.take();
	ui.click(ImVec2(device->origin.x + float(device->width) * 0.5f, device->origin.y + float(device->height) * 0.5f));
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	CHECK(count_of_kind(raised, EditorRequestKind::EditInViewport) == 1 && viewport->scene().count(MissionPool::Building) == buildings + 1,
	      "a click places one");
	const MissionEntityMark *placed = viewport->scene().entity(v.documents.selection.primary.row);
	CHECK(placed && placed->item == 100200, "the placed building selected");
	CHECK(v.project.recent_items == std::vector<int64_t>({ 100200 }), "the item among the recently placed");
	// Esc: back to Select.
	ui.key(ImGuiKey_Escape, true);
	ui.key(ImGuiKey_Escape, false);
	run.settle();
	text = logged_frame(ui);
	CHECK(text.find("Buildings (1)") == std::string::npos && text.find("Place Skinned Thing") == std::string::npos,
	      "Esc: the palette gone, Select again");
	// The East field: the placed building moved to 123.5 m east, one batch.
	column = column_window();
	CHECK(column != nullptr && placed, "the column again");
	if (!column || !placed) return;
	const ImGuiID east = item_id(column->ID, { "###east" });
	run.take();
	ImGui::ActivateItemByID(east);
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	CHECK(GImGui->ActiveId == east, "the East field has the keyboard");
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
	ui.frames(1);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
	ui.frames(1);
	ImGui::GetIO().AddInputCharactersUTF8("123.5");
	ui.frames(2);
	ui.key(ImGuiKey_Enter, true);
	ui.key(ImGuiKey_Enter, false);
	run.settle();
	raised = run.take();
	const MissionEntityMark *moved = viewport->scene().entity(v.documents.selection.primary.row);
	CHECK(count_of_kind(raised, EditorRequestKind::EditRecord) == 1 && moved && std::fabs(moved->x - 123.5) < 1e-3,
	      "East typed: the building at 123.5 m east, one batch");
	// The right button's click on a pump's mark: its menu; Select same item selects every pump.
	std::vector<MissionMark> marks = viewport->marks(device->width, device->height, device);
	int pump = -1;
	for (size_t i = 0; i < marks.size() && pump < 0; ++i)
		if (marks[i].shown && marks[i].entity >= 0 && viewport->scene().entities()[size_t(marks[i].entity)].item == 106100 &&
				pick_mission_mark(marks, viewport->camera(), device->width, device->height, marks[i].x, marks[i].y, nullptr, MissionPick::Press) == int(i))
			pump = int(i);
	CHECK(pump >= 0, "a pump's mark on the picture");
	if (pump < 0) return;
	ui.mouse(device->origin.x + marks[size_t(pump)].x, device->origin.y + marks[size_t(pump)].y);
	ui.button(true, 1);
	ui.button(false, 1);
	run.settle();
	CHECK(v.documents.selection.primary == marks[size_t(pump)].record, "the right button selects the mark under it");
	text = logged_frame(ui);
	CHECK(in_order(text, { "Paste here", "Frame", "Drop to ground", "Duplicate", "Delete", "Select same item", "Go to in outline",
	                         "Show events using this" }),
	      "the menu of what applies");
	column = column_window();
	if (!column) return;
	ui.activate(popup_item(item_id(column->ID, { "mission_canvas_menu" }), "Select same item"));
	run.settle();
	CHECK(v.documents.selection.records.size() == 3, "Select same item: the three pumps");
	// Ctrl+V over the picture with an event on the clipboard (nothing with a place to paste at): the
	// session's own paste, never Paste here's refusal (S15 review).
	const auto *placed_in = static_cast<const MissionDocument *>(session.document_for(path));
	const std::vector<const Node *> events = placed_in ? placed_in->rows_of(MissionKind::Event) : std::vector<const Node *>();
	CHECK(!events.empty(), "the mission has events");
	if (events.empty()) return;
	session.handle(request::select_record(path, { events.front()->id, events.front()->kind, 0 }));
	EditorRequest copy = request::of(EditorRequestKind::Copy);
	copy.path = path;
	session.handle(copy);
	double middle[2];
	CHECK(!v.documents.clipboard.empty() && !mission_clip_middle(v.documents.clipboard, middle), "an event copied");
	run.settle();
	// The picture clicked (its corner, the sky: the view has the keyboard), the pointer over its middle.
	ui.click(ImVec2(device->origin.x + 3.0f, device->origin.y + 3.0f));
	run.settle();
	run.take();
	ui.mouse(device->origin.x + float(device->width) * 0.5f, device->origin.y + float(device->height) * 0.5f);
	ui.frames(1);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ui.key(ImGuiKey_V, true);
	ui.key(ImGuiKey_V, false);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
	run.settle();
	raised = run.take();
	CHECK(count_of_kind(raised, EditorRequestKind::Paste) == 1 && count_of_kind(raised, EditorRequestKind::EditInViewport) == 0,
	      "Ctrl+V with an event: the session's paste");
}

// The Preview steps aside for a mission (ADR 0046 S15), over a real session at the first layout:
// nothing open, Preview draws beside Document (what to open); the mission made active with nothing
// to preview, Preview is not drawn (its Windows item still open), its node hides and Document takes
// the whole centre, the mission's picture the main view; a menu opened, Preview is back in the node
// it left and Document has its share again; the mission active again, Preview steps aside again (the
// UX round's project lane), though it has the menu to show.
void test_preview_steps_aside() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_steps_aside");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(preview_project(session, dir), "the preview project");
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/missions/synth_logic.bms",
	                               test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")),
	      "the mission written");
	session.handle(request::rescan());
	session.run_operations();
	DrawnDevices devices;
	session.viewports().set_devices(&devices.cache);
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	PreviewRun run{ session, devices, ui, {} };
	const devtools::Window *preview_window = find_window(ui.windows.pass(), "Preview");
	CHECK(preview_window != nullptr, "the Preview window");
	if (!preview_window) return;
	const auto centre = [] {
		const ImGuiWindow *files = ImGui::FindWindowByName("Files");
		const ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
		return files && inspector ? inspector->Pos.x - (files->Pos.x + files->Size.x) : 0.0f;
	};
	run.settle();
	ImGuiWindow *preview = ImGui::FindWindowByName("Preview");
	ImGuiWindow *document = ImGui::FindWindowByName("Document");
	CHECK(preview && preview->Active && document && centre() > 0.0f, "nothing open: Preview drawn beside Document");
	if (!preview || !document) return;
	const ImGuiID node = preview->DockId;
	CHECK(node != 0 && document->Size.x < centre() * 0.5f, "Document its share of the centre, Preview the rest");
	CHECK(!preview_stands_aside(v), "with no mission active the Preview stands");
	run.open("missions/synth_logic.bms");
	ui.frames(3);
	CHECK(preview_stands_aside(v) && !preview->Active && preview_window->open,
	      "a mission, nothing to preview: Preview steps aside, still open in the Windows menu");
	const ImGuiDockNode *left = ImGui::DockBuilderGetNode(node);
	CHECK(left && !left->IsVisible && left->Windows.Size == 0, "the node it left kept, empty and hidden");
	// The whole centre but the separators between the docks.
	CHECK(std::fabs(document->Size.x - centre()) < 2.0f * ImGui::GetStyle().DockingSeparatorSize + 1.0f,
	      "Document the whole centre");
	run.open("main.mnu");
	ui.frames(3);
	CHECK(preview->Active && preview->DockId == node && document->Size.x < centre() * 0.5f,
	      "a menu opened: Preview back in its node, Document its share");
	// The UX round's project lane: the Preview steps aside for a document it has nothing of to show, whatever
	// else it could show (S15 kept the menu beside the mission).
	run.open("missions/synth_logic.bms");
	ui.frames(3);
	CHECK(preview_stands_aside(v) && !preview->Active && v.documents.preview_shown == ViewportKind::Menu,
	      "the mission again: Preview steps aside, though it has the menu to show");
	// The author's ask (S15 review): the menu closed, the Preview steps aside for the mission again;
	// ticked in the Windows menu (show_anyway), it shows beside that mission until another document is
	// made active.
	devtools::Window *preview_item = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count(); ++i)
		if (std::strcmp(ui.windows.pass().window(i).title(), "Preview") == 0) preview_item = &ui.windows.pass().window(i);
	CHECK(preview_item != nullptr, "the Preview's window");
	if (!preview_item) return;
	const DocumentBase *menu = session.document_base_for("main.mnu");
	if (menu) session.handle(request::close_document(menu->path()));
	run.settle();
	ui.frames(3);
	CHECK(preview_item->stands_aside() && !preview->Active, "the menu closed: aside again");
	preview_item->show_anyway();
	ui.frames(3);
	CHECK(!preview_item->stands_aside() && preview->Active, "asked for: shown beside the mission");
	run.open("items.def");
	run.open("missions/synth_logic.bms");
	ui.frames(3);
	CHECK(preview_item->stands_aside() && !preview->Active, "another document made active between: aside again");
	// Floated off the dockspace (another monitor), it never steps aside: it frees no room.
	preview_item->show_anyway();
	ui.frames(3);
	ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), preview);
	ui.frames(3);
	CHECK(preview->DockId == 0, "the Preview floated");
	run.open("items.def");
	run.open("missions/synth_logic.bms");
	ui.frames(3);
	CHECK(!preview_item->stands_aside() && preview->Active, "floated: it stays beside the mission");
}

// A canvas whose picture fills it (the model's, the mission's), read through ImGui: the right
// button is apart from a press (right_pressed, right_down; never pressed or down), a click of it
// only when it comes up having travelled less than a drag; the left button is a press; the keys a
// camera flies by held (W forward, D right, E up; none with Ctrl, a chord's letter), Shift fast,
// Delete, PgUp; the frame's time; and a line of text among the shapes is drawn.
void test_canvas_fill_input() {
	NullBackend backend;
	ViewportCanvas canvas;
	CanvasInput in;
	bool right_clicked = false;
	int text_vertices = 0;
	const auto frame = [&]() {
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
		ImGui::Begin("canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
		in = CanvasInput();
		right_clicked = false;
		if (canvas.begin(200.0f, 0, 0)) {
			canvas.picture(
					[](const ViewportPicture &picture) {
						ImGui::Dummy(ImVec2(float(picture.width), float(picture.height)));
					},
					nullptr);
			in = canvas.input();
			right_clicked = canvas.right_clicked();
			OverlayList shapes;
			const int before = ImGui::GetWindowDrawList()->VtxBuffer.Size;
			shapes.text(CanvasPoint{ 4.0f, 4.0f }, "label");
			canvas.draw(shapes, CanvasCursor::Default);
			text_vertices = ImGui::GetWindowDrawList()->VtxBuffer.Size - before;
		}
		canvas.end();
		ImGui::End();
		ImGui::Render();
		canvas.end_frame();
	};
	ImGuiIO &io = ImGui::GetIO();
	const auto mouse = [&](float x, float y) {
		io.AddMousePosEvent(x, y);
		frame();
	};
	const auto button = [&](int which, bool down) {
		io.AddMouseButtonEvent(which, down);
		frame();
	};
	const auto key = [&](ImGuiKey which, bool down) {
		io.AddKeyEvent(which, down);
		frame();
	};
	frame();
	frame();
	CHECK(in.width > 0 && in.height == 200 && text_vertices > 0, "the canvas draws, its text too");
	CHECK(in.dt == io.DeltaTime && in.dt > 0.0f, "the frame's time");

	// The right button: a look, never a press.
	mouse(100.0f, 100.0f);
	button(1, true);
	CHECK(in.hovered && in.right_pressed && in.right_down && !in.pressed && !in.down && !in.middle,
			"the right button down: apart from a press");
	mouse(140.0f, 110.0f);
	CHECK(!in.right_pressed && in.right_down && !in.down && in.delta.x == 40.0f && in.delta.y == 10.0f,
			"held and moved");
	button(1, false);
	CHECK(!in.right_down && !right_clicked, "let go after a drag: no click");
	button(1, true);
	CHECK(in.right_pressed && !right_clicked, "pressed again");
	button(1, false);
	CHECK(!in.right_down && right_clicked, "let go where it went down: a click");
	frame();
	CHECK(!right_clicked, "a click once");

	// The left button: a press, as before.
	button(0, true);
	CHECK(in.pressed && in.down && !in.right_pressed && !in.right_down, "the left button: a press");
	button(0, false);
	CHECK(!in.pressed && !in.down, "let go");

	// The keys, while the canvas's window has the keyboard.
	CHECK(in.keyboard.focused, "the canvas's window has the keyboard");
	key(ImGuiKey_W, true);
	key(ImGuiKey_D, true);
	key(ImGuiKey_E, true);
	CHECK(in.keyboard.move_z == 1 && in.keyboard.move_x == 1 && in.keyboard.move_y == 1 &&
					!in.keyboard.fast,
			"W, D and E held: forward, right, up");
	key(ImGuiMod_Shift, true);
	CHECK(in.keyboard.fast && in.keyboard.move_z == 1, "Shift: fast");
	key(ImGuiMod_Shift, false);
	key(ImGuiMod_Ctrl, true);
	CHECK(in.keyboard.move_z == 0 && in.keyboard.move_x == 0 && in.keyboard.move_y == 0,
			"with Ctrl a letter is a chord's, not the camera's");
	key(ImGuiMod_Ctrl, false);
	key(ImGuiKey_W, false);
	key(ImGuiKey_D, false);
	key(ImGuiKey_E, false);
	key(ImGuiKey_S, true);
	key(ImGuiKey_A, true);
	key(ImGuiKey_Q, true);
	CHECK(in.keyboard.move_z == -1 && in.keyboard.move_x == -1 && in.keyboard.move_y == -1,
			"S, A and Q held: back, left, down");
	key(ImGuiKey_S, false);
	key(ImGuiKey_A, false);
	key(ImGuiKey_Q, false);
	CHECK(in.keyboard.move_z == 0 && in.keyboard.move_x == 0 && in.keyboard.move_y == 0, "let go");
	key(ImGuiKey_Delete, true);
	CHECK(in.keyboard.remove, "Delete pressed");
	key(ImGuiKey_Delete, false);
	CHECK(!in.keyboard.remove, "once");
	key(ImGuiKey_PageUp, true);
	CHECK(in.keyboard.page == 1, "PgUp");
	key(ImGuiKey_PageUp, false);
	key(ImGuiKey_PageDown, true);
	CHECK(in.keyboard.page == -1, "PgDn");
	key(ImGuiKey_PageDown, false);
}

// The OS window's title: the product, the project's name before it, a bullet while a file has
// unsaved changes.
void test_window_title() {
	SessionView v;
	CHECK(editor_window_title(v) == "OpenNova Editor", "no project");
	v.project.open = true;
	editor_test::own(v.project.document).title = "Armory";
	CHECK(editor_window_title(v) == "Armory - OpenNova Editor", "a project");
	editor_test::TempProjectDir dir("opennova_editor_ui_title_test");
	const auto menu = load_menu(dir);
	v.documents.open = {menu};
	CHECK(editor_window_title(v) == "Armory - OpenNova Editor", "every file saved");
	edited(menu);
	CHECK(editor_window_title(v) == "Armory \xE2\x97\x8F - OpenNova Editor", "a file with unsaved changes");
}

} // namespace

// S13 D1: Files makes its tree and each file's counts again only when what they read moves (the
// files, the findings): a line of Output, the status line and every step of a build leave them
// as they were; a finding about a file, the files as they were, makes them again, and so does a
// file the scan finds anew.
void test_files_tree_kept() {
	editor_test::TempProjectDir dir("opennova_editor_ui_files_kept");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Kept"));
	session.run_operations();
	editor_test::create_missing_files(session);
	Ui ui;
	ui.windows.set_view(&session.view());
	ui.frames(6);
	const auto *window = dynamic_cast<const FilesWindow *>(find_window(ui.windows.pass(), "Files"));
	CHECK(window != nullptr, "the Files window");
	if (!window) return;
	const size_t made = window->rebuilds();
	CHECK(made >= 1, "the tree made");
	session.handle(request::clear_output());
	// Nothing to save: the status line alone.
	session.handle(request::save_all());
	ui.frames(3);
	CHECK(window->rebuilds() == made, "Output and the status line: the tree kept");
	// An Undo in a file that is not open is refused with a warning on the file: Findings moves
	// alone of what the tree reads.
	const AssetEntry *items = session.view().project.scan->find("items.def");
	CHECK(items != nullptr, "the item table");
	if (!items) return;
	const uint64_t files = session.view().revisions.of(ViewConcern::Files);
	session.handle(request::undo(items->relative_path));
	ui.frames(2);
	CHECK(session.view().revisions.of(ViewConcern::Files) == files, "the files as they were");
	CHECK(window->rebuilds() == made + 1, "a finding alone: the tree and its counts made again");
	session.set_poll_budget({0, 64 * 1024}); // one step per poll
	session.handle(request::build()); // its refresh reads the files again
	ui.frames(2);
	const size_t building = window->rebuilds();
	size_t steps = 0;
	while (session.view().activity.operation.running() && steps < 100) {
		session.poll();
		ui.frames(1);
		if (!session.view().activity.operation.running()) break;
		++steps;
		CHECK(window->rebuilds() == building, "a build's step: the tree kept");
	}
	CHECK(steps > 1 && session.view().activity.has_build, "the build stepped");
	const std::string readme = session.view().project.root + "/notes/readme.txt";
	CHECK(editor_test::write_text(readme, "x"), "a file written");
	session.handle(request::rescan());
	session.run_operations();
	ui.frames(2);
	CHECK(window->rebuilds() > building && logged_frame(ui).find("readme.txt") != std::string::npos,
	      "a file found anew: the tree made again");
}

// "Import N files": how many rows the import dialog's last frame has checked (-1: none said).
int import_count(Ui &ui) {
	const std::string text = logged_frame(ui);
	for (size_t at = text.find("Import "); at != std::string::npos;
			at = text.find("Import ", at + 1)) {
		size_t end = at + 7;
		int count = 0;
		bool digits = false;
		while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
			count = count * 10 + (text[end++] - '0');
			digits = true;
		}
		if (digits && text.compare(end, 5, " file") == 0)
			return count;
	}
	return -1;
}

// The view events' mailboxes (S13 V4): the workspace sends each new event to the window it is
// for, which holds it until it draws and takes it once. A RevealRecord for a document whose tab
// does not show waits for that document's view (a closed document's go with it); a RevealFile
// while Files is closed waits, and is taken when Files opens: the file selected; one that asks the
// rename opens Rename... once, and nothing opens it again. An AskRename opens Rename everywhere on
// the preview it names, once (a Cancel is not undone by the frames after), and one naming another
// preview opens nothing. An ImportPlanned has the import dialog take the plan's checks again,
// once: a row unchecked stays unchecked until the next one. Another view's events already held
// when the workspace is given it are not sent; those posted after are.
void test_view_event_mailboxes() {
	editor_test::TempProjectDir dir("opennova_editor_ui_mailboxes");
	const auto a = menu_at(dir, "a.mnu", "menus/a.mnu");
	const auto b = menu_at(dir, "b.mnu", "menus/b.mnu");
	const auto c = menu_at(dir, "c.mnu", "menus/c.mnu");
	SessionView v = menu_view(a);
	editor_test::own(v.project.scan).entries = { file_entry("a.mnu", a->path(), AssetKind::Menu),
		file_entry("b.mnu", b->path(), AssetKind::Menu),
		file_entry("c.mnu", c->path(), AssetKind::Menu) };
	editor_test::own(v.project.scan).index();
	v.documents.open = { a, b, c };
	v.documents.active = a->path();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.away();
	ui.drain();
	DocumentWindow *documents = nullptr;
	FilesWindow *files = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count(); ++i) {
		devtools::Window &window = ui.windows.pass().window(i);
		if (!documents)
			documents = dynamic_cast<DocumentWindow *>(&window);
		if (!files)
			files = dynamic_cast<FilesWindow *>(&window);
	}
	CHECK(documents && files, "the Document and Files windows");
	if (!documents || !files)
		return;

	// A record of b asked to show while a's tab shows: held for b's view until it draws.
	const NodeAddress screen{ b->rows().front()->id, b->rows().front()->kind, 0 };
	post_event(v, ViewEventKind::RevealRecord, b->path(), screen, "name");
	ui.frames(3);
	CHECK(documents->held_events(b->path()) == 1, "held while b's view does not draw");
	v.documents.active = b->path();
	v.documents.selection.primary = screen;
	v.documents.selection.records = { screen };
	v.revisions.touch(ViewConcern::ActiveDocument);
	ui.frames(4);
	CHECK(documents->held_events(b->path()) == 0, "taken as b's view draws");
	// One for c, which closes before its view draws: it goes with it.
	post_event(v, ViewEventKind::RevealRecord, c->path(),
			{ c->rows().front()->id, c->rows().front()->kind, 0 }, "name");
	ui.frames(2);
	CHECK(documents->held_events(c->path()) == 1, "held for c");
	v.documents.open = { a, b };
	v.revisions.touch(ViewConcern::DocumentSet);
	ui.frames(2);
	CHECK(documents->held_events(c->path()) == 0, "c closed: its events gone");

	// Files closed: a RevealFile waits; Files opened, it is taken and the file selected.
	devtools::Window &files_window = *files;
	files_window.open = false;
	ui.frames(2);
	post_event(v, ViewEventKind::RevealFile, c->path());
	ui.frames(3);
	CHECK(files->events().held() == 1 && files->selected().empty(), "held while Files is closed");
	files_window.open = true;
	ui.frames(4);
	CHECK(files->events().held() == 0 && files->selected() == c->path(),
			"taken as Files draws: the file selected");
	// Asking the rename: Rename... opens on it once.
	post_event(v, ViewEventKind::RevealFile, b->path(), NodeAddress(), std::string(), true);
	ui.frames(4);
	CHECK(files->selected() == b->path() &&
					logged_frame(ui).find("Rename b.mnu to") != std::string::npos,
			"Rename... asked on it");
	ImGui::ClosePopupsExceptModals();
	ui.frames(4);
	CHECK(logged_frame(ui).find("Rename b.mnu to") == std::string::npos,
			"taken once: closed, it stays closed");
	// Two asks before Files draws, the older asking Rename...: the newest is shown and the
	// older passed over, its Rename... too, whether Files was closed or both came in one pump
	// (the view's one reveal was overwritten by each ask).
	files_window.open = false;
	ui.frames(2);
	post_event(v, ViewEventKind::RevealFile, c->path(), NodeAddress(), std::string(), true);
	post_event(v, ViewEventKind::RevealFile, a->path());
	ui.frames(3);
	CHECK(files->events().held() == 2, "both held while Files is closed");
	files_window.open = true;
	ui.frames(4);
	CHECK(files->events().held() == 0 && files->selected() == a->path() &&
					logged_frame(ui).find("Rename c.mnu to") == std::string::npos,
			"Files opened: the newest shown, the older's Rename... not opened");
	post_event(v, ViewEventKind::RevealFile, b->path(), NodeAddress(), std::string(), true);
	post_event(v, ViewEventKind::RevealFile, c->path());
	ui.frames(4);
	CHECK(files->selected() == c->path() &&
					logged_frame(ui).find("Rename b.mnu to") == std::string::npos,
			"both in one pump: the same");
	// Files closed while more asks come than the view keeps, over several frames: the newest
	// kKept wait, the newest of them shown when it opens.
	files_window.open = false;
	ui.frames(2);
	for (size_t i = 0; i < ViewEvents::kKept + 6; ++i) {
		const bool last = i + 1 == ViewEvents::kKept + 6;
		post_event(v, ViewEventKind::RevealFile, last ? b->path() : a->path());
		if (i % 10 == 9)
			ui.frames(1);
	}
	ui.frames(3);
	CHECK(files->events().held() == ViewEvents::kKept, "at most as many held as the view keeps");
	files_window.open = true;
	ui.frames(4);
	CHECK(files->events().held() == 0 && files->selected() == b->path(),
			"opened: the newest shown");

	// Rename everywhere on the preview an AskRename names, once.
	v.dialogs.rename_preview.serial = 3;
	v.dialogs.rename_preview.symbol = true;
	v.dialogs.rename_preview.kind = ReferenceKind::MenuScreen;
	v.dialogs.rename_preview.path = b->path();
	v.dialogs.rename_preview.locator = b->locator(screen);
	v.dialogs.rename_preview.field = "name";
	v.dialogs.rename_preview.old_name = b->rows().front()->name();
	v.dialogs.rename_preview.new_name = v.dialogs.rename_preview.requested = "RENAMED";
	post_event(v, ViewEventKind::AskRename, b->path(), NodeAddress(), "name", false, 2);
	ui.frames(3);
	CHECK(!modal_open("Rename everywhere"), "an ask naming another preview opens nothing");
	post_event(v, ViewEventKind::AskRename, b->path(), NodeAddress(), "name", false, 3);
	ui.frames(3);
	CHECK(modal_open("Rename everywhere"), "the ask naming the preview opens it");
	ui.activate(item_id(ImHashStr("Rename everywhere"), { "Cancel" }));
	ui.frames(4);
	ui.drain();
	CHECK(!modal_open("Rename everywhere"), "taken once: cancelled, it stays closed");

	// The import dialog takes a plan's checks again on each ImportPlanned, once.
	v.dialogs.import_preview = planned_import("C:/assets");
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(4);
	ImGui::SetWindowSize("Import files", ImVec2(1700.0f, 1000.0f));
	ui.frames(2);
	ui.away();
	CHECK(modal_open("Import files") && import_count(ui) == 5,
			"the dialog opens, five rows checked");
	ui.activate(import_table_item("import_plan", 5, "##take"));
	ui.away();
	CHECK(import_count(ui) == 4, "a row unchecked");
	ui.frames(4);
	ui.away();
	CHECK(import_count(ui) == 4, "no event: the checks kept");
	post_event(v, ViewEventKind::ImportPlanned);
	ui.frames(3);
	ui.away();
	CHECK(import_count(ui) == 5, "a plan made: its checks taken again");
	ui.activate(import_table_item("import_plan", 5, "##take"));
	ui.frames(4);
	ui.away();
	CHECK(import_count(ui) == 4, "taken once: the next uncheck stays");
	v.dialogs.import_preview = DialogsView::ImportPreview();
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(3);
	ui.drain();

	// Another view: the events it already holds are not sent, those posted after are.
	SessionView other = menu_view(a);
	editor_test::own(other.project.scan).entries = { file_entry(
															 "a.mnu", a->path(), AssetKind::Menu),
		file_entry("b.mnu", b->path(), AssetKind::Menu) };
	editor_test::own(other.project.scan).index();
	for (int i = 0; i < 20; ++i)
		post_event(other, ViewEventKind::RevealFile, a->path());
	ui.windows.set_view(&other);
	ui.frames(3);
	CHECK(files->events().held() == 0 && files->selected() == b->path(),
			"the other view's held events not sent");
	post_event(other, ViewEventKind::RevealFile, a->path());
	ui.frames(3);
	CHECK(files->selected() == a->path(), "one posted after is");
	ui.windows.set_view(&v);
	ui.frames(2);
}

void run_workspace_tests() {
	test_files_tree_kept();
	test_workspace_layout();
	test_dock_survives_resizes();
	test_document_tabs();
	test_document_tab_choices();
	test_view_prompt_outlives_its_tab();
	test_thirty_tabs();
	test_welcome_view();
	test_project_settings();
	test_project_settings_two_applies();
	test_menus();
	test_menu_bar_status();
	test_files_window();
	test_import_dialog();
	test_import_dialog_problem_root();
	test_import_dialog_held_rows();
	test_preview_follows();
	test_preview_model_gestures();
	test_preview_model_pane_input();
	test_mission_view_input();
	test_mission_view_ground();
	test_mission_view_placing();
	test_preview_steps_aside();
	test_canvas_fill_input();
	test_window_title();
	test_view_event_mailboxes();
	test_view_event_mailbox_cap();
}

} // namespace editor_ui_test
