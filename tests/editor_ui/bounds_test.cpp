// S11e (ADR 0046 S11): nothing the editor draws runs past what shows of it. Over a real
// project with a file of every kind the editor opens (a catalog; thirty string tables, one
// open, its strings' keys and text long; a menu with a deep tree of long names; a
// stylesheet with long names and comments; a model; a clip and an animation table),
// findings with long messages, each document's inspector form with every section open, the
// tools' popups (Files' Import and New lists, the File menu, the reference picker), and
// then the import dialog (also narrowed to 360 pixels) and the project settings open: at
// 1280x720, again with Files narrowed to 240 pixels, and again with the font a quarter
// larger, every window, child window and popup keeps its content within its width unless it
// scrolls sideways, and every cell of a table keeps within its column (a table that scrolls
// sideways still clips a cell to its column); the failure lists the offenders by name. In
// the narrow layout the mouse reaches the last button of a toolbar that wrapped (Files'
// Refresh, the menu view's Outdent) and the last of thirty tabs through the tab list, each
// raising its request.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/mnu_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_windows.h>
#include <editor/ui/inspector_layout.h>
#include "common/file_io.h"
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

// Every window drawn in the last frame whose content is wider than what shows of it while it
// does not scroll sideways (its content size against its content region), and every cell of
// a table whose content runs past its column (a table that scrolls sideways moves its
// columns, but each still clips its cells): by name, with the widths, for the failure to
// list. A child ImGui makes for a widget is left to the widget (a multiline text box scrolls
// its text itself; the child is named after its "##" label).
std::vector<std::string> overflowing() {
	std::vector<std::string> out;
	ImGuiContext &g = *GImGui;
	char line[320];
	for (const ImGuiWindow *window : g.Windows) {
		if (!window->Active || window->Hidden || window->SkipItems) continue;
		if (window->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_HorizontalScrollbar |
		                     ImGuiWindowFlags_AlwaysHorizontalScrollbar))
			continue;
		const char *child = std::strrchr(window->Name, '/');
		if ((window->Flags & ImGuiWindowFlags_ChildWindow) && child && std::strncmp(child + 1, "##", 2) == 0) continue;
		const float visible = window->ContentRegionRect.GetWidth();
		if (window->ContentSize.x > visible + 1.0f) {
			std::snprintf(line, sizeof(line), "window %s: content %.0f wide in %.0f", window->Name, window->ContentSize.x, visible);
			out.push_back(line);
		}
	}
	for (int i = 0; i < g.Tables.GetMapSize(); ++i) {
		const ImGuiTable *table = g.Tables.TryGetMapData(i);
		if (!table || table->LastFrameActive != g.FrameCount) continue;
		if (!table->OuterWindow || table->OuterWindow->SkipItems) continue;
		for (int c = 0; c < table->ColumnsCount; ++c) {
			const ImGuiTableColumn &column = table->Columns[c];
			if (!column.IsEnabled || !column.IsVisibleX) continue;
			const float content = std::max(column.ContentMaxXFrozen, column.ContentMaxXUnfrozen);
			if (content > column.WorkMaxX + 1.0f) {
				std::snprintf(line, sizeof(line), "table %08x in %s, column %d: content to %.0f, past %.0f", table->ID,
				              table->OuterWindow->Name, c, content, column.WorkMaxX);
				out.push_back(line);
			}
		}
	}
	return out;
}

// Ten windows each inside the last, every name long: a tree deeper and wider than a narrow
// dock.
std::string deep_menu() {
	std::string text = "<SCREEN>\r\n\t<NAME>A_SCREEN_WHOSE_NAME_RUNS_ON_AND_ON</NAME>\r\n";
	for (int depth = 0; depth < 10; ++depth) {
		text += std::string(size_t(depth) + 1, '\t') + "<WINDOW type=\"" + (depth ? "static" : "window") +
		        "\" name=\"LEVEL_" + std::to_string(depth) + "_WINDOW_WITH_A_RATHER_LONG_NAME\">\r\n";
		text += std::string(size_t(depth) + 2, '\t') +
		        "<POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>700</RIGHT><BOTTOM>500</BOTTOM></POSITION>\r\n";
	}
	for (int depth = 9; depth >= 0; --depth) text += std::string(size_t(depth) + 1, '\t') + "</WINDOW>\r\n";
	return text + "</SCREEN>\r\n";
}

// Words that run far past any column: `what`, then on and on.
std::string long_words(const char *what) {
	std::string text = what;
	for (int i = 0; i < 6; ++i) text += " and on, past what any column of a window shows";
	return text;
}

// A stylesheet whose names, values and comments run long, the blank menus' variables kept.
std::string long_style() {
	std::string text = "// " + long_words("A comment line") + ".\r\n";
	for (const char *define : {"DEF_FONTNAME Arial16n.fnt", "DEF_FONTNAME_LG Arial16b.fnt", "IMPACT_FONTNAME Impac38b.fnt",
	                           "DEF_TEXT_FG FFFFFFFF", "DEF_TEXT_MOUSEOVER_FG FFFFC040", "DEF_TEXT_SELECTED_FG FFFF8000",
	                           "DEF_TEXT_DISABLED_FG FF545252", "TRIM_COLOR FF808080", "ITEM_SELECTED_BG 80FF8000",
	                           "COLOR_BLACK FF000000", "SEMIOPAQUE_BLACK 80000000"})
		text += std::string(define) + " // " + long_words("A trailing comment") + "\r\n";
	text += "A_VARIABLE_WHOSE_NAME_RUNS_ON_AND_ON_PAST_ITS_COLUMN A_VALUE_THAT_RUNS_ON_AND_ON_PAST_ITS_COLUMN_TOO\r\n";
	return text;
}

// table00.bin's first section given strings whose keys and text run long, saved and closed.
bool fill_strings(ProjectSession &session) {
	const char *const path = "strings/table00.bin";
	session.handle(make_request(EditorRequestKind::OpenDocument, path));
	Document *table = session.document_for(path);
	if (!table || table->rows().empty()) return false;
	const NodeId section = table->rows().front()->id;
	for (int i = 0; i < 3; ++i) {
		Edit add;
		add.operation = EditOperation::Add;
		add.address = {section, table->kind_from_name("string"), 0};
		Diagnostic error;
		if (!table->apply(add, error)) return false;
		const NodeAddress string = table->address_of(table->last_added());
		Edit key, text;
		key.address = text.address = string;
		key.field = "key";
		key.value = "A_KEY_THAT_RUNS_ON_PAST_ITS_COLUMN_" + std::to_string(i);
		text.field = "text";
		text.value = long_words("A string's text");
		if (!table->apply(std::vector<Edit>{key, text}, error)) return false;
	}
	session.handle(make_request(EditorRequestKind::Save, path));
	const bool saved = !table->dirty();
	session.handle(make_request(EditorRequestKind::CloseDocument, path));
	return saved;
}

// The project: the preview project's files, thirty string tables (the first with long
// strings), a stylesheet whose lines run long and the deep menu.
bool bounds_project(ProjectSession &session, const editor_test::TempProjectDir &dir) {
	if (!preview_project(session, dir)) return false;
	const SessionView &v = session.view();
	const AssetEntry *table = v.project.scan->find("gametext.bin");
	const AssetEntry *style = v.project.scan->find("menu_style.mns");
	if (!table || !style) return false;
	const std::vector<uint8_t> bytes = test_io::read_file(v.project.root + "/" + table->relative_path);
	for (int i = 0; i < 30; ++i) {
		char name[32];
		std::snprintf(name, sizeof(name), "/strings/table%02d.bin", i);
		if (!editor_test::write_bytes(v.project.root + name, bytes)) return false;
	}
	if (!editor_test::write_text(v.project.root + "/" + style->relative_path, long_style()) ||
	    !editor_test::write_text(v.project.root + "/menus/deep.mnu", deep_menu()))
		return false;
	session.handle(make_request(EditorRequestKind::Rescan));
	return v.project.scan->find("table29.bin") && v.project.scan->find("deep.mnu") && fill_strings(session);
}

// A finding whose message runs far past any line, on a record's field of a file.
Diagnostic long_finding(const std::string &asset, const char *code) {
	std::string message = "A finding whose message runs on";
	for (int i = 0; i < 12; ++i) message += " and on, past what any line of a window shows";
	message += ".";
	Diagnostic d = make_diagnostic(DiagnosticSeverity::Warning, code, message, asset, "a_field_with_a_long_identifier");
	d.record = "A_RECORD_WITH_A_NAME_LONG_ENOUGH_TO_CUT";
	d.line = 1234;
	return d;
}

// The workspace over a copy of the session's view with long findings added, the preview
// devices following it as the shell's do.
struct Sweep {
	ProjectSession &session;
	Ui &ui;
	FakePreview &menu;
	ModelDevice &model;
	SessionView view;

	// The session's view again, the long findings added, drawn until it settles.
	void follow() {
		view = session.view();
		view.findings.diagnostics.push_back(long_finding("defs/items.def", "catalog.test"));
		view.findings.diagnostics.push_back(long_finding("", "project.test"));
		view.revisions.touch(ViewConcern::Findings);
		ui.windows.set_view(&view);
		for (int i = 0; i < 4; ++i) {
			model.held.follow(view);
			ui.frames();
		}
		ui.away();
		ui.frames(2);
		ui.drain();
	}
	// A document made the one open (as the editor MCP makes it), one of its records selected.
	// One tab at a time: a tab bar counts every tab's width as its content (it scrolls its tabs
	// itself, reach_last_tab), which would hide what the view draws past the window.
	void open(const char *path, const char *record) {
		std::vector<std::string> others;
		for (const auto &document : session.view().documents.open) others.push_back(document->path());
		for (const std::string &other : others) session.handle(make_request(EditorRequestKind::CloseDocument, other));
		session.handle(make_request(EditorRequestKind::OpenDocument, path));
		const Document *document = session.document_for(path);
		NodeAddress address;
		if (document && record && document->find(record, address)) {
			EditorRequest select = make_request(EditorRequestKind::SelectRecord, document->path());
			select.edit.address = address;
			session.handle(select);
		} else if (document && !record && !document->rows().empty() && !session.view().documents.selection.row) {
			EditorRequest select = make_request(EditorRequestKind::SelectRecord, document->path());
			select.edit.address = {document->rows().front()->id, document->rows().front()->kind, 0};
			session.handle(select);
		}
		follow();
	}
	// Every section of the inspector's form open (those the file leaves out start folded), and
	// the catalog's spawn registry: set open in the windows' own state, as a click leaves it.
	void unfold() {
		const Document *document = session.document_for(view.documents.active);
		ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
		if (!document || !inspector || !view.documents.selection.row) return;
		NodeAddress owner = view.documents.selection;
		Document::Placement at;
		if (document->collections_of(owner).empty() && document->placement(owner, at)) owner = at.owner;
		for (const InspectorSection &section : plan_inspector(*document, view.documents.selection, owner, ""))
			if (!section.key.empty()) inspector->StateStorage.SetInt(item_id(inspector->ID, {("###" + section.key).c_str()}), 1);
		if (ImGuiWindow *window = ImGui::FindWindowByName("Document"))
			window->StateStorage.SetInt(item_id(document_tab_id(document->path()), {"spawn"}), 1);
		ui.frames(3);
	}
	// The first reference field's picker in the inspector's form (Pick), open; false when the
	// form has none or it did not open.
	bool open_picker() {
		const Document *document = session.document_for(view.documents.active);
		if (!document || !view.documents.selection.row) return false;
		NodeAddress owner = view.documents.selection;
		Document::Placement at;
		if (document->collections_of(owner).empty() && document->placement(owner, at)) owner = at.owner;
		for (const InspectorSection &section : plan_inspector(*document, view.documents.selection, owner, ""))
			for (const FieldUse &field : section.fields) {
				if (field.reference == ReferenceKind::None || field.schema->type != FieldType::Text) continue;
				if (field.schema->optional && !document->present(view.documents.selection, field.schema->id)) continue;
				ui.activate(item_id(Ui::window_id("Inspector"), {section.key.c_str(), "fields", field.schema->id.c_str(), "Pick"}));
				ui.frames(2);
				return GImGui->OpenPopupStack.Size > 0;
			}
		return false;
	}
	// What overflows now, each named with where the sweep was (none: nothing).
	void check(const std::string &where) {
		for (const std::string &offender : overflowing()) CHECK(false, (where + ": " + offender).c_str());
	}
};

// The tools' popups, each checked while it shows: Files' Import and New lists, the File menu.
void sweep_popups(Sweep &sweep, const std::string &where) {
	Ui &ui = sweep.ui;
	const ImGuiID files = Ui::window_id("Files");
	for (const char *list : {"##import", "##new"}) {
		ui.activate(item_id(files, {list}));
		ui.frames(2);
		CHECK(ImGui::FindWindowByName("##Combo_00") && ImGui::FindWindowByName("##Combo_00")->Active,
		      (where + ": Files " + list + " open").c_str());
		sweep.check(where + ", Files " + list);
		ImGui::ClosePopupsExceptModals();
		ui.frames(2);
	}
	ui.activate(item_id(menu_bar_id(), {"File"}));
	ui.frames(2);
	sweep.check(where + ", the File menu");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
	ui.drain();
}

// Each document open in turn (its inspector form folded as it opens, then every section open,
// and the reference picker of a form that has one), the tools' popups, then the import
// dialog, then the project settings: nothing runs past what shows of it.
void sweep_everything(Sweep &sweep, const std::string &layout) {
	const struct {
		const char *path;
		const char *record; // a record to select (null: the first row, unless one is)
	} documents[] = {{"defs/items.def", nullptr},
	                 {"strings/table00.bin", nullptr},
	                 {"menus/deep.mnu", "LEVEL_9_WINDOW_WITH_A_RATHER_LONG_NAME"},
	                 {"menu_style.mns", nullptr},
	                 {"models/armory.3di", nullptr},
	                 {"anims/walk.bad", nullptr},
	                 {"anims/SKIN.adm", nullptr}};
	bool picked = false;
	for (const auto &document : documents) {
		sweep.open(document.path, document.record);
		sweep.check(layout + ", " + document.path);
		sweep.unfold();
		sweep.check(layout + ", " + document.path + ", every section open");
		if (sweep.open_picker()) {
			picked = true;
			sweep.check(layout + ", " + document.path + ", the reference picker");
			ImGui::ClosePopupsExceptModals();
			sweep.ui.frames(2);
			sweep.ui.drain();
		}
	}
	CHECK(picked, (layout + ": a reference picker opened").c_str());
	sweep_popups(sweep, layout);
	// The import dialog: a plan whose names, places and notes run long, with an archive's members
	// to choose from (one of them named long), and changed since it was shown.
	sweep.follow();
	sweep.view.dialogs.import_preview =
	        planned_import("C:/assets/a_folder_whose_path_runs_long_enough_to_be_cut_in_any_column", "_with_a_name_long_enough_to_cut");
	sweep.view.dialogs.import_preview.choices = {{"C:/assets/data.pff", "main.mnu", false, false},
	                                     {"C:/assets/data.pff", "a_member_whose_name_runs_long_enough_to_be_cut.mnu", false, false}};
	sweep.view.dialogs.import_preview.changed = true;
	sweep.view.revisions.touch(ViewConcern::Dialogs);
	sweep.ui.frames(4);
	CHECK(ImGui::FindWindowByName("Import files") && ImGui::FindWindowByName("Import files")->Active,
	      (layout + ": the import dialog open").c_str());
	sweep.check(layout + ", the import dialog");
	// Narrowed to 360 pixels: each line wraps whole or is cut to what shows.
	if (ImGuiWindow *dialog = ImGui::FindWindowByName("Import files")) {
		const ImVec2 size = dialog->Size;
		ImGui::SetWindowSize("Import files", ImVec2(360.0f, size.y));
		sweep.ui.frames(3);
		sweep.check(layout + ", the import dialog 360 pixels wide");
		ImGui::SetWindowSize("Import files", size);
	}
	sweep.view.dialogs.import_preview = DialogsView::ImportPreview();
	sweep.view.revisions.touch(ViewConcern::Dialogs);
	sweep.ui.frames(3);
	// The project settings.
	choose(sweep.ui, "File", {"Project settings..."});
	sweep.ui.frames(3);
	CHECK(ImGui::FindWindowByName("Project settings") && ImGui::FindWindowByName("Project settings")->Active,
	      (layout + ": the project settings open").c_str());
	sweep.check(layout + ", the project settings");
	sweep.ui.activate(item_id(ImHashStr("Project settings"), {"Cancel"}));
	sweep.ui.frames(2);
	sweep.ui.drain();
}

// Where the mouse hovers the item `id`, moving over `window`'s rows `top` to `bottom` in steps;
// false when it never does.
bool hover_item(Ui &ui, ImGuiID id, const ImGuiWindow *window, float top, float bottom, ImVec2 &at) {
	for (float y = top; y < bottom; y += 3.0f)
		for (float x = window->Pos.x + 2.0f; x < window->Pos.x + window->Size.x - 2.0f; x += 6.0f) {
			ui.mouse(x, y);
			if (GImGui->HoveredId == id) {
				at = ImVec2(x, y);
				return true;
			}
		}
	return false;
}

// The last button of a toolbar that wrapped, pressed with the mouse: Files' Refresh, and
// the menu view's Outdent (a nested window selected).
void reach_wrapped_toolbars(Sweep &sweep) {
	Ui &ui = sweep.ui;
	const ImGuiWindow *files = ImGui::FindWindowByName("Files");
	ImVec2 at;
	CHECK(files && hover_item(ui, item_id(Ui::window_id("Files"), {"Refresh"}), files, files->Pos.y,
	                          files->Pos.y + ImGui::GetFrameHeightWithSpacing() * 4.0f, at),
	      "Files' Refresh reached");
	const float first_line = files ? files->Pos.y + files->TitleBarHeight + ImGui::GetStyle().WindowPadding.y +
	                                         ImGui::GetFrameHeight() : 0.0f;
	CHECK(at.y > first_line, "Refresh wrapped under Import and New");
	ui.button(true);
	ui.button(false);
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::Rescan) != nullptr, "Refresh raised");
	ui.away();

	sweep.open("menus/deep.mnu", "LEVEL_9_WINDOW_WITH_A_RATHER_LONG_NAME");
	const ImGuiWindow *document = ImGui::FindWindowByName("Document");
	const ImGuiID outdent = item_id(document_tab_id(sweep.session.document_for("menus/deep.mnu")->path()), {"Outdent"});
	CHECK(document && hover_item(ui, outdent, document, document->Pos.y, document->Pos.y + document->Size.y * 0.6f, at),
	      "the menu view's Outdent reached");
	ui.button(true);
	ui.button(false);
	requests = ui.drain();
	const EditorRequest *move = one(requests, EditorRequestKind::EditRecord);
	CHECK(move && move->edit.operation == EditOperation::Move, "Outdent raised");
	ui.away();
}

// Thirty string tables open: the tab list, opened with the mouse, lists them, and the mouse
// reaches its last entry, which makes that table the active document.
void reach_last_tab(Sweep &sweep) {
	Ui &ui = sweep.ui;
	std::string last;
	for (int i = 0; i < 30; ++i) {
		char path[32];
		std::snprintf(path, sizeof(path), "strings/table%02d.bin", i);
		sweep.session.handle(make_request(EditorRequestKind::OpenDocument, path));
		last = path;
	}
	sweep.session.handle(make_request(EditorRequestKind::OpenDocument, "defs/items.def"));
	sweep.follow();
	ImGuiTabBar *bar = GImGui->TabBars.GetByKey(item_id(Ui::window_id("Document"), {"documents"}));
	CHECK(bar && bar->WidthAllTabs > bar->BarRect.GetWidth(), "more tabs than the bar shows");
	if (!bar) return;
	const ImGuiWindow *document = ImGui::FindWindowByName("Document");
	ImVec2 at;
	CHECK(hover_item(ui, item_id(bar->ID, {"##v"}), document, bar->BarRect.Min.y + 1.0f, bar->BarRect.Max.y, at),
	      "the tab list's button reached");
	ui.button(true);
	ui.button(false);
	ui.frames();
	const ImGuiWindow *list = ImGui::FindWindowByName("##Combo_00");
	CHECK(list && list->Active, "the tab list open");
	if (!list || !list->Active) return;
	const ImGuiID entry = ImHashStr(("###" + last).c_str(), 0, ImHashStr("##Combo_00"));
	bool reached = false;
	for (int turn = 0; turn < 12 && !reached; ++turn) {
		reached = hover_item(ui, entry, list, list->Pos.y, list->Pos.y + list->Size.y, at);
		if (reached) break;
		ui.mouse(list->Pos.x + list->Size.x * 0.5f, list->Pos.y + list->Size.y * 0.5f);
		ImGui::GetIO().AddMouseWheelEvent(0.0f, -5.0f);
		ui.frames(2);
	}
	CHECK(reached, "the last entry reached");
	ui.button(true);
	ui.button(false);
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *open = only(requests, EditorRequestKind::OpenDocument);
	CHECK(open && open->path == last, "the last tab, chosen from the list, opens");
	ui.away();
}

void test_bounds() {
	editor_test::TempProjectDir dir("opennova_editor_ui_bounds");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(bounds_project(session, dir), "the project");
	FakePreview menu;
	session.handle(make_request(EditorRequestKind::OpenDocument, "menus/deep.mnu"));
	const auto *deep = dynamic_cast<const MnuDocument *>(session.document_for("menus/deep.mnu"));
	CHECK(deep && !deep->rows().empty() &&
	              menu.render.configure(*deep, deep->rows().front()->id, menu.files, {}) == MenuPreviewStatus::Ready,
	      "the deep menu renders");
	ModelDevice model;
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
	ui.windows.set_menu_preview_viewport(&menu);
	ui.windows.set_model_preview_viewport(&model);
	Sweep sweep{session, ui, menu, model, SessionView()};
	sweep.follow();
	ui.frames(4);

	sweep_everything(sweep, "1280x720");

	// Files narrowed to 240 pixels.
	const ImGuiWindow *files = ImGui::FindWindowByName("Files");
	CHECK(files && files->DockId, "Files docked");
	if (files && files->DockId) ImGui::DockBuilderSetNodeSize(files->DockId, ImVec2(240.0f, files->Size.y));
	ui.frames(4);
	files = ImGui::FindWindowByName("Files");
	CHECK(files && std::fabs(files->Size.x - 240.0f) < 2.0f, "Files 240 wide");
	sweep_everything(sweep, "Files at 240");

	// The font a quarter larger.
	ImGui::GetIO().FontGlobalScale = 1.25f;
	ui.frames(4);
	sweep_everything(sweep, "Files at 240, font x1.25");
	reach_wrapped_toolbars(sweep);
	reach_last_tab(sweep);
	ImGui::GetIO().FontGlobalScale = 1.0f;
}

} // namespace

void run_bounds_tests() { test_bounds(); }

} // namespace editor_ui_test
