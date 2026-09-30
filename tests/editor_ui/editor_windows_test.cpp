// The editor's windows over a null ImGui backend (ADR 0046 d11); the S11d workspace's shell (the
// windows and their layout, the Document window's tabs, Files, the menus, the modals) is
// workspace_test.cpp's. S9h2: the menu view's tree (built once from the Windows collections,
// the Moves a drop, an Indent and an Outdent make) and the inspector's plan (groups by dotted
// id, the block toggle first, collections in the group their token names,
// ignored-and-left-out fields hidden); then, driven through the real windows, a click on a
// tree row in the menu's Document tab selects it (Ctrl+click toggles), a drag drops a window
// inside, before or after another, the toolbar's buttons and Ctrl+C / X / V raise their
// requests, and the inspector's tables add, move and edit rows in place. S9k1: the Preview
// window's menu pane over a fake device backed by the headless render: a click selects, a
// drag of a window or a handle is one gesture of Sets then its end (a small window's middle
// a move), the arrows nudge, Esc selects the parent, a stale picture maps nothing, the zoom,
// the held state, the empty states; a drag or a nudge ends once when the pane stops drawing
// (the model pane shown, Preview closed) and the hidden pane takes no key. S11a: a frame's
// Save goes after the frame's edits, and the save
// shortcuts work while a text field has the keyboard. S11c: the Problems window, pressed
// with the mouse where a user presses (the filters, the grouping and folding, the fixes and
// what asks before it acts, a press and its release on the same fix of the same finding, a
// confirmation following its project and its findings, nothing past its cell in a narrow
// dock, no Rewrite for a file that does not serialize, a thousand findings clipped, flat and
// grouped) and the pieces it draws with (ui_kit). S12 D3: the Inspector's Go to (a menu of
// the places a font through a style variable leads) and its clickable "Referenced by" rows.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/model_document.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/model/field_text.h>
#include <editor/session/problem_query.h>
#include <editor/session/session_view.h>
#include "../editor/editor_test_support.h"
#include "../editor/menu_test_support.h"
#include "common/test_paths.h"
#include "editor_ui_test_support.h"
#include <editor/ui/editor_windows.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/preview_window.h>
#include <editor/ui/problems_window.h>
#include <editor/ui/record_tree.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>
#include <imgui_internal.h>

using namespace opennova::editor;
using namespace editor_ui_test;
namespace devtools = opennova::devtools;

namespace {

void test_requests_round_trip() {
	EditorWindows windows;
	windows.request(make_request(EditorRequestKind::OpenProject, "C:/mods/A"));
	EditorRequest set = make_request(EditorRequestKind::ApplyProjectSettings);
	set.settings.mission = true;
	windows.request(set);
	CHECK(windows.pending_requests() == 2, "two queued");
	EditorRequest out;
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::OpenProject && out.path == "C:/mods/A",
	      "oldest first");
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::ApplyProjectSettings &&
	              out.settings.mission == std::optional<bool>(true),
	      "then the settings");
	CHECK(!windows.take_request(out), "drained");

	// The pickers' answers: an open-project pick becomes the request; the runtime and the
	// game install folder fill the project settings' fields (while the dialog that asked is
	// open on its project), which its Apply sends (workspace_test.cpp); a folder for a new
	// project fills the new-project form; a cancelled pick raises nothing.
	windows.deliver_pick(PickPurpose::OpenProject, "C:/mods/B");
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::OpenProject && out.path == "C:/mods/B",
	      "open pick");
	windows.deliver_pick(PickPurpose::RuntimeExecutable, "C:/tools/opennova.exe");
	windows.deliver_pick(PickPurpose::RetailDirectory, "C:/games/Joint Operations");
	CHECK(!windows.take_request(out), "the settings' picks fill its fields, they apply nothing by themselves");
	windows.deliver_pick(PickPurpose::OpenProject, "");
	CHECK(!windows.take_request(out), "a cancelled pick raises nothing");
	windows.deliver_pick(PickPurpose::NewProjectLocation, "C:/mods/New");
	CHECK(!windows.take_request(out) && std::string(windows.new_project_form().folder()) == "C:/mods/New",
	      "a location pick fills the form, it does not open");
}

// --- S9h2: the menu view's tree and the inspector over the full menu schema ---------

constexpr NodeKind kScreen = node_kind(MenuKind::Screen);
constexpr NodeKind kWindow = node_kind(MenuKind::Window);

std::vector<std::string> tree_names(const MnuDocument &document, const RecordTree &tree) {
	std::vector<std::string> names;
	for (const RecordTree::Entry &entry : tree.entries) names.push_back(menu_test::window_of(document, entry.address)->name);
	return names;
}

// The tree of one screen's windows (a part's windows left out) and the one Move each
// gesture makes, applied and undone.
void test_menu_tree_model() {
	editor_test::TempProjectDir dir("opennova_editor_ui_tree_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const Node &screen = *document->rows()[0];
	const RecordTree tree = build_record_tree(*document, screen, kWindow);
	CHECK(tree_names(*document, tree) == std::vector<std::string>({"MAIN", "BACK", "PANEL", "CHOICES", "TITLE", "OVERLAY"}),
	      "pre-order, a part's window left out");
	CHECK(tree.roots.size() == 2 && tree.children_of(0).size() == 2, "two roots");
	const NodeAddress main = named(*document, "MAIN"), back = named(*document, "BACK"), panel = named(*document, "PANEL"),
	                  choices = named(*document, "CHOICES"), title = named(*document, "TITLE"),
	                  overlay = named(*document, "OVERLAY");
	NodeId in_part = 0; // find() reaches the document's windows only: walk for the one a part holds
	document->walk_records(screen, [&](const NodeAddress &record, const Document::Placement &) {
		if (record.kind == kWindow && menu_test::window_of(*document, record)->name == "INPART") in_part = record.child;
		return true;
	});
	CHECK(in_part && !tree.find(in_part), "INPART sits in a part");
	CHECK(tree.find(choices.child)->owner == panel.child && tree.find(choices.child)->depth == 2, "CHOICES under PANEL");
	CHECK(tree.find(title.child)->index == 2 && tree.children_of(main.child).size() == 3, "TITLE third in MAIN");
	CHECK(tree.inside(choices.child, main.child) && !tree.inside(main.child, choices.child), "inside");

	const std::string original = document->serialize().text;
	Diagnostic error;
	auto names_under = [&](const NodeAddress &owner) {
		std::vector<std::string> out;
		const RecordTree now = build_record_tree(*document, *document->rows()[0], kWindow);
		for (const size_t index : now.children_of(owner.child))
			out.push_back(menu_test::window_of(*document, now.entries[index].address)->name);
		return out;
	};
	auto apply_and_undo = [&](const Edit &move, const NodeAddress &owner, const std::vector<std::string> &expected,
	                          const char *what) {
		CHECK(document->apply(move, error), what);
		CHECK(names_under(owner) == expected, what);
		CHECK(document->identities_match(), what);
		document->undo();
		CHECK(document->serialize().text == original, what);
	};
	Edit move;
	CHECK(drop_edit(tree, back.child, title.child, DropPlace::Inside, move) && move.operation == EditOperation::Move &&
	              move.address == back && move.parent == title.child && move.position == SIZE_MAX,
	      "a drop inside names the target");
	apply_and_undo(move, title, {"BACK"}, "BACK into TITLE");
	CHECK(drop_edit(tree, back.child, title.child, DropPlace::After, move) && move.parent == main.child && move.position == 2,
	      "after a later sibling: its index once BACK is out");
	apply_and_undo(move, main, {"PANEL", "TITLE", "BACK"}, "BACK after TITLE");
	CHECK(drop_edit(tree, title.child, back.child, DropPlace::Before, move) && move.position == 0, "before an earlier sibling");
	apply_and_undo(move, main, {"TITLE", "BACK", "PANEL"}, "TITLE before BACK");
	CHECK(drop_edit(tree, choices.child, overlay.child, DropPlace::After, move) && move.parent == screen.id && move.position == 2,
	      "after a root: the screen's own list, named by its row");
	apply_and_undo(move, {screen.id, kScreen, 0}, {"MAIN", "OVERLAY", "CHOICES"}, "CHOICES to the top level");
	CHECK(!drop_edit(tree, main.child, choices.child, DropPlace::Inside, move), "never inside itself");
	CHECK(!drop_edit(tree, main.child, main.child, DropPlace::Before, move), "never onto itself");
	CHECK(!indent_edit(tree, back.child, move), "the first sibling has nothing to go into");
	CHECK(indent_edit(tree, panel.child, move) && move.parent == back.child && move.position == SIZE_MAX, "indent");
	apply_and_undo(move, back, {"PANEL"}, "PANEL into BACK");
	CHECK(!outdent_edit(tree, main.child, move), "a root window has nowhere to go out to");
	CHECK(outdent_edit(tree, choices.child, move) && move.parent == main.child && move.position == 2, "outdent");
	apply_and_undo(move, main, {"BACK", "PANEL", "CHOICES", "TITLE"}, "CHOICES out of PANEL");
	CHECK(outdent_edit(tree, back.child, move) && move.parent == screen.id && move.position == 1, "outdent a child of a root");
	apply_and_undo(move, {screen.id, kScreen, 0}, {"MAIN", "BACK", "OVERLAY"}, "BACK to the top level");
}

const InspectorSection *section_of(const std::vector<InspectorSection> &plan, const char *key) {
	for (const InspectorSection &section : plan)
		if (section.key == key) return &section;
	return nullptr;
}

bool has_field(const InspectorSection *section, const char *id) {
	return section && std::any_of(section->fields.begin(), section->fields.end(), [&](const FieldUse &f) { return f.schema->id == id; });
}

// The inspector's plan: groups by the first step of a dotted id with their readable
// headings, the block's toggle first and apart, collections in the group their token
// names (a list's ITEMS rows, a part), the rest in sections of their own; what the type
// ignores and the file leaves out is not there; a filter; a leaf's own fields with the
// records beside it.
void test_inspector_plan() {
	editor_test::TempProjectDir dir("opennova_editor_ui_plan_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const NodeAddress back = named(*document, "BACK"), choices = named(*document, "CHOICES");
	const std::vector<InspectorSection> button = plan_inspector(*document, back, back, "");
	CHECK(!button.empty() && button.front().key.empty() && has_field(&button.front(), "name") &&
	              has_field(&button.front(), "type"),
	      "the general fields come first, with no heading");
	const InspectorSection *position = section_of(button, "position");
	CHECK(position && position->title == "Position" && position->fields.size() == 4 && !position->has_toggle && position->written,
	      "POSITION's four edges under one heading, open while written");
	CHECK(position && position->fields[0].schema->label == "Left", "a field's readable name");
	const InspectorSection *text = section_of(button, "string");
	CHECK(text && text->title == "Text" && text->has_toggle && text->toggle.schema->id == "string" && !has_field(text, "string"),
	      "STRING's own switch leads its group, apart from its fields");
	CHECK(!section_of(button, "items") && !section_of(button, "column") && !section_of(button, "datasource"),
	      "what a button does not read, and its file leaves out, is not there");
	const InspectorSection *actions = section_of(button, "action");
	CHECK(actions && actions->collections.size() == 1 && actions->collections[0].ids.size() == 2 && actions->title == "Actions",
	      "the actions are a section of their own");
	CHECK(section_of(button, "sound") && section_of(button, "hotkey") && section_of(button, "appearance"), "the other lists");
	CHECK(!has_field(&button.front(), "checked"), "a flag the button ignores and the file leaves out is hidden");

	const std::vector<InspectorSection> list = plan_inspector(*document, choices, choices, "");
	const InspectorSection *items = section_of(list, "items");
	CHECK(items && items->has_toggle && has_field(items, "items.multiselect") &&
	              std::any_of(items->collections.begin(), items->collections.end(),
	                          [](const Document::Collection &c) { return std::string(c.spec.kind_name) == "items.item" && c.ids.size() == 2; }),
	      "ITEMS: its switch, its fields and its item rows together");
	const InspectorSection *scrollbar = section_of(list, "scrollbar");
	CHECK(scrollbar && scrollbar->collections.size() == 1 && scrollbar->collections[0].spec.max == 1 &&
	              scrollbar->collections[0].ids.size() == 1,
	      "the scrollbar part in its own group, one at most");

	const std::vector<InspectorSection> filtered = plan_inspector(*document, back, back, "left");
	CHECK(filtered.size() == 1 && filtered[0].key == "position" && filtered[0].fields.size() == 1 &&
	              filtered[0].fields[0].schema->id == "position.left",
	      "a filter keeps the matching fields and drops the rest");

	const NodeAddress second = menu_test::child_of(*document, back, "action", 1);
	const std::vector<InspectorSection> leaf = plan_inspector(*document, second, back, "");
	CHECK(!leaf.empty() && has_field(&leaf.front(), "type") && has_field(&leaf.front(), "state") && has_field(&leaf.front(), "target"),
	      "a WINDOW action's own fields");
	CHECK(!has_field(&leaf.front(), "file"), "a WINDOW action reads no FILE, and leaves it out");
	CHECK(section_of(leaf, "action") && !section_of(leaf, "position"), "the records beside it, not its owner's fields");
	for (const FieldSchema &field : document->fields(menu_kind("action"))) {
		if (field.id != "type") continue;
		const FieldChoice *verb = choice_of(field, Value(std::string("pop_screen")));
		CHECK(verb && std::string(choice_title(*verb)) == "Go back" && std::string(verb->name) == "POP_SCREEN",
		      "a verb's readable name, its token kept");
	}
	for (NodeKind kind = 0; kind < 40; ++kind)
		for (const FieldSchema &field : document->fields(kind))
			CHECK(!field.label.empty(), ("every menu field has a readable name: " + field.id).c_str());
	const FieldSchema *hidden = nullptr;
	for (const FieldSchema &field : document->fields(kWindow))
		if (field.id == "hidden") hidden = &field;
	CHECK(hidden && is_yes_no(*hidden) && !written(*document, back, *hidden), "a flag is a switch, left out while no");
}

// --- driving the real windows ------------------------------------------------------------

// The Problems list as ImGui laid it out in the last frame, for a test to press on what a
// user sees: its table's columns, and where its lines are (each as high as a control, the
// first at the top of the scrolled region; a line after an expanded one moves down by it).
struct ProblemsLines {
	const ImGuiTable *table = nullptr;
	float line = 0.0f;
	float top = 0.0f;
	float y(size_t index) const { return top + (float(index) + 0.5f) * line; }
	// A point in a line's cell, `along` the column's width.
	ImVec2 at(size_t index, int column, float along = 0.5f) const {
		const ImGuiTableColumn &c = table->Columns[column];
		return ImVec2(c.MinX + (c.MaxX - c.MinX) * along, y(index));
	}
	// A point on the first control of a line's Fix column.
	ImVec2 fix(size_t index) const { return ImVec2(table->Columns[3].WorkMinX + 4.0f, y(index)); }
	// A point on its More, after a first fix labelled `label` (cut as the window cuts it).
	ImVec2 more(size_t index, const std::string &label) const {
		const ImGuiTableColumn &c = table->Columns[3];
		const ImGuiStyle &style = ImGui::GetStyle();
		const float more = ui_kit::button_width("More");
		const float room = c.WorkMaxX - c.WorkMinX - more - style.ItemSpacing.x;
		const float first = ui_kit::button_width(ui_kit::fit(label, room - style.FramePadding.x * 2.0f).c_str());
		return ImVec2(c.WorkMinX + first + style.ItemSpacing.x + more * 0.5f, y(index));
	}
};

ProblemsLines problems_lines() {
	ProblemsLines out;
	out.table = ImGui::TableFindByID(item_id(Ui::window_id("Problems"), {"problems"}));
	out.line = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
	if (out.table) out.top = out.table->InnerWindow->Pos.y - out.table->InnerWindow->Scroll.y;
	return out;
}

// The confirmation Problems asks before a Fix all or a Use fix, while it shows, and a point
// on its Apply or its Cancel (the last line of the modal, which fits its content).
const ImGuiWindow *confirmation() {
	const ImGuiWindow *modal = ImGui::FindWindowByName("Apply fixes");
	return modal && modal->Active ? modal : nullptr;
}
ImVec2 confirmation_button(bool cancel) {
	const ImGuiWindow *modal = ImGui::FindWindowByName("Apply fixes");
	const ImGuiStyle &style = ImGui::GetStyle();
	const float x = modal->Pos.x + style.WindowPadding.x + 6.0f +
	                (cancel ? ui_kit::button_width("Apply") + style.ItemSpacing.x : 0.0f);
	return ImVec2(x, modal->Pos.y + modal->Size.y - style.WindowPadding.y - ImGui::GetFrameHeight() * 0.5f);
}

// The menu view's tree, its drags, its toolbar and its shortcuts, through the menu's tab in
// the Document window: a row is found by clicking down the tree until it selects, a drag is
// a press, a move and a release, a button is pressed by its id, a shortcut by its keys.
void test_menu_window_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_menu_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const Node &screen = *document->rows()[0];
	SessionView v = menu_view(document);
	select_in(v, {screen.id, kScreen, 0});
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Document");
	ui.drain();
	CHECK(ui.windows.pending_requests() == 0, "drawing the tree raises nothing");

	// The tree's rows, top to bottom: click down the tree region.
	ImGuiWindow *menu = ImGui::FindWindowByName("Document");
	ImGuiWindow *tree = nullptr;
	for (ImGuiWindow *window : GImGui->Windows)
		if (window->ParentWindow == menu && std::strstr(window->Name, "/windows_")) tree = window;
	CHECK(tree != nullptr, "the tree has its own scrolling region");
	if (!tree) return;
	const float x = tree->Pos.x + tree->Size.x - 16.0f;
	std::map<NodeId, std::pair<float, float>> rows; // a window's first and last y a click selects it at
	std::vector<NodeId> order;
	for (float y = tree->Pos.y + 1.0f; y < tree->Pos.y + tree->Size.y - 1.0f; y += 1.0f) {
		ui.mouse(x, y);
		ui.button(true);
		ui.button(false);
		for (const EditorRequest &request : ui.drain()) {
			if (request.kind != EditorRequestKind::SelectRecord) continue;
			const NodeId id = request.edit.address.child;
			CHECK(request.edit.address.kind == kWindow && request.select_mode == SelectMode::Replace, "a click selects a window");
			if (!rows.count(id)) order.push_back(id);
			auto &extent = rows.emplace(id, std::make_pair(y, y)).first->second;
			extent.second = y;
		}
	}
	const RecordTree model = build_record_tree(*document, screen, kWindow);
	std::vector<NodeId> expected;
	for (const RecordTree::Entry &entry : model.entries) expected.push_back(entry.address.child);
	CHECK(order == expected, "every window of the screen is a row, in the tree's order");
	if (order != expected) return;
	auto middle = [&](const char *name) {
		const auto &extent = rows[named(*document, name).child];
		return (extent.first + extent.second) * 0.5f;
	};
	auto top = [&](const char *name) { return rows[named(*document, name).child].first + 1.0f; };
	auto bottom = [&](const char *name) { return rows[named(*document, name).child].second - 1.0f; };

	// Ctrl+click joins or leaves the selection.
	ui.mouse(x, middle("TITLE"));
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ui.button(true);
	ui.button(false);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
	ui.frames();
	const std::vector<EditorRequest> toggled = ui.drain();
	const EditorRequest *toggle = only(toggled, EditorRequestKind::SelectRecord);
	CHECK(toggle && toggle->edit.address == named(*document, "TITLE") && toggle->select_mode == SelectMode::Toggle,
	      "Ctrl+click toggles");

	// Shift+click selects the rows from the last one clicked (TITLE) to BACK in the tree's
	// order, BACK the primary: PANEL alone, then CHOICES, TITLE and BACK joined.
	ui.mouse(x, middle("BACK"));
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
	ui.button(true);
	ui.button(false);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
	ui.frames();
	std::vector<std::pair<std::string, SelectMode>> ranged;
	for (const EditorRequest &request : ui.drain())
		if (request.kind == EditorRequestKind::SelectRecord)
			ranged.emplace_back(menu_test::window_of(*document, request.edit.address)->name, request.select_mode);
	CHECK(ranged == (std::vector<std::pair<std::string, SelectMode>>{{"PANEL", SelectMode::Replace},
	                                                                 {"CHOICES", SelectMode::Add},
	                                                                 {"TITLE", SelectMode::Add},
	                                                                 {"BACK", SelectMode::Add}}),
	      "Shift+click selects the range, the clicked row the primary");
	// The row a range starts from belongs to its file: ids start again in every document, so
	// in another menu file (the same windows, the same ids) a Shift+click is a plain click
	// until a row there was clicked.
	{
		auto extra = std::make_shared<MnuDocument>();
		Diagnostic error;
		CHECK(editor_test::write_text(dir.file("extra.mnu"), kMenu), "second menu fixture");
		CHECK(extra->load(dir.file("extra.mnu"), "extra.mnu", AssetKind::Menu, "jo", error), error.message.c_str());
		CHECK(named(*extra, "TITLE") == named(*document, "TITLE"), "the same ids in both files");
		v.documents.push_back(extra);
		v.active_document = extra->path();
		select_in(v, {extra->rows()[0]->id, kScreen, 0});
		ui.frames(2);
		ui.drain();
		ui.mouse(x, middle("BACK"));
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
		ui.button(true);
		ui.button(false);
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
		ui.frames();
		const std::vector<EditorRequest> fresh = ui.drain();
		const EditorRequest *plain = only(fresh, EditorRequestKind::SelectRecord);
		CHECK(plain && plain->path == extra->path() && plain->edit.address == named(*extra, "BACK") &&
		              plain->select_mode == SelectMode::Replace,
		      "another file: no range from a row of the first");
		v.documents.pop_back();
		v.active_document = document->path();
		select_in(v, {screen.id, kScreen, 0});
		ui.frames(2);
		ui.drain();
	}

	// A drag: pressed on a window, moved in steps onto another, released there.
	auto drag = [&](const char *from, float to_y) {
		const float from_y = middle(from);
		ui.mouse(x, from_y);
		ui.button(true);
		for (int step = 1; step <= 8; ++step) ui.mouse(x, from_y + (to_y - from_y) * float(step) / 8.0f);
		ui.frames(2);
		ui.button(false);
		ui.frames();
		std::vector<EditorRequest> requests = ui.drain();
		requests.erase(std::remove_if(requests.begin(), requests.end(),
		                              [](const EditorRequest &r) { return r.kind == EditorRequestKind::SelectRecord; }),
		               requests.end());
		return requests;
	};
	const NodeAddress main = named(*document, "MAIN"), back = named(*document, "BACK"), title = named(*document, "TITLE");
	std::vector<EditorRequest> dropped = drag("BACK", middle("TITLE"));
	CHECK(dropped.size() == 1 && dropped[0].kind == EditorRequestKind::EditRecord &&
	              dropped[0].edit.operation == EditOperation::Move && dropped[0].edit.address == back &&
	              dropped[0].edit.parent == title.child && dropped[0].edit.position == SIZE_MAX,
	      "dropped on a window's middle: inside it, at the end");
	dropped = drag("BACK", bottom("TITLE"));
	CHECK(dropped.size() == 1 && dropped[0].edit.operation == EditOperation::Move && dropped[0].edit.parent == main.child &&
	              dropped[0].edit.position == 2,
	      "dropped on a window's lower edge: after it");
	dropped = drag("TITLE", top("PANEL"));
	CHECK(dropped.size() == 1 && dropped[0].edit.operation == EditOperation::Move && dropped[0].edit.address == title &&
	              dropped[0].edit.parent == main.child && dropped[0].edit.position == 1,
	      "dropped on a window's upper edge: before it");
	dropped = drag("MAIN", middle("CHOICES"));
	CHECK(dropped.empty(), "a window never drops inside itself");

	// The toolbar acts on the selected window.
	const ImGuiID menu_id = document_tab_id(document->path()); // the tab's items: its own id scope
	select_in(v, named(*document, "PANEL"));
	ui.frames(2);
	ui.drain();
	ui.activate(item_id(menu_id, {"Indent"}));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *indent = only(requests, EditorRequestKind::EditRecord);
	CHECK(indent && indent->edit.operation == EditOperation::Move && indent->edit.address == named(*document, "PANEL") &&
	              indent->edit.parent == back.child,
	      "Indent: into the window above");
	ui.activate(item_id(menu_id, {"Duplicate"}));
	requests = ui.drain();
	const EditorRequest *duplicate = only(requests, EditorRequestKind::Duplicate);
	CHECK(duplicate && duplicate->path == document->path() && !only(requests, EditorRequestKind::EditRecord),
	      "Duplicate: the session's, each selected window right after itself");
	ui.activate(item_id(menu_id, {"Add window"}));
	requests = ui.drain();
	const EditorRequest *add = only(requests, EditorRequestKind::EditRecord);
	CHECK(add && add->edit.operation == EditOperation::Add && add->edit.address.kind == kWindow &&
	              add->edit.parent == named(*document, "PANEL").child && add->edit.field == "type" &&
	              std::get<std::string>(add->edit.value) == "static",
	      "Add window: a window of the picked type inside the selection, one edit");
	select_in(v, named(*document, "CHOICES"));
	ui.frames(2);
	ui.activate(item_id(menu_id, {"Outdent"}));
	requests = ui.drain();
	const EditorRequest *outdent = only(requests, EditorRequestKind::EditRecord);
	CHECK(outdent && outdent->edit.operation == EditOperation::Move && outdent->edit.parent == main.child &&
	              outdent->edit.position == 2,
	      "Outdent: right after the window that held it");
	ui.activate(item_id(menu_id, {"Duplicate screen"}));
	requests = ui.drain();
	const EditorRequest *copy_screen = only(requests, EditorRequestKind::EditRecord);
	const NodeAddress screen_address{screen.id, kScreen, 0};
	CHECK(copy_screen && copy_screen->edit.operation == EditOperation::Duplicate &&
	              copy_screen->edit.address == screen_address && copy_screen->edit.position == 1,
	      "Duplicate screen: right after it");

	// Ctrl+C / X / V on the focused window: the session's clipboard requests.
	select_in(v, back);
	ui.frames(2);
	ui.drain();
	auto chord = [&](ImGuiKey key) {
		ui.key(ImGuiMod_Ctrl, true);
		ui.key(key, true);
		ui.key(key, false);
		ui.key(ImGuiMod_Ctrl, false);
		return ui.drain();
	};
	requests = chord(ImGuiKey_C);
	CHECK(only(requests, EditorRequestKind::Copy) && only(requests, EditorRequestKind::Copy)->path == document->path(),
	      "Ctrl+C copies");
	requests = chord(ImGuiKey_X);
	CHECK(only(requests, EditorRequestKind::Cut) != nullptr, "Ctrl+X cuts");
	requests = chord(ImGuiKey_V);
	CHECK(!only(requests, EditorRequestKind::Paste), "nothing to paste while the clipboard is empty");
	requests = chord(ImGuiKey_D);
	CHECK(only(requests, EditorRequestKind::Duplicate) != nullptr, "Ctrl+D duplicates");
	v.clipboard = "\xEF\xBB\xBF<SCREEN></SCREEN>";
	ui.frames();
	// Where a Paste goes is the window's to say: after the selected window among its siblings.
	auto pasted_at = [&](const std::vector<EditorRequest> &raised, NodeId parent, size_t position) {
		const EditorRequest *paste = only(raised, EditorRequestKind::Paste);
		return paste && paste->path == document->path() && paste->edit.address.row == screen.id &&
		       paste->edit.parent == parent && paste->edit.position == position;
	};
	requests = chord(ImGuiKey_V);
	CHECK(pasted_at(requests, main.child, 1), "Ctrl+V pastes after the selected window");
	ui.activate(item_id(menu_id, {"Paste"}));
	requests = ui.drain();
	CHECK(pasted_at(requests, main.child, 1), "the Paste button");
	// A record of a window's list selected (BACK's second ACTION): after BACK, never at the
	// action's index among BACK's child windows.
	select_in(v, menu_test::child_of(*document, back, "action", 1));
	ui.frames(2);
	ui.drain();
	ui.activate(item_id(menu_id, {"Paste"}));
	requests = ui.drain();
	CHECK(pasted_at(requests, main.child, 1), "a list row selected: after the window holding it");
	// The screen selected: at the end of its root windows.
	select_in(v, {screen.id, kScreen, 0});
	ui.frames(2);
	ui.drain();
	ui.activate(item_id(menu_id, {"Paste"}));
	requests = ui.drain();
	CHECK(pasted_at(requests, 0, SIZE_MAX), "the screen selected: after its last root window");
}

// The inspector's tables through the window: a collection's Add, a selected row's Up and
// Duplicate, a switch and a choice edited in a cell, a block's switch; several windows'
// shared fields (S9k2).
void test_inspector_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_inspector_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const NodeAddress back = named(*document, "BACK");
	SessionView v = menu_view(document);
	select_in(v, back);
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.drain();
	CHECK(ui.windows.pending_requests() == 0, "drawing the inspector raises nothing");
	const ImGuiID inspector = Ui::window_id("Inspector");
	// A section of its own: PushID(its key), then the collection's PushID(its kind token).
	const ImGuiID actions = item_id(inspector, {"action", "action"});
	ui.activate(item_id(actions, {"Add"}));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *add = only(requests, EditorRequestKind::EditRecord);
	CHECK(add && add->edit.operation == EditOperation::Add && add->edit.address.kind == menu_kind("action") &&
	              add->edit.parent == back.child,
	      "a collection's Add goes into the record shown");

	// A row of it selected: the leaf's form, and the rows beside it with their toolbar.
	const NodeAddress second = menu_test::child_of(*document, back, "action", 1);
	select_in(v, second);
	ui.frames(3);
	ui.drain();
	ui.activate(item_id(actions, {"Up"}));
	requests = ui.drain();
	const EditorRequest *up = only(requests, EditorRequestKind::EditRecord);
	CHECK(up && up->edit.operation == EditOperation::Move && up->edit.address == second && up->edit.position == 0,
	      "Up moves the selected row");
	ui.activate(item_id(actions, {"Duplicate"}));
	requests = ui.drain();
	const EditorRequest *duplicate = only(requests, EditorRequestKind::EditRecord);
	CHECK(duplicate && duplicate->edit.operation == EditOperation::Duplicate && duplicate->edit.position == 2,
	      "Duplicate puts the copy after it");

	// In place: the hotkey's Virtual key switch, and the second action's verb.
	select_in(v, back);
	ui.frames(3);
	ui.drain();
	const NodeAddress hotkey = menu_test::child_of(*document, back, "hotkey");
	const ImGuiID hotkeys = item_id(inspector, {"hotkey", "hotkey", "records"});
	ui.activate(item_id(pushed(hotkeys, static_cast<int>(hotkey.child)), {"virtual", "##value"}));
	requests = ui.drain();
	const EditorRequest *virtual_key = only(requests, EditorRequestKind::EditRecord);
	CHECK(virtual_key && virtual_key->edit.address == hotkey && virtual_key->edit.field == "virtual" &&
	              std::get<int64_t>(virtual_key->edit.value) == 1,
	      "a switch in a cell sets the row's field");
	const ImGuiID verbs = item_id(inspector, {"action", "action", "records"});
	ui.activate(item_id(pushed(verbs, static_cast<int>(second.child)), {"type", "##value"}));
	// Each choice is an item under its place in the field's list (two of one name are two).
	int pop_screen = -1;
	for (const FieldSchema &schema : document->fields(second.kind))
		if (schema.id == "type")
			for (size_t i = 0; i < schema.choices.size(); ++i)
				if (schema.choices[i].name == "POP_SCREEN") pop_screen = static_cast<int>(i);
	CHECK(pop_screen >= 0, "POP_SCREEN is a choice of an ACTION's type");
	ui.activate(item_id(pushed(ImHashStr("##Combo_00"), pop_screen), {"Go back"}));
	requests = ui.drain();
	const EditorRequest *verb = only(requests, EditorRequestKind::EditRecord);
	CHECK(verb && verb->edit.address == second && verb->edit.field == "type" &&
	              std::get<std::string>(verb->edit.value) == "POP_SCREEN",
	      "a choice picked by its readable name writes its token");

	// TITLE's STRING switch, the first row of its block's form: the block left out.
	select_in(v, named(*document, "TITLE"));
	ui.frames(3);
	ui.drain();
	ui.activate(item_id(inspector, {"string", "fields", "string", "##value"}));
	requests = ui.drain();
	const EditorRequest *block = only(requests, EditorRequestKind::EditRecord);
	CHECK(block && block->edit.field == "string" && std::get<int64_t>(block->edit.value) == 0, "a block's switch");

	// S9k2: BACK and TITLE selected together (BACK the primary): the fields they share, the
	// type marked mixed; a switch sets both in one batch.
	const NodeAddress title = named(*document, "TITLE");
	v.selection = back;
	v.selected = {back, title};
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("2 Window records selected") != std::string::npos && text.find("(mixed)") != std::string::npos,
	      "the shared form says how many and marks what differs");
	CHECK(text.find("Actions") == std::string::npos, "each record's lists stay with its own form");
	ui.activate(item_id(inspector, {"", "fields", "hidden", "##value"}));
	requests = ui.drain();
	const EditorRequest *both = only(requests, EditorRequestKind::EditRecord);
	CHECK(both && both->edits.size() == 2 && both->edits[0].address == back && both->edits[1].address == title &&
	              both->edits[0].field == "hidden" && std::get<int64_t>(both->edits[1].value) == 1,
	      "a shared field's change is one batch over every selected window");
	const std::vector<InspectorSection> shared = plan_shared_inspector(*document, {back, title}, "");
	CHECK(!shared.empty() && !has_field(&shared.front(), "name") && has_field(&shared.front(), "type") &&
	              section_of(shared, "position") && !section_of(shared, "action"),
	      "the plan: no name, the general fields and the groups, no lists");
	CHECK(field_mixed(*document, {back, title}, "type") && !field_mixed(*document, {back, title}, "hidden"),
	      "mixed where they differ");
}

// S11a: what acts on the files as saved waits for the frame's edits. In one frame the
// inspector sets a field and Ctrl+S goes down: the drained requests put the EditRecord
// before the Save, though the menu bar that raises the Save draws first. Ctrl+Shift+Z is
// Redo and Ctrl+Z Undo; while a text field has the keyboard Ctrl+S still saves and Ctrl+Z
// is the field's own. A Save names the document active when it was raised; no shortcut
// acts while the unsaved prompt is open.
void test_actions_after_edits() {
	editor_test::TempProjectDir dir("opennova_editor_ui_actions_test");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	const NodeAddress back = named(*document, "BACK");
	SessionView v = menu_view(document);
	select_in(v, back);
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.drain();
	const auto chord = [&](std::initializer_list<ImGuiKey> keys) {
		for (const ImGuiKey key : keys) ui.key(key, true);
		for (auto key = std::rbegin(keys); key != std::rend(keys); ++key) ui.key(*key, false);
		return ui.drain();
	};
	std::vector<EditorRequest> requests = chord({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_Z});
	CHECK(requests.size() == 1 && requests[0].kind == EditorRequestKind::Redo, "Ctrl+Shift+Z redoes");
	requests = chord({ImGuiMod_Ctrl, ImGuiKey_Z});
	CHECK(requests.size() == 1 && requests[0].kind == EditorRequestKind::Undo, "Ctrl+Z undoes");

	// One frame: the inspector's Hidden switch pressed, and Ctrl+S down.
	const ImGuiID inspector = Ui::window_id("Inspector");
	ImGui::ActivateItemByID(item_id(inspector, {"", "fields", "hidden", "##value"}));
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_S, true);
	ui.frames();
	requests = ui.drain();
	CHECK(requests.size() == 2 && requests[0].kind == EditorRequestKind::EditRecord && requests[0].edit.field == "hidden" &&
	              requests[1].kind == EditorRequestKind::Save,
	      "the frame's edit is raised before the frame's Save");
	ui.key(ImGuiKey_S, false);
	ui.key(ImGuiMod_Ctrl, false);
	ui.drain();

	// A text field with the keyboard (activated for input, as Enter on it does): Ctrl+S
	// saves, Ctrl+Z is the field's.
	ImGui::ActivateItemByID(item_id(inspector, {"", "fields", "name", "##value"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	CHECK(ImGui::GetIO().WantTextInput, "the name field has the keyboard");
	ui.drain();
	requests = chord({ImGuiMod_Ctrl, ImGuiKey_S});
	CHECK(requests.size() == 1 && requests[0].kind == EditorRequestKind::Save && requests[0].path == document->path(),
	      "Ctrl+S saves the active document, named, while typing");
	requests = chord({ImGuiMod_Ctrl, ImGuiKey_Z});
	CHECK(!only(requests, EditorRequestKind::Undo), "Ctrl+Z is the text field's own while typing");
	ImGui::ClearActiveID();
	ui.frames(2);
	CHECK(ui.windows.pending_requests() == 0, "nothing else");

	// A Save raised without a path names the document active when it was raised: Ctrl+S and
	// a Problems row opening another menu in one frame save this menu, not that one, though
	// the Save waits for the row's request.
	auto other = std::make_shared<MnuDocument>();
	Diagnostic error;
	CHECK(editor_test::write_text(dir.file("extra.mnu"), kMenu), "second menu fixture");
	CHECK(other->load(dir.file("extra.mnu"), "extra.mnu", AssetKind::Menu, "jo", error), error.message.c_str());
	v.documents.push_back(other);
	// A Problems row opens a file of the project the editor opens (problem_location): the
	// scan lists this one.
	AssetEntry other_entry;
	other_entry.logical_name = "extra.mnu";
	other_entry.relative_path = other->path();
	other_entry.kind = AssetKind::Menu;
	v.scan.entries.push_back(other_entry);
	v.scan.index();
	// A required file the project lacks names no file of it: its row (showing the file it is
	// about) opens nothing.
	Diagnostic lacking = make_diagnostic(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	lacking.role = "gametext";
	lacking.target = "gametext.bin";
	v.diagnostics = {lacking};
	v.revisions.touch(ViewConcern::Documents);
	v.revisions.touch(ViewConcern::Files);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.focus("Problems");
	problems_grouping(ui, "None");
	ui.drain();
	ui.click(problems_lines().at(0, 2));
	CHECK(ui.drain().empty(), "a required file the project lacks opens nothing");
	// S12: a finding about a file the editor does not open (a font): its row shows it in Files.
	AssetEntry font_entry;
	font_entry.logical_name = "Arial14b.fnt";
	font_entry.relative_path = "fonts/Arial14b.fnt";
	font_entry.kind = AssetKind::Font;
	v.scan.entries.push_back(font_entry);
	v.scan.index();
	v.diagnostics = {make_diagnostic(DiagnosticSeverity::Warning, "graph.unreadable", "The font could not be read.",
	                                 font_entry.relative_path)};
	v.revisions.touch(ViewConcern::Files);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.focus("Problems");
	ui.drain();
	ui.click(problems_lines().at(0, 2));
	requests = ui.drain();
	CHECK(requests.size() == 1 && requests[0].kind == EditorRequestKind::ShowInFiles &&
	              requests[0].path == font_entry.relative_path && !requests[0].flag,
	      "a font's row shows it in Files");
	Diagnostic finding = make_diagnostic(DiagnosticSeverity::Error, "menu.test", "A finding in the other menu.", other->path());
	finding.row_id = other->rows()[0]->id;
	finding.record_kind = kScreen;
	v.diagnostics = {finding};
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.focus("Problems");
	ui.drain();
	// Pressed on the row, then released in the frame Ctrl+S goes down (the keys queued first:
	// ImGui holds a key queued after a mouse button's change for the next frame).
	const ImVec2 row = problems_lines().at(0, 2);
	ui.mouse(row.x, row.y);
	ui.button(true);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_S, true);
	ImGui::GetIO().AddMouseButtonEvent(0, false);
	ui.frames();
	requests = ui.drain();
	CHECK(requests.size() == 2 && requests[0].kind == EditorRequestKind::OpenDocument && requests[0].path == other->path() &&
	              requests[1].kind == EditorRequestKind::Save && requests[1].path == document->path(),
	      "the frame's open of another menu, then the Save of the menu that was active");
	ui.key(ImGuiKey_S, false);
	ui.key(ImGuiMod_Ctrl, false);
	ui.drain();

	// While the unsaved prompt is open no shortcut acts behind it (an Undo would make a file
	// it does not list unsaved); the prompt says what waits and on which file.
	v.unsaved_prompt.open = true;
	v.unsaved_prompt.action = EditorRequestKind::Quit;
	v.unsaved_prompt.files = {document->path()};
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(2);
	ui.drain();
	CHECK(chord({ImGuiMod_Ctrl, ImGuiKey_Z}).empty() && chord({ImGuiMod_Ctrl, ImGuiKey_S}).empty() &&
	              chord({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_Z}).empty() && chord({ImGuiKey_F5}).empty(),
	      "no shortcut while the prompt is open");
	const std::string prompt = logged_frame(ui);
	CHECK(prompt.find("Quit") != std::string::npos && prompt.find(document->path()) != std::string::npos &&
	              prompt.find("Save all") != std::string::npos && prompt.find("Discard") != std::string::npos,
	      "the prompt names what waits, its file and its answers");
	v.unsaved_prompt = SessionView::UnsavedPrompt();
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(2);
	ui.drain();
}

// --- S9k1: the preview window's canvas --------------------------------------------------

// MAIN over the whole design, BOX, OTHER and the 12-unit TINY inside it, every edge written.
const char *const kLayoutMenu =
        "<SCREEN>\r\n"
        "\t<NAME>LAYOUT</NAME>\r\n"
        "\t<WINDOW type=\"window\" name=\"MAIN\">\r\n"
        "\t\t<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>\r\n"
        "\t\t<WINDOW type=\"window\" name=\"BOX\">\r\n"
        "\t\t\t<POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>200</BOTTOM></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"checkbox\" name=\"OTHER\">\r\n"
        "\t\t\t<POSITION><LEFT>400</LEFT><TOP>300</TOP><RIGHT>600</RIGHT><BOTTOM>400</BOTTOM></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t\t<WINDOW type=\"checkbox\" name=\"TINY\">\r\n"
        "\t\t\t<POSITION><LEFT>600</LEFT><TOP>104</TOP><RIGHT>612</RIGHT><BOTTOM>116</BOTTOM></POSITION>\r\n"
        "\t\t</WINDOW>\r\n"
        "\t</WINDOW>\r\n"
        "</SCREEN>\r\n";

// The edits of a drag's requests: every batch, and the gesture they share (0: none, or
// not one).
std::vector<Edit> batches(const std::vector<EditorRequest> &requests, uint64_t &gesture, size_t &count) {
	std::vector<Edit> last;
	gesture = 0;
	count = 0;
	bool one = true;
	for (const EditorRequest &request : requests) {
		if (request.kind != EditorRequestKind::EditRecord) continue;
		++count;
		for (const Edit &edit : request.edits) {
			one = one && edit.gesture != 0 && (gesture == 0 || edit.gesture == gesture);
			gesture = edit.gesture;
		}
		last = request.edits;
	}
	if (!one) gesture = 0;
	return last;
}

int64_t set_value(const std::vector<Edit> &edits, const char *field) {
	for (const Edit &edit : edits)
		if (edit.operation == EditOperation::Set && edit.field == field) return std::get<int64_t>(edit.value);
	return -1;
}

// The canvas through the window: a click selects what the game's hit test finds, a drag of
// the selected window and of a corner handle writes its POSITION as Sets sharing one
// gesture then ends it, the arrows nudge, Esc selects the parent, a stale picture maps
// nothing; the zoom; the state options follow the selection; each empty state says why.
void test_preview_window_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_test");
	auto document = std::make_shared<MnuDocument>();
	Diagnostic error;
	CHECK(editor_test::write_text(dir.file("layout.mnu"), kLayoutMenu), "layout fixture");
	CHECK(document->load(dir.file("layout.mnu"), "layout.mnu", AssetKind::Menu, "jo", error), error.message.c_str());
	if (document->rows().empty()) return;
	const Node &screen = *document->rows()[0];
	FakePreview fake;
	CHECK(fake.render.configure(*document, screen.id, fake.files, {}) == MenuPreviewStatus::Ready, "the screen renders");
	SessionView v = menu_view(document);
	v.menu_preview.path = document->path();
	v.menu_preview.screen = screen.id;
	select_in(v, {screen.id, kScreen, 0});
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_menu_preview_viewport(&fake);
	ui.frames(6);
	ui.focus("Preview");
	ui.drain();
	CHECK(ui.windows.pending_requests() == 0, "drawing the preview raises nothing");
	CHECK(fake.width > 0 && fake.width * 3 == fake.height * 4, "Fit: the design's 4:3");
	// 100% from the toolbar: a pixel is a design unit (ImGui floors the mouse to pixels).
	const ImGuiID preview_id = item_id(Ui::window_id("Preview"), {"menu"}); // the menu pane's scope
	ui.activate(item_id(preview_id, {"Zoom"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"100%"}));
	ui.frames(2);
	CHECK(fake.width == 800 && fake.height == 600, "100%");
	ui.drain();
	const NodeAddress main = named(*document, "MAIN"), box = named(*document, "BOX"), other = named(*document, "OTHER");
	auto at = [&](float x, float y) {
		return ImVec2(fake.origin.x + x * float(fake.width) / 800.0f, fake.origin.y + y * float(fake.height) / 600.0f);
	};
	auto click = [&](ImVec2 p) {
		ui.mouse(p.x, p.y);
		ui.button(true);
		ui.button(false);
		return ui.drain();
	};
	// A drag from `from` by (dx, dy) design units in steps, released there.
	auto drag = [&](ImVec2 from, float dx, float dy) {
		ui.mouse(from.x, from.y);
		ui.button(true);
		const float px = dx * float(fake.width) / 800.0f, py = dy * float(fake.height) / 600.0f;
		for (int step = 1; step <= 4; ++step) ui.mouse(from.x + px * float(step) / 4.0f, from.y + py * float(step) / 4.0f);
		ui.button(false);
		ui.frames();
		return ui.drain();
	};

	// A click selects the window the game's hit test finds.
	std::vector<EditorRequest> requests = click(at(200.0f, 150.0f));
	const EditorRequest *picked = only(requests, EditorRequestKind::SelectRecord);
	CHECK(picked && picked->edit.address == box && picked->select_mode == SelectMode::Replace, "a click selects BOX");
	CHECK(!only(requests, EditorRequestKind::EditRecord), "a click edits nothing");

	// BOX selected: dragged by (40, 20), snapped: one gesture of Sets, then its end.
	select_in(v, box);
	ui.frames(2);
	ui.drain();
	requests = drag(at(200.0f, 150.0f), 40.0f, 21.0f);
	uint64_t gesture = 0;
	size_t count = 0;
	std::vector<Edit> last = batches(requests, gesture, count);
	CHECK(count >= 2 && gesture != 0, "a drag's steps share one gesture");
	CHECK(set_value(last, "position.left") == 144 && set_value(last, "position.right") == 344 &&
	              set_value(last, "position.top") == 120 && set_value(last, "position.bottom") == 220,
	      "the last step: moved and snapped on the grid of 8");
	CHECK(!requests.empty() && requests.back().kind == EditorRequestKind::EndEdit && requests.back().path == document->path(),
	      "release ends the gesture");
	CHECK(!only(requests, EditorRequestKind::SelectRecord), "the selected window moves without a new selection");

	// Its bottom-right handle, Alt held: resized, not snapped.
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, true);
	requests = drag(at(300.0f, 200.0f), 13.0f, 7.0f);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, false);
	ui.frames();
	last = batches(requests, gesture, count);
	CHECK(gesture != 0 && set_value(last, "position.right") == 313 && set_value(last, "position.bottom") == 207 &&
	              set_value(last, "position.left") == -1 && set_value(last, "position.top") == -1,
	      "a corner handle resizes its two edges");

	// A drag that starts on another window selects it and moves it.
	requests = drag(at(500.0f, 350.0f), -8.0f, 0.0f);
	const EditorRequest *took = only(requests, EditorRequestKind::SelectRecord);
	last = batches(requests, gesture, count);
	CHECK(took && took->edit.address == other && set_value(last, "position.left") == 392, "OTHER picked and moved");

	// A window 12 pixels across: pressed in its middle it moves (every point of it is within
	// a handle's reach of a corner, so the handles keep out of its middle); its corner still
	// resizes it.
	const NodeAddress tiny = named(*document, "TINY");
	select_in(v, tiny);
	ui.frames(2);
	ui.drain();
	requests = drag(at(606.0f, 110.0f), 16.0f, 0.0f);
	last = batches(requests, gesture, count);
	CHECK(gesture != 0 && set_value(last, "position.left") == 616 && set_value(last, "position.right") == 628 &&
	              set_value(last, "position.top") == -1 && set_value(last, "position.bottom") == -1,
	      "the middle of a small window moves it, its size kept");
	CHECK(!only(requests, EditorRequestKind::SelectRecord), "the selected small window moves without a new selection");
	requests = drag(at(612.0f, 116.0f), 12.0f, 12.0f);
	last = batches(requests, gesture, count);
	CHECK(gesture != 0 && set_value(last, "position.right") == 624 && set_value(last, "position.bottom") == 128 &&
	              set_value(last, "position.left") == -1 && set_value(last, "position.top") == -1,
	      "a small window's corner still resizes it");
	select_in(v, box);
	ui.frames(2);
	ui.drain();

	// The arrows nudge the selected window (Shift: 8), one gesture while held.
	auto nudge = [&](ImGuiKey key, bool shift) {
		if (shift) ui.key(ImGuiMod_Shift, true);
		ui.key(key, true);
		ui.key(key, false);
		if (shift) ui.key(ImGuiMod_Shift, false);
		return ui.drain();
	};
	requests = nudge(ImGuiKey_RightArrow, false);
	last = batches(requests, gesture, count);
	CHECK(count == 1 && gesture != 0 && set_value(last, "position.left") == 101 && set_value(last, "position.right") == 301,
	      "Right moves BOX a unit");
	CHECK(requests.back().kind == EditorRequestKind::EndEdit, "letting go ends the nudge");
	requests = nudge(ImGuiKey_UpArrow, true);
	last = batches(requests, gesture, count);
	CHECK(set_value(last, "position.top") == 92 && set_value(last, "position.bottom") == 192, "Shift+Up moves it 8");

	// Esc selects what holds it.
	requests = nudge(ImGuiKey_Escape, false);
	const EditorRequest *parent = only(requests, EditorRequestKind::SelectRecord);
	CHECK(parent && parent->edit.address == main, "Esc selects MAIN");

	// The held state follows the selection; Checked only where the type has one.
	MenuPreviewOptions options;
	options.force_state = opennova::menu::kStateMouseover;
	options.force_window = box.child;
	fake.held = options;
	select_in(v, other);
	ui.frames(2);
	CHECK(fake.held.force_window == other.child && fake.held.force_state == opennova::menu::kStateMouseover,
	      "the held state moves to the selected window");
	ui.activate(item_id(preview_id, {"Checked"}));
	CHECK(fake.held.checked && fake.held.force_window == other.child, "a check box can be held checked");
	select_in(v, box);
	ui.frames(2);
	CHECK(fake.held.force_window == box.child && !fake.held.checked, "a plain window lets the check go");
	ui.drain();

	// A picture of another revision maps nothing.
	fake.stale = true;
	requests = click(at(500.0f, 350.0f));
	CHECK(!only(requests, EditorRequestKind::SelectRecord), "a stale picture selects nothing");
	requests = drag(at(200.0f, 150.0f), 40.0f, 0.0f);
	CHECK(!only(requests, EditorRequestKind::EditRecord), "a stale picture drags nothing");
	fake.stale = false;

	// The zoom: 200% from the toolbar; Ctrl+wheel over the picture to the next level.
	ui.activate(item_id(preview_id, {"Zoom"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"200%"}));
	ui.frames(2);
	CHECK(fake.width == 1600 && fake.height == 1200, "200%");
	const ImVec2 inside_canvas(ImGui::FindWindowByName("Preview")->Pos.x + 40.0f, fake.origin.y + 40.0f);
	ui.mouse(inside_canvas.x, inside_canvas.y);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
	ui.frames(2);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
	ui.frames(2);
	CHECK(fake.width == 2400 && fake.height == 1800, "Ctrl+wheel zooms in to 300%");
	ui.drain();

	// Each empty state says why.
	const MenuPreviewStatus empty[] = {MenuPreviewStatus::NoProject, MenuPreviewStatus::NoMenu, MenuPreviewStatus::NoScreen,
	                               MenuPreviewStatus::Unserializable, MenuPreviewStatus::ScreenMissing};
	for (const MenuPreviewStatus status : empty) {
		fake.shown_status = status;
		fake.shown_detail = status == MenuPreviewStatus::ScreenMissing ? "LAYOUT" : "a reason";
		const std::string text = logged_frame(ui);
		const std::string message = menu_preview_status_message(status, fake.shown_detail);
		CHECK(text.find(message) != std::string::npos, message.c_str());
	}
	ui.windows.set_menu_preview_viewport(nullptr);
	CHECK(logged_frame(ui).find("No preview renderer is attached.") != std::string::npos, "no device");
	CHECK(ui.windows.pending_requests() == 0, "the empty states raise nothing");
}

// S11d: a gesture the menu pane began ends once, for the menu it began in, whenever the pane
// stops drawing mid-gesture: the model pane shown (a model made the active document) during
// a drag or a held nudge, and Preview closed during either (the workspace's frame bracket
// ends what a window the pass skipped left open); after that, letting go raises nothing.
// The pane not drawn takes no key: with the model pane shown, an arrow, Esc and Space raise
// nothing.
void test_preview_gestures_end() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_gestures_test");
	auto document = std::make_shared<MnuDocument>();
	Diagnostic error;
	CHECK(editor_test::write_text(dir.file("layout.mnu"), kLayoutMenu), "layout fixture");
	CHECK(document->load(dir.file("layout.mnu"), "layout.mnu", AssetKind::Menu, "jo", error), error.message.c_str());
	auto model = std::make_shared<ModelDocument>();
	CHECK(model->load(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/armory.3di", "models/armory.3di",
	                  AssetKind::Model, "jo", error),
	      "a model");
	if (document->rows().empty()) return;
	const Node &screen = *document->rows()[0];
	FakePreview fake;
	CHECK(fake.render.configure(*document, screen.id, fake.files, {}) == MenuPreviewStatus::Ready, "the screen renders");
	SessionView v = menu_view(document);
	v.menu_preview.path = document->path();
	v.menu_preview.screen = screen.id;
	select_in(v, named(*document, "BOX"));
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_menu_preview_viewport(&fake);
	ui.frames(6);
	ui.focus("Preview");
	ui.activate(item_id(item_id(Ui::window_id("Preview"), {"menu"}), {"Zoom"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"100%"}));
	ui.frames(2);
	ui.drain();
	devtools::Window *preview = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count(); ++i)
		if (std::strcmp(ui.windows.pass().window(i).title(), "Preview") == 0) preview = &ui.windows.pass().window(i);
	CHECK(preview && preview->is_closeable(), "Preview has a close button");
	if (!preview) return;
	// The model the active document, or the menu again.
	const auto show_model = [&](bool on) {
		v.documents = on ? std::vector<std::shared_ptr<const Document>>{document, model}
		                 : std::vector<std::shared_ptr<const Document>>{document};
		v.model_preview.path = on ? model->path() : std::string();
		v.active_document = on ? model->path() : document->path();
		v.revisions.touch(ViewConcern::Documents);
		v.revisions.touch(ViewConcern::Selection);
		ui.frames(2);
	};
	// Exactly one request, the menu's gesture's end.
	const auto one_end = [&](const std::vector<EditorRequest> &requests) {
		return one(requests, EditorRequestKind::EndEdit) && requests[0].path == document->path();
	};
	const ImVec2 box(fake.origin.x + 200.0f, fake.origin.y + 150.0f); // BOX at 100%
	const auto start_drag = [&]() {
		ui.mouse(box.x, box.y);
		ui.button(true);
		ui.mouse(box.x + 24.0f, box.y);
		return only(ui.drain(), EditorRequestKind::EditRecord) != nullptr;
	};
	const auto start_nudge = [&]() {
		ui.key(ImGuiKey_RightArrow, true);
		return only(ui.drain(), EditorRequestKind::EditRecord) != nullptr;
	};

	// The model shown mid-drag, then mid-nudge.
	CHECK(start_drag(), "a drag's first step");
	show_model(true);
	CHECK(one_end(ui.drain()), "the model shown mid-drag: the drag's one end");
	ui.button(false);
	ui.frames(2);
	CHECK(ui.drain().empty(), "letting go raises nothing");
	show_model(false);
	ui.focus("Preview");
	ui.drain();
	CHECK(start_nudge(), "a nudge's first step");
	show_model(true);
	CHECK(one_end(ui.drain()), "the model shown mid-nudge: the nudge's one end");
	ui.key(ImGuiKey_RightArrow, false);
	ui.frames(2);
	CHECK(ui.drain().empty(), "letting go of the arrow raises nothing");
	show_model(false);

	// Preview closed mid-drag, then mid-nudge: the frame bracket ends each once.
	ui.focus("Preview");
	ui.drain();
	CHECK(start_drag(), "a drag's first step");
	preview->open = false;
	ui.frames(2);
	CHECK(one_end(ui.drain()), "Preview closed mid-drag: the drag's one end");
	ui.button(false);
	ui.frames(2);
	preview->open = true;
	ui.frames(3);
	CHECK(ui.drain().empty(), "let go and opened again: nothing");
	ui.focus("Preview");
	ui.drain();
	CHECK(start_nudge(), "a nudge's first step");
	preview->open = false;
	ui.frames(2);
	CHECK(one_end(ui.drain()), "Preview closed mid-nudge: the nudge's one end");
	ui.key(ImGuiKey_RightArrow, false);
	preview->open = true;
	ui.frames(3);
	CHECK(ui.drain().empty(), "let go and opened again: nothing");

	// The menu pane hidden behind the model's: its keys do nothing.
	show_model(true);
	ui.focus("Preview");
	ui.drain();
	for (const ImGuiKey key : {ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_Escape, ImGuiKey_Space}) {
		ui.key(key, true);
		ui.key(key, false);
	}
	CHECK(ui.drain().empty(), "the hidden menu pane takes no key");
}

// S9k2: several windows on the canvas. Shift+click adds a window, Ctrl+click toggles one; a
// drag from the screen's background selects what its box touches (none: the screen); a
// drag of a selected window moves every selected one in one batch per step, one gesture;
// the arrows nudge them all; Ctrl+C / X / V / D; the toolbar's Arrange aligns them.
void test_preview_several_windows_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_preview_multi_test");
	auto document = std::make_shared<MnuDocument>();
	Diagnostic error;
	CHECK(editor_test::write_text(dir.file("layout.mnu"), kLayoutMenu), "layout fixture");
	CHECK(document->load(dir.file("layout.mnu"), "layout.mnu", AssetKind::Menu, "jo", error), error.message.c_str());
	if (document->rows().empty()) return;
	const Node &screen = *document->rows()[0];
	FakePreview fake;
	CHECK(fake.render.configure(*document, screen.id, fake.files, {}) == MenuPreviewStatus::Ready, "the screen renders");
	SessionView v = menu_view(document);
	v.menu_preview.path = document->path();
	v.menu_preview.screen = screen.id;
	const NodeAddress main = named(*document, "MAIN"), box = named(*document, "BOX"), other = named(*document, "OTHER");
	select_in(v, box);
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_menu_preview_viewport(&fake);
	ui.frames(6);
	ui.focus("Preview");
	const ImGuiID preview_id = item_id(Ui::window_id("Preview"), {"menu"}); // the menu pane's scope
	ui.activate(item_id(preview_id, {"Zoom"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"100%"}));
	ui.frames(2);
	ui.drain();
	auto at = [&](float x, float y) {
		return ImVec2(fake.origin.x + x * float(fake.width) / 800.0f, fake.origin.y + y * float(fake.height) / 600.0f);
	};
	auto click = [&](ImVec2 p, ImGuiKey modifier) {
		ui.mouse(p.x, p.y);
		if (modifier != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(modifier, true);
		ui.button(true);
		ui.button(false);
		if (modifier != ImGuiKey_None) ImGui::GetIO().AddKeyEvent(modifier, false);
		ui.frames();
		return ui.drain();
	};
	auto drag = [&](ImVec2 from, float dx, float dy) {
		ui.mouse(from.x, from.y);
		ui.button(true);
		for (int step = 1; step <= 4; ++step) ui.mouse(from.x + dx * float(step) / 4.0f, from.y + dy * float(step) / 4.0f);
		ui.button(false);
		ui.frames();
		return ui.drain();
	};
	auto selections = [&](const std::vector<EditorRequest> &requests) {
		std::vector<std::pair<NodeAddress, SelectMode>> out;
		for (const EditorRequest &request : requests)
			if (request.kind == EditorRequestKind::SelectRecord) out.emplace_back(request.edit.address, request.select_mode);
		return out;
	};
	// Where the batch sets one window's field (-1: it does not).
	auto set_on = [](const std::vector<Edit> &edits, const NodeAddress &window, const char *field) -> int64_t {
		for (const Edit &edit : edits)
			if (edit.operation == EditOperation::Set && edit.address == window && edit.field == field)
				return std::get<int64_t>(edit.value);
		return -1;
	};

	// Shift+click adds OTHER; Ctrl+click toggles BOX; neither edits.
	std::vector<EditorRequest> requests = click(at(500.0f, 350.0f), ImGuiMod_Shift);
	CHECK(selections(requests) == (std::vector<std::pair<NodeAddress, SelectMode>>{{other, SelectMode::Add}}) &&
	              !only(requests, EditorRequestKind::EditRecord),
	      "Shift+click adds a window");
	requests = click(at(200.0f, 150.0f), ImGuiMod_Ctrl);
	CHECK(selections(requests) == (std::vector<std::pair<NodeAddress, SelectMode>>{{box, SelectMode::Toggle}}) &&
	              !only(requests, EditorRequestKind::EditRecord),
	      "Ctrl+click toggles a window");

	// BOX and OTHER selected, OTHER the primary: a drag of BOX moves both, snapped by BOX.
	v.selection = other;
	v.selected = {box, other};
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(2);
	ui.drain();
	requests = drag(at(200.0f, 150.0f), 40.0f, 21.0f);
	uint64_t gesture = 0;
	size_t count = 0;
	std::vector<Edit> last = batches(requests, gesture, count);
	CHECK(count >= 2 && gesture != 0, "the steps share one gesture");
	CHECK(set_on(last, box, "position.left") == 144 && set_on(last, box, "position.top") == 120 &&
	              set_on(last, other, "position.left") == 444 && set_on(last, other, "position.right") == 644 &&
	              set_on(last, other, "position.top") == 320,
	      "one batch moves both windows by BOX's snapped step");
	CHECK(selections(requests) == (std::vector<std::pair<NodeAddress, SelectMode>>{{box, SelectMode::Add}}),
	      "the window dragged becomes the primary, the other stays selected");
	CHECK(!requests.empty() && requests.back().kind == EditorRequestKind::EndEdit, "release ends the gesture");

	// The arrows move both.
	ui.key(ImGuiKey_RightArrow, true);
	ui.key(ImGuiKey_RightArrow, false);
	requests = ui.drain();
	last = batches(requests, gesture, count);
	CHECK(count == 1 && set_on(last, box, "position.left") == 101 && set_on(last, other, "position.left") == 401,
	      "Right moves every selected window a unit");

	// A drag from MAIN's empty part (a root window not selected) selects what the box touches.
	requests = drag(at(50.0f, 500.0f), 400.0f, -390.0f);
	CHECK(selections(requests) == (std::vector<std::pair<NodeAddress, SelectMode>>{{box, SelectMode::Replace},
	                                                                              {other, SelectMode::Add}}) &&
	              !only(requests, EditorRequestKind::EditRecord),
	      "the marquee selects BOX and OTHER, not TINY or MAIN");
	requests = drag(at(20.0f, 500.0f), 40.0f, 60.0f);
	const NodeAddress screen_address{screen.id, node_kind(MenuKind::Screen), 0};
	CHECK(selections(requests) == (std::vector<std::pair<NodeAddress, SelectMode>>{{screen_address, SelectMode::Replace}}),
	      "a box over nothing selects the screen");
	requests = click(at(20.0f, 500.0f), ImGuiKey_None);
	CHECK(selections(requests) == (std::vector<std::pair<NodeAddress, SelectMode>>{{main, SelectMode::Replace}}),
	      "a click on the background still selects it");

	// The clipboard keys.
	auto chord = [&](ImGuiKey key) {
		ui.key(ImGuiMod_Ctrl, true);
		ui.key(key, true);
		ui.key(key, false);
		ui.key(ImGuiMod_Ctrl, false);
		return ui.drain();
	};
	CHECK(only(chord(ImGuiKey_C), EditorRequestKind::Copy) != nullptr, "Ctrl+C copies");
	CHECK(only(chord(ImGuiKey_X), EditorRequestKind::Cut) != nullptr, "Ctrl+X cuts");
	CHECK(only(chord(ImGuiKey_D), EditorRequestKind::Duplicate) != nullptr, "Ctrl+D duplicates");
	CHECK(!only(chord(ImGuiKey_V), EditorRequestKind::Paste), "nothing to paste while the clipboard is empty");
	v.clipboard = "\xEF\xBB\xBF<SCREEN></SCREEN>";
	ui.frames();
	requests = chord(ImGuiKey_V);
	const EditorRequest *paste = only(requests, EditorRequestKind::Paste);
	CHECK(paste && paste->edit.address.row == screen.id && paste->edit.parent == main.child && paste->edit.position == 2,
	      "Ctrl+V pastes after the primary window (OTHER)");
	// Copy, Cut and Duplicate take the selection as it is: with the screen among it (or a
	// window's list row) the preview raises none of them, as the menu view does not.
	v.selected = {screen_address, box, other};
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(2);
	ui.drain();
	CHECK(!only(chord(ImGuiKey_C), EditorRequestKind::Copy) && !only(chord(ImGuiKey_X), EditorRequestKind::Cut) &&
	              !only(chord(ImGuiKey_D), EditorRequestKind::Duplicate),
	      "the screen selected with the windows: no Copy, Cut or Duplicate");
	v.selected = {box, other};
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(2);
	ui.drain();

	// Arrange from the toolbar: BOX's left edge to the primary OTHER's, one batch.
	ui.activate(item_id(preview_id, {"Arrange"}));
	ui.activate(popup_item(item_id(preview_id, {"arrange"}), "Align left edges"));
	requests = ui.drain();
	const EditorRequest *aligned = only(requests, EditorRequestKind::EditRecord);
	CHECK(aligned && set_on(aligned->edits, box, "position.left") == 400 && set_on(aligned->edits, box, "position.right") == 600 &&
	              set_on(aligned->edits, other, "position.left") == -1,
	      "Align left edges: BOX to OTHER's left, one batch");
	ui.activate(item_id(preview_id, {"Arrange"}));
	ui.activate(popup_item(item_id(preview_id, {"arrange"}), "Bring to front"));
	requests = ui.drain();
	const EditorRequest *front = only(requests, EditorRequestKind::EditRecord);
	const NodeAddress tiny = named(*document, "TINY");
	CHECK(front && front->edits.size() == 1 && front->edits[0].operation == EditOperation::Move &&
	              front->edits[0].address == tiny && front->edits[0].position == 0,
	      "Bring to front of BOX and OTHER: one Move, TINY before them");

	// MAIN and OTHER selected, OTHER the primary: a press on BOX (not selected, but inside
	// MAIN's rect) is a press inside a selected window, so the drag moves MAIN (OTHER rides
	// inside it), and MAIN becomes the primary with OTHER still selected.
	v.selection = other;
	v.selected = {main, other};
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(2);
	ui.drain();
	requests = drag(at(200.0f, 150.0f), 16.0f, 8.0f);
	last = batches(requests, gesture, count);
	CHECK(gesture != 0 && set_on(last, main, "position.left") == 16 && set_on(last, main, "position.top") == 8 &&
	              set_on(last, box, "position.left") == -1 && set_on(last, other, "position.left") == -1,
	      "a press on a window inside a selected one moves the selection");
	CHECK(selections(requests) == (std::vector<std::pair<NodeAddress, SelectMode>>{{main, SelectMode::Add}}),
	      "the selected window pressed in becomes the primary, the other stays selected");
	CHECK(ui.windows.pending_requests() == 0, "nothing else");
}

// --- S11c: the Problems window ------------------------------------------------------------

// The row that wraps whole controls and the text cut to a width: in a window 220 wide, six
// buttons wrap onto more lines and none runs past its edge; each control is as wide as its
// measure says; a text is cut with "...", its first line only, and to what fits of it when
// even the "..." does not; a fitted button stays within its width.
void test_ui_kit() {
	NullBackend backend;
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(220.0f, 400.0f));
	ImGui::Begin("kit");
	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	ui_kit::WrapRow row;
	float widest = 0.0f;
	std::vector<float> tops;
	for (int i = 0; i < 6; ++i) {
		const std::string label = "Button " + std::to_string(i);
		row.next(ui_kit::button_width(label.c_str()));
		ImGui::Button(label.c_str());
		CHECK(std::fabs(ImGui::GetItemRectSize().x - ui_kit::button_width(label.c_str())) < 0.5f, "a button is as wide as measured");
		widest = std::max(widest, ImGui::GetItemRectMax().x);
		tops.push_back(ImGui::GetItemRectMin().y);
	}
	CHECK(widest <= right, "no button runs past the window's edge");
	CHECK(tops[1] == tops[0] && tops.back() > tops.front(), "whole buttons wrap onto the next line");
	bool flag = false;
	ImGui::Checkbox("Only fixable", &flag);
	CHECK(std::fabs(ImGui::GetItemRectSize().x - ui_kit::checkbox_width("Only fixable")) < 0.5f, "a check box's measure");
	ImGui::SetNextItemWidth(100.0f);
	if (ImGui::BeginCombo("Scope", "Project")) ImGui::EndCombo();
	CHECK(std::fabs(ImGui::GetItemRectSize().x - ui_kit::field_width(100.0f, "Scope")) < 0.5f, "a field and its label");
	const std::string text = "A finding whose message runs much longer than the cell it is drawn in.";
	const std::string cut = ui_kit::fit(text, 100.0f);
	CHECK(cut.size() > 3 && cut.compare(cut.size() - 3, 3, "...") == 0 && ImGui::CalcTextSize(cut.c_str()).x <= 100.0f &&
	              text.compare(0, cut.size() - 3, cut, 0, cut.size() - 3) == 0,
	      "cut to the width, the rest an ellipsis");
	CHECK(ui_kit::fit("Short.", 100.0f) == "Short.", "a text that fits stays whole");
	CHECK(ui_kit::fit("One line.\nAnother.", 200.0f) == "One line....", "the first line only");
	const float dots = ImGui::CalcTextSize("...").x;
	const std::string narrow = ui_kit::fit(text, dots - 1.0f);
	CHECK(narrow.find("...") == std::string::npos && ImGui::CalcTextSize(narrow.c_str()).x <= dots - 1.0f &&
	              ui_kit::fit(text, 0.0f).empty(),
	      "narrower than the ellipsis: what fits of the text alone");
	ui_kit::fitted_button(text, "long", 90.0f);
	CHECK(ImGui::GetItemRectSize().x <= 90.0f, "a fitted button stays within its width");
	ImGui::End();
	ImGui::Render();
}

RequirementRow missing_row(const char *role, const char *name, AssetKind kind, bool required = true) {
	RequirementRow row;
	row.role = role;
	row.name = name;
	row.required = required;
	row.expected_kind = kind;
	row.state = RequirementState::Missing;
	return row;
}

Diagnostic missing_finding(const char *role, const char *name) {
	Diagnostic d = make_diagnostic(DiagnosticSeverity::Error, "requirement.missing", std::string("Missing required file ") + name + ".");
	d.role = role;
	d.target = name;
	return d;
}

constexpr const char *kGametext = "Missing required file gametext.bin";
constexpr const char *kMainMenu = "Missing required file main.mnu";

// Two required files the project lacks, each made by a factory (the game data has
// gametext.bin; a spare string table and two menus could stand in for them), a catalog
// error on a record's field, a warning in the active menu and a note in the other open one.
SessionView problems_view(const std::shared_ptr<MnuDocument> &a, const std::shared_ptr<MnuDocument> &b) {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Problems";
	v.scan.entries = {file_entry("items.def", "defs/items.def", AssetKind::ItemDefs), file_entry("a.mnu", "menus/a.mnu", AssetKind::Menu),
	                  file_entry("b.mnu", "menus/b.mnu", AssetKind::Menu),
	                  file_entry("spare.bin", "strings/spare.bin", AssetKind::Strings)};
	v.scan.index();
	v.requirements.rows = {missing_row("gametext", "gametext.bin", AssetKind::Strings),
	                       missing_row("main_menu", "main.mnu", AssetKind::Menu)};
	v.requirements.required_total = 2;
	v.requirements.required_missing = 2;
	v.retail_files = {"gametext.bin"};
	v.documents = {a, b};
	v.active_document = a->path();
	Diagnostic type = make_diagnostic(DiagnosticSeverity::Error, "catalog.item_type", "Alpha: choose an item type.",
	                                  "defs/items.def", "type");
	type.record = "Marker";
	type.line = 12;
	type.row_id = 4;
	type.record_kind = 2;
	v.diagnostics = {missing_finding("gametext", "gametext.bin"), missing_finding("main_menu", "main.mnu"), type,
	                 make_diagnostic(DiagnosticSeverity::Warning, "menu.duplicate_window", "Bravo: two windows are named GO.",
	                                 "menus/a.mnu"),
	                 make_diagnostic(DiagnosticSeverity::Info, "style.unused", "Charlie: nothing uses it.", "menus/b.mnu")};
	return v;
}

// Which of the five findings a frame of Problems lists, in its order (its log).
std::vector<std::string> listed(Ui &ui) {
	const std::string text = logged_frame(ui);
	std::vector<std::pair<size_t, std::string>> found;
	for (const char *message : {kGametext, kMainMenu, "Alpha:", "Bravo:", "Charlie:"})
		if (const size_t at = text.find(message); at != std::string::npos) found.emplace_back(at, message);
	std::sort(found.begin(), found.end());
	std::vector<std::string> out;
	for (const auto &entry : found) out.push_back(entry.second);
	return out;
}

// The Problems window over problems_view, driven by the mouse where a user presses and by
// ids where the harness allows: grouped by kind at first, a group of notes alone folded
// (S11e); the severity toggles, the text, the scope and the grouping change what it lists; a
// group's header folds it away; a click on a required file's row opens nothing and shows
// every fix (a second click folds it back), one on a catalog finding opens its record at the
// field; a required file's Fix creates it, its More lists the others, a Use fix among them
// waiting for Apply; the Required files group's Fix all asks (Cancel raises nothing, Apply
// one Create naming every role); the summary's buttons ask and raise one request each; an
// optional file's note has its fixes too.
void test_problems_window_ui() {
	using List = std::vector<std::string>;
	editor_test::TempProjectDir dir("opennova_editor_ui_problems_test");
	SessionView v = problems_view(menu_at(dir, "a.mnu", "menus/a.mnu"), menu_at(dir, "b.mnu", "menus/b.mnu"));
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	ui.drain();
	CHECK(ui.windows.pending_requests() == 0, "drawing Problems raises nothing");
	const ImGuiID window = Ui::window_id("Problems");
	const auto pick = [&](const char *combo, const char *choice) {
		ui.activate(item_id(window, {combo}));
		ui.activate(item_id(ImHashStr("##Combo_00"), {choice}));
	};

	// First grouped by kind, the stylesheet's note alone in its group, folded away.
	CHECK(in_order(logged_frame(ui), {"Required files (2 errors)", kGametext, kMainMenu, "Catalogs (1 error)", "Alpha:",
	                                  "Menus (1 warning)", "Bravo:", "Stylesheets (1 info)"}) &&
	              listed(ui) == List({kGametext, kMainMenu, "Alpha:", "Bravo:"}),
	      "grouped by kind, the group of notes alone folded");
	pick("Group", "None");
	CHECK(listed(ui) == List({kGametext, kMainMenu, "Alpha:", "Bravo:", "Charlie:"}), "every finding, errors first");
	std::string text = logged_frame(ui);
	CHECK(in_order(text, {"Errors 3", "Warnings 1", "Info 1", "5 of 5"}), "the counts, every finding's");
	CHECK(text.find("The game cannot start: 2 required files are missing.") != std::string::npos, "the summary");
	CHECK(text.find("items.def:12 - Marker - type") != std::string::npos, "where a finding is");
	CHECK(text.find("Create gametext.bin") != std::string::npos, "a required file's first fix");

	// Each severity hidden and shown again; the counts stay every finding's.
	ui.activate(item_id(window, {"###errors"}));
	CHECK(listed(ui) == List({"Bravo:", "Charlie:"}), "the errors hidden");
	CHECK(in_order(logged_frame(ui), {"Errors 3", "2 of 5"}), "2 of 5");
	ui.activate(item_id(window, {"###errors"}));
	ui.activate(item_id(window, {"###warnings"}));
	CHECK(listed(ui) == List({kGametext, kMainMenu, "Alpha:", "Charlie:"}), "the warnings hidden");
	ui.activate(item_id(window, {"###warnings"}));
	ui.activate(item_id(window, {"###infos"}));
	CHECK(listed(ui) == List({kGametext, kMainMenu, "Alpha:", "Bravo:"}), "the info hidden");
	ui.activate(item_id(window, {"###infos"}));

	// The text, typed into the filter (without case, over the file too); cleared again.
	ImGui::ActivateItemByID(item_id(window, {"##filter"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	CHECK(ImGui::GetIO().WantTextInput, "the filter has the keyboard");
	ImGui::GetIO().AddInputCharactersUTF8("ITEMS.def");
	ui.frames(2);
	CHECK(listed(ui) == List({"Alpha:"}), "the text over the file, without case");
	ui.key(ImGuiMod_Ctrl, true);
	ui.key(ImGuiKey_A, true);
	ui.key(ImGuiKey_A, false);
	ui.key(ImGuiMod_Ctrl, false);
	ui.key(ImGuiKey_Backspace, true);
	ui.key(ImGuiKey_Backspace, false);
	ImGui::ClearActiveID();
	ui.frames(2);
	CHECK(listed(ui).size() == 5, "the text cleared: every finding");

	// The scope: the active menu's, the open menus', the project's.
	pick("Scope", "Active file");
	CHECK(listed(ui) == List({"Bravo:"}), "the active file's");
	pick("Scope", "Open files");
	CHECK(listed(ui) == List({"Bravo:", "Charlie:"}), "the open files'");
	pick("Scope", "Project");
	CHECK(listed(ui).size() == 5, "the project's");

	// The grouping: a header per file (the project's own findings first), then per kind; a
	// group of notes alone (b.mnu's, the stylesheets') starts folded. A click on a group's
	// header folds it away, keeping the header; another opens it again.
	pick("Group", "File");
	CHECK(in_order(logged_frame(ui), {"Project (2 errors)", kGametext, kMainMenu, "defs/items.def (1 error)", "Alpha:",
	                                  "menus/a.mnu (1 warning)", "Bravo:", "menus/b.mnu (1 info)"}) &&
	              listed(ui).size() == 4,
	      "grouped by file");
	pick("Group", "Kind");
	CHECK(in_order(logged_frame(ui), {"Required files (2 errors)", kGametext, kMainMenu, "Catalogs (1 error)", "Alpha:",
	                                  "Menus (1 warning)", "Bravo:", "Stylesheets (1 info)"}),
	      "grouped by kind");
	ui.click(problems_lines().at(0, 2));
	ui.away();
	CHECK(logged_frame(ui).find("Required files (2 errors)") != std::string::npos &&
	              listed(ui) == List({"Alpha:", "Bravo:"}),
	      "a folded group hides its rows");
	ui.click(problems_lines().at(0, 2));
	ui.away();
	CHECK(listed(ui).size() == 4, "unfolded again");
	// The stylesheets' group unfolded (its header the eighth line): it stays open.
	ui.click(problems_lines().at(7, 2));
	ui.away();
	CHECK(listed(ui).size() == 5, "a group of notes unfolded");

	// The Required files group's Fix all (the header's line) asks first: Cancel raises nothing,
	// Apply one Create naming every role. The other groups, a finding each with no fix, have
	// none.
	CHECK(ui.drain().empty(), "folding raises nothing");
	ui.click(problems_lines().fix(0));
	ui.frames(2);
	CHECK(confirmation() && ui.drain().empty(), "Fix all asks first");
	text = logged_frame(ui);
	CHECK(text.find("Create 2 files: gametext.bin, main.mnu.") != std::string::npos &&
	              text.find("cannot be undone with Undo") != std::string::npos,
	      "saying what it will do");
	CHECK(count_of(text, "Fix all") == 1, "the other groups have no Fix all");
	ui.click(confirmation_button(true));
	ui.frames(2);
	CHECK(!confirmation() && ui.drain().empty(), "Cancel raises nothing");
	ui.click(problems_lines().fix(0));
	ui.frames(2);
	ui.click(confirmation_button(false));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].names == List({"gametext", "main_menu"}),
	      "Apply: one Create naming every role");
	ui.frames(2);
	CHECK(!confirmation(), "and the confirmation closes");
	pick("Group", "None");

	// A click on a required file's row opens nothing: it is selected, every fix shown with
	// what it does, and its code; a second click folds it back. One on the catalog finding
	// opens its record at the field.
	ui.click(problems_lines().at(0, 2));
	CHECK(ui.drain().empty(), "a required file's row opens nothing");
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {kGametext, "Create gametext.bin", "Import gametext.bin from the game data...",
	                      "Use spare.bin as gametext.bin", "Renames spare.bin to gametext.bin.", "requirement.missing"}),
	      "the selected row: every fix with what it does, and its code");
	ui.click(problems_lines().at(0, 2));
	ui.away();
	CHECK(logged_frame(ui).find("requirement.missing") == std::string::npos, "a second click folds it back");
	ui.click(problems_lines().at(2, 2));
	requests = ui.drain();
	const EditorRequest *opened = one(requests, EditorRequestKind::OpenDocument);
	CHECK(opened && opened->path == "defs/items.def" && opened->edit.address == (NodeAddress{4, 2, 0}) && opened->edit.field == "type",
	      "a catalog finding opens its record at the field");
	ui.click(problems_lines().at(2, 2));
	ui.drain();

	// A required file's Fix creates it; its More lists every fix; a Use fix waits for Apply.
	ui.click(problems_lines().fix(0));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].names == List({"gametext"}),
	      "a required file's Fix creates it");
	ui.click(problems_lines().more(0, "Create gametext.bin"));
	text = logged_frame(ui);
	CHECK(text.find("Import gametext.bin from the game data...") != std::string::npos &&
	              text.find("Use spare.bin as gametext.bin") != std::string::npos,
	      "More lists Import and Use");
	ui.activate(popup_item(item_id(window, {"more"}), "Use spare.bin as gametext.bin"));
	CHECK(confirmation() && ui.drain().empty(), "a Use fix waits for Apply");
	ui.away();
	CHECK(logged_frame(ui).find("Renames spare.bin to gametext.bin.") != std::string::npos, "saying what it renames");
	ui.click(confirmation_button(false));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::AssignRequirement) && requests[0].path == "strings/spare.bin" &&
	              requests[0].text == "gametext",
	      "Apply renames it");

	// The summary's Fix alls: one Create for what factories make, one import list for what
	// only the game data has (cmap.mnu), each asking first.
	v.requirements.rows.push_back(missing_row("cmap_menu", "cmap.mnu", AssetKind::Menu));
	v.requirements.required_missing = 3;
	v.retail_files = {"cmap.mnu", "gametext.bin"};
	v.diagnostics.push_back(missing_finding("cmap_menu", "cmap.mnu"));
	v.revisions.touch(ViewConcern::Files);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	CHECK(in_order(logged_frame(ui), {"The game cannot start: 3 required files are missing.", "Create 2",
	                                  "Import 1 from the game data..."}),
	      "the summary's Fix alls");
	const ImGuiID summary = item_id(window, {v.project_root.c_str(), "required"});
	ui.activate(item_id(pushed(summary, static_cast<int>(EditorRequestKind::CreateMissing)), {"###fix"}));
	CHECK(confirmation() && ui.drain().empty(), "the summary's Create asks first");
	ui.click(confirmation_button(false));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].names == List({"gametext", "main_menu"}),
	      "one Create for every file a factory makes");
	ui.frames(2);
	ui.activate(item_id(pushed(summary, static_cast<int>(EditorRequestKind::PreviewRetailImport)), {"###fix"}));
	ui.click(confirmation_button(false));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PreviewRetailImport) && requests[0].names == List({"cmap.mnu"}),
	      "one import list for what the game data has");

	// An optional file the project lacks is a note with the same fixes: its Fix creates it.
	v.requirements.rows.push_back(missing_row("brand_style", "brand.mns", AssetKind::MenuStyle, false));
	Diagnostic optional = make_diagnostic(DiagnosticSeverity::Info, "requirement.optional_missing",
	                                      "Optional file brand.mns is not in the project.");
	optional.role = "brand_style";
	optional.target = "brand.mns";
	v.diagnostics.push_back(optional);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	// Errors (the three required files and the catalog's), the warning, then the notes.
	CHECK(in_order(logged_frame(ui), {"Charlie:", "Optional file brand.mns", "Create brand.mns"}), "the optional file's note");
	ui.drain();
	ui.click(problems_lines().fix(6));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].names == List({"brand_style"}),
	      "an optional file's Fix creates it");
	CHECK(ui.windows.pending_requests() == 0, "nothing else");
}

// S11e: a group of notes alone starts folded (the stylesheets'); once a warning joins it (a
// line-ending change made outside the editor, then Refresh) it opens again, and stays open
// when it holds notes alone again. A group the user folded (the menus') stays folded when a
// warning joins it.
void test_problems_auto_fold() {
	editor_test::TempProjectDir dir("opennova_editor_ui_problems_fold");
	SessionView v = problems_view(menu_at(dir, "a.mnu", "menus/a.mnu"), menu_at(dir, "b.mnu", "menus/b.mnu"));
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	ui.away();
	ui.drain();
	const auto shows = [&](const char *words) { return logged_frame(ui).find(words) != std::string::npos; };
	CHECK(shows("Stylesheets (1 info)") && !shows("Charlie:"), "the stylesheets' notes folded away");

	// A warning joins the stylesheets: the group folded for its notes opens.
	v.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning, "style.line_ending",
	                                        "Delta: its line ends changed.", "menus/b.mns"));
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	CHECK(shows("Delta:") && shows("Charlie:"), "a warning opens the group folded for its notes");
	// The warning gone: its notes stay in sight.
	v.diagnostics.pop_back();
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	CHECK(shows("Stylesheets (1 info)") && shows("Charlie:"), "notes alone again: it stays open");

	// The menus' group (its header the sixth line) folded by the user: a warning joining it
	// leaves it folded.
	ui.click(problems_lines().at(5, 2));
	ui.away();
	CHECK(shows("Menus (1 warning)") && !shows("Bravo:"), "the user folded the menus' group");
	v.diagnostics.push_back(make_diagnostic(DiagnosticSeverity::Warning, "menu.test", "Echo: another warning.",
	                                        "menus/a.mnu"));
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	CHECK(shows("Menus (2 warnings)") && !shows("Bravo:") && !shows("Echo:"), "a group the user folded stays folded");
	CHECK(ui.drain().empty(), "folding raises nothing");
}

// A press and its release must be on the same fix of the same finding: a finding before it
// that goes between them leaves another under the mouse, and its fix is not applied; a fix
// that changes between them is not applied either. Findings alike but for their record are
// kept apart: the one clicked stays the selected one when one before it goes.
void test_problems_presses() {
	using List = std::vector<std::string>;
	editor_test::TempProjectDir dir("opennova_editor_ui_problems_presses");
	SessionView v = problems_view(menu_at(dir, "a.mnu", "menus/a.mnu"), menu_at(dir, "b.mnu", "menus/b.mnu"));
	// Three required files, each with a Create: lines 0, 1 and 2.
	v.requirements.rows.push_back(missing_row("menu_style", "menu_style.mns", AssetKind::MenuStyle));
	v.requirements.required_missing = 3;
	Diagnostic font = make_diagnostic(DiagnosticSeverity::Error, "reference.missing", "Kilo: the font is missing.",
	                                  "menus/a.mnu", "font.name");
	font.reference = ReferenceKind::Font;
	font.target = "Custom.fnt";
	Diagnostic unnamed = make_diagnostic(DiagnosticSeverity::Error, "catalog.name_empty", "Lima: a record has no name.",
	                                     "defs/items.def", "name");
	unnamed.row_id = 10;
	unnamed.record_kind = 2;
	Diagnostic unnamed_too = unnamed;
	unnamed_too.row_id = 11;
	Diagnostic unnamed_three = unnamed;
	unnamed_three.row_id = 12;
	const std::vector<Diagnostic> findings = {missing_finding("gametext", "gametext.bin"), missing_finding("main_menu", "main.mnu"),
	                                          missing_finding("menu_style", "menu_style.mns"), unnamed, unnamed_too,
	                                          unnamed_three, font};
	v.diagnostics = findings;
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	problems_grouping(ui, "None"); // the lines below are the flat list's
	ui.away();
	ui.drain();

	// Pressed on main.mnu's Create (line 1); gametext's finding goes (menu_style.mns's slides
	// under the mouse); released: nothing is applied.
	ProblemsLines lines = problems_lines();
	const ImVec2 press = lines.fix(1);
	ui.mouse(press.x, press.y);
	ui.button(true);
	v.diagnostics.erase(v.diagnostics.begin());
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.button(false);
	ui.frames();
	CHECK(ui.drain().empty(), "a release over another finding applies nothing");
	// Released where it was pressed, the finding staying: its fix.
	ui.click(problems_lines().fix(0));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].names == List({"main_menu"}), "a whole click applies");

	// The font's Create pressed; the game data gains the font (its first fix becomes Import);
	// released: nothing is applied, and a new click takes the Import.
	v.diagnostics = findings;
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	CHECK(in_order(logged_frame(ui), {"Kilo:", "Create Custom.fnt"}), "the font's Create");
	lines = problems_lines();
	const ImVec2 create = lines.fix(6);
	ui.mouse(create.x, create.y);
	ui.button(true);
	v.retail_files = {"Custom.fnt", "gametext.bin"};
	v.revisions.touch(ViewConcern::Files);
	ui.frames(2);
	ui.button(false);
	ui.frames();
	CHECK(ui.drain().empty(), "a release on a fix that changed since the press applies nothing");
	ui.click(problems_lines().fix(6));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PreviewRetailImport) && requests[0].names == List({"Custom.fnt"}),
	      "a new click applies the fix it shows");

	// Three findings alike but for their record: the second one clicked opens its own record.
	// Made again without the first, the one clicked is the selected (expanded) one: now the
	// first of the two, its code before either one's place.
	ui.away();
	ui.click(problems_lines().at(4, 2));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::OpenDocument) && requests[0].edit.address.row == 11, "the second one's record");
	v.diagnostics = findings;
	v.diagnostics.erase(v.diagnostics.begin() + 3);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	const std::string text = logged_frame(ui);
	const size_t code = text.find("catalog.name_empty");
	CHECK(code != std::string::npos && code < text.find("items.def - name") && count_of(text, "catalog.name_empty") == 1,
	      "the one clicked stays selected when one before it goes");
	CHECK(ui.windows.pending_requests() == 0, "nothing else");
}

// A confirmation belongs to the project it was asked in and follows the findings it is for:
// another project closes it; a finding gone while it is open changes what it says, a release
// on Apply pressed before the change applies nothing, and Apply then raises the new list.
// More's list follows its finding when others go, and closes when it goes.
void test_problems_confirmation_follows() {
	using List = std::vector<std::string>;
	editor_test::TempProjectDir dir("opennova_editor_ui_problems_follow");
	SessionView v = problems_view(menu_at(dir, "a.mnu", "menus/a.mnu"), menu_at(dir, "b.mnu", "menus/b.mnu"));
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	ui.away();
	ui.drain();
	const ImGuiID window = Ui::window_id("Problems");
	ui.activate(item_id(window, {"Group"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"Kind"}));

	// Another project while it is open: it closes, nothing it held is raised, and it stays
	// closed when that project is the one open again.
	ui.click(problems_lines().fix(0));
	ui.frames(2);
	CHECK(confirmation() != nullptr, "the Fix all asks");
	const std::string root = v.project_root;
	v.project_root = "C:/mods/Another";
	v.revisions.touch(ViewConcern::Project);
	ui.frames(2);
	CHECK(!confirmation(), "another project closes it");
	v.project_root = root;
	v.revisions.touch(ViewConcern::Project);
	ui.frames(2);
	CHECK(!confirmation() && ui.drain().empty(), "and nothing it held is raised");

	// A finding gone while it is open (main.mnu made elsewhere): the confirmation says the new
	// list; Apply pressed before the change applies nothing on its release; pressed again, the
	// new list alone.
	ui.click(problems_lines().fix(0));
	ui.frames(2);
	ui.away();
	CHECK(logged_frame(ui).find("Create 2 files: gametext.bin, main.mnu.") != std::string::npos, "both files");
	const ImVec2 pressed = confirmation_button(false);
	ui.mouse(pressed.x, pressed.y);
	ui.button(true);
	v.diagnostics.erase(v.diagnostics.begin() + 1);
	v.requirements.rows[1].state = RequirementState::Present;
	v.requirements.required_missing = 1;
	v.revisions.touch(ViewConcern::Findings);
	v.revisions.touch(ViewConcern::Files);
	ui.frames(2);
	ui.button(false);
	ui.frames();
	CHECK(ui.drain().empty(), "Apply pressed on the old list applies nothing");
	ui.away();
	std::string text = logged_frame(ui);
	CHECK(text.find("Create gametext.bin. It starts as placeholder content") != std::string::npos &&
	              text.find("gametext.bin, main.mnu") == std::string::npos &&
	              text.find("Changed while open") != std::string::npos,
	      "it says the new list, and that it changed");
	ui.click(confirmation_button(false));
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].names == List({"gametext"}), "then the new list alone");

	// More's list follows its finding: main.mnu's again, gametext's gone before it.
	replace_view(v, problems_view(menu_at(dir, "a.mnu", "menus/a.mnu"),
			menu_at(dir, "b.mnu", "menus/b.mnu")));
	ui.activate(item_id(window, {"Group"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"None"}));
	ui.frames(2);
	ui.click(problems_lines().more(1, "Create main.mnu"));
	ui.away();
	CHECK(logged_frame(ui).find("Use a.mnu as main.mnu") != std::string::npos, "More lists main.mnu's fixes");
	v.diagnostics.erase(v.diagnostics.begin());
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	text = logged_frame(ui);
	CHECK(text.find("Use a.mnu as main.mnu") != std::string::npos && text.find("Use spare.bin") == std::string::npos,
	      "still main.mnu's when a finding before it goes");
	v.diagnostics.erase(v.diagnostics.begin());
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	CHECK(logged_frame(ui).find("Use a.mnu as main.mnu") == std::string::npos, "closed when its finding goes");
	CHECK(ui.windows.pending_requests() == 0, "nothing else");
}

// In a narrow dock nothing runs past its cell: a Fix column too narrow for a fix and More
// has one Fix... that lists every fix; an expanded finding's buttons, details and code stay
// in the message's column; a group's Fix all stays in its cell. (Problems spans the bottom:
// a window 480 wide makes it narrow.)
void test_problems_narrow() {
	editor_test::TempProjectDir dir("opennova_editor_ui_problems_narrow");
	SessionView v = problems_view(menu_at(dir, "a.mnu", "menus/a.mnu"), menu_at(dir, "b.mnu", "menus/b.mnu"));
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(480.0f, 700.0f);
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	problems_grouping(ui, "None"); // the lines below are the flat list's
	ui.away();
	ui.drain();
	const auto within = [&](const char *what) {
		const ImGuiTable *table = problems_lines().table;
		CHECK(table != nullptr, what);
		if (!table) return;
		for (int column = 1; column < 4; ++column)
			CHECK(table->Columns[column].ContentMaxXUnfrozen <= table->Columns[column].WorkMaxX + 0.5f, what);
	};
	std::string text = logged_frame(ui);
	CHECK(text.find("Fix...") != std::string::npos && text.find("More") == std::string::npos, "one Fix... where both do not fit");
	within("a line's controls stay in their cells");
	ui.click(problems_lines().fix(0));
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Import gametext.bin from the game data...") != std::string::npos &&
	              text.find("Use spare.bin as gametext.bin") != std::string::npos,
	      "Fix... lists every fix");
	// A click outside the list closes it, pressing nothing under it; the next one selects.
	ui.click(problems_lines().at(0, 2));
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Import gametext.bin from the game data...") == std::string::npos &&
	              text.find("requirement.missing") == std::string::npos && ui.drain().empty(),
	      "a click outside closes the list");
	ui.click(problems_lines().at(0, 2));
	ui.away();
	ui.frames(2);
	CHECK(logged_frame(ui).find("requirement.missing") != std::string::npos, "the finding expanded");
	within("the expanded finding stays in its cells");
	ui.activate(item_id(Ui::window_id("Problems"), {"Group"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"Kind"}));
	ui.away();
	ui.frames(2);
	within("a group's Fix all stays in its cell");
	CHECK(ui.drain().empty(), "nothing raised");
}

// A Rewrite is offered for input a rewrite drops, but not for a file that does not serialize
// (its own finding says so, and the Save would be refused); Only fixable lists what has a fix.
void test_problems_rewrite_hidden() {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Rewrite";
	v.diagnostics = {make_diagnostic(DiagnosticSeverity::Warning, "catalog.ignored_input", "Delta: a key the game ignores.",
	                                 "defs/weapon.def"),
	                 make_diagnostic(DiagnosticSeverity::Error, "catalog.unserializable", "Echo: this cannot be written.",
	                                 "defs/weapon.def"),
	                 make_diagnostic(DiagnosticSeverity::Warning, "catalog.ignored_input", "Foxtrot: a key the game ignores.",
	                                 "defs/ammo.def")};
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	ui.away();
	std::string text = logged_frame(ui);
	CHECK(in_order(text, {"Echo:", "Delta:", "Foxtrot:", "Rewrite ammo.def"}) && text.find("Rewrite weapon.def") == std::string::npos,
	      "no Rewrite of a file that does not serialize");
	ui.activate(item_id(Ui::window_id("Problems"), {"Only fixable"}));
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Foxtrot:") != std::string::npos && text.find("Delta:") == std::string::npos &&
	              text.find("Echo:") == std::string::npos && text.find("1 of 3") != std::string::npos,
	      "Only fixable: the finding a fix is offered for");
	CHECK(ui.windows.pending_requests() == 0, "drawing raises nothing");
}

// S11h: two textures the project lacks, a model's and a particle's, under one group: its Fix
// all says their placeholders in one line (the game's missing-texture checkerboard) and Apply
// raises a CreateFile for each, in the order the findings show.
void test_problems_placeholders() {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Placeholders";
	Diagnostic skin = make_diagnostic(DiagnosticSeverity::Error, "reference.missing", "Golf: the texture 'skin.tga'.",
	                                  "models/tank.3di", "name");
	skin.reference = ReferenceKind::Texture;
	skin.target = "skin.tga";
	skin.loader_arg = 0;
	Diagnostic puff = make_diagnostic(DiagnosticSeverity::Error, "reference.missing", "Hotel: the texture 'puff.tga'.",
	                                  "fx.ptl", "graphic1");
	puff.reference = ReferenceKind::Texture;
	puff.target = "puff.tga";
	v.diagnostics = {skin, puff};
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	ui.away();
	ui.drain();
	ui.click(problems_lines().fix(0)); // the Missing references group's Fix all
	ui.frames(2);
	ui.away();
	CHECK(confirmation() != nullptr &&
	              logged_frame(ui).find("Create 2 placeholder textures: skin.tga, puff.tga. Each is the checkerboard the game "
	                                    "draws for a missing texture") != std::string::npos,
	      "the Fix all says the placeholders in one line");
	ui.click(confirmation_button(false));
	const std::vector<EditorRequest> requests = ui.drain();
	CHECK(requests.size() == 2 && requests[0].kind == EditorRequestKind::CreateFile && requests[0].path == "skin.tga" &&
	              requests[0].text == "texture" && requests[1].kind == EditorRequestKind::CreateFile &&
	              requests[1].path == "puff.tga",
	      "Apply: a CreateFile for each");
}

// A thousand findings in fifty catalogs: only the lines that show are drawn and only their
// fixes asked (fixes_for is never run for a line not drawn). Scrolled to the middle, a click
// opens the finding under the mouse; one expanded there, the lines after it sit right under
// it and those before where they were. Grouped by file (a header before each fifty's twenty),
// a click in the middle opens the finding under it too.
void test_problems_many() {
	SessionView v;
	v.project_open = true;
	v.project_root = "C:/mods/Many";
	for (int file = 0; file < 50; ++file) {
		const std::string name = "f" + std::to_string(file) + ".def";
		v.scan.entries.push_back(file_entry(name, "defs/" + name, AssetKind::ItemDefs));
	}
	for (size_t i = 0; i < 1000; ++i) {
		Diagnostic d = make_diagnostic(DiagnosticSeverity::Warning, "catalog.ignored_input",
		                               "Finding " + std::to_string(i) + ": a line the game ignores.",
		                               v.scan.entries[i / 20].relative_path, "name");
		d.row_id = i + 1;
		d.record_kind = 2;
		d.line = i + 1;
		v.diagnostics.push_back(d);
	}
	v.scan.index();
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	problems_grouping(ui, "None"); // the lines below are the flat list's
	ui.away();
	ui.drain();
	const auto *window = dynamic_cast<const ProblemsWindow *>(find_window(ui.windows.pass(), "Problems"));
	CHECK(window != nullptr, "the Problems window");
	if (!window) return;
	const auto rows_drawn = [] {
		const ImGuiTable *table = problems_lines().table;
		return table ? table->CurrentRow + 1 : -1;
	};
	const auto opened_row = [&](const std::vector<EditorRequest> &requests) -> NodeId {
		const EditorRequest *open = one(requests, EditorRequestKind::OpenDocument);
		return open ? open->edit.address.row : 0;
	};
	CHECK(rows_drawn() > 0 && rows_drawn() < 60, "a thousand findings: only the lines that show");
	CHECK(window->fixes_asked() > 0 && window->fixes_asked() < 60, "and only their fixes asked");

	// Scrolled to line 500: a click on the fourth line shown opens finding 503.
	ProblemsLines lines = problems_lines();
	ImGui::SetScrollY(lines.table->InnerWindow, 500.0f * lines.line);
	ui.frames(3);
	lines = problems_lines();
	const float flat = lines.table->InnerWindow->ContentSize.y;
	ui.click(lines.at(503, 2));
	CHECK(opened_row(ui.drain()) == 504, "scrolled: the finding under the mouse");
	ui.away();
	ui.frames(3);
	CHECK(rows_drawn() > 0 && rows_drawn() < 60, "the expanded one whole, the rest still clipped");
	// Finding 503 expanded: the list grows by what it adds, and the line after it sits right
	// under it.
	lines = problems_lines();
	const float extra = lines.table->InnerWindow->ContentSize.y - flat;
	CHECK(extra > 0.0f, "the expanded line is taller");
	ui.click(ImVec2(lines.at(504, 2).x, lines.y(504) + extra));
	CHECK(opened_row(ui.drain()) == 505, "the line after the expanded one, right under it");
	ui.away();
	ui.frames(3);
	lines = problems_lines();
	ui.click(lines.at(502, 2));
	CHECK(opened_row(ui.drain()) == 503, "a line before it, where it was");
	ui.away();
	ui.click(problems_lines().at(502, 2));
	ui.drain(); // folded back

	// Grouped by file: 21 lines a catalog (its header, its twenty findings).
	ui.activate(item_id(Ui::window_id("Problems"), {"Group"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"File"}));
	ui.away();
	ui.frames(2);
	lines = problems_lines();
	ImGui::SetScrollY(lines.table->InnerWindow, 525.0f * lines.line);
	ui.frames(3);
	CHECK(rows_drawn() > 0 && rows_drawn() < 60, "grouped: only the lines that show");
	lines = problems_lines();
	ui.click(lines.at(527, 2)); // catalog 25's second finding: 25 * 20 + 1
	CHECK(opened_row(ui.drain()) == 502, "grouped: the finding under the mouse");
	ui.away();
	CHECK(window->fixes_asked() < 120, "fixes asked only for the lines drawn");
	const std::string text = logged_frame(ui);
	CHECK(text.find("Finding 0:") != std::string::npos && text.find("Finding 999:") != std::string::npos, "a log lists every one");
	ui.frames(2);
	CHECK(ui.windows.pending_requests() == 0, "drawing them raises nothing");
}

} // namespace

// --- S9i: the stylesheet view ---------------------------------------------------------

// The stylesheet's lines as a table: an add goes after the selected line, Remove takes
// the selected line, and a locked line (a directive) does not go.
void test_styles_window_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_styles_test");
	CHECK(editor_test::write_text(dir.file("menu_style.mns"),
	                              "// Colors\r\nDEF_TEXT_FG FFFFFFFF\r\n\r\n#if 0\r\nOFF 1\r\n#endif\r\n"),
	      "stylesheet fixture");
	auto document = std::make_shared<MnsDocument>();
	Diagnostic error;
	CHECK(document->load(dir.file("menu_style.mns"), "menu_style.mns", AssetKind::MenuStyle, "jo", error),
	      "the stylesheet loads");
	SessionView v;
	v.project_open = true;
	v.project_root = dir.root();
	AssetEntry entry;
	entry.logical_name = "menu_style.mns";
	entry.relative_path = "menu_style.mns";
	entry.kind = AssetKind::MenuStyle;
	v.scan.entries.push_back(entry);
	v.scan.index();
	v.documents.push_back(document);
	v.active_document = document->path();
	const Node &fg = *document->rows()[1];
	select_in(v, {fg.id, fg.kind, 0});
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Document");
	ui.drain();
	CHECK(ui.windows.pending_requests() == 0, "drawing the stylesheet raises nothing");
	const ImGuiID styles = document_tab_id(document->path());
	ui.activate(item_id(styles, {"Add variable"}));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *add = only(requests, EditorRequestKind::EditRecord);
	CHECK(add && add->edit.operation == EditOperation::Add && add->edit.address.kind == node_kind(StyleKind::Variable) &&
	              add->edit.position == 2,
	      "Add variable goes after the selected line");
	ui.activate(item_id(styles, {"Remove"}));
	requests = ui.drain();
	const EditorRequest *remove = only(requests, EditorRequestKind::EditRecord);
	CHECK(remove && remove->edit.operation == EditOperation::Remove && remove->edit.address.row == fg.id,
	      "Remove takes the selected line");
	const Node &directive = *document->rows()[3];
	select_in(v, {directive.id, directive.kind, 0});
	ui.frames(2);
	ui.activate(item_id(styles, {"Remove"}));
	requests = ui.drain();
	CHECK(only(requests, EditorRequestKind::EditRecord) == nullptr, "a locked line does not go");
}

// S11h2: the table lists a stylesheet's variables and its #if lines, never a comment or a
// blank line (they stay in the file), and has no Add comment or Add blank line. Over a real
// session, each request served and the file's text read back: Up and Down take a line to the
// place of the listed line before or after it, past the unlisted lines between them, which
// keep their order and their place beside the line it passes (the header first, the footer
// last); the first listed line has no Up and the last no Down, though unlisted lines lie past
// them; a comment a Problems row selects (the line whose end the game does not read) is no
// line of the table, so the row tools wait for one and Add variable adds at the end of the
// file.
void test_styles_lines_listed() {
	editor_test::TempProjectDir dir("opennova_editor_ui_styles_lines");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Styles"));
	const SessionView &v = session.view();
	CHECK(editor_test::write_text(v.project_root + "/menu_style.mns",
	                              "// Header\r\nA_FG FFFFFFFF\r\n// Colours below\n\r\nB_FG FF000000\r\n// Footer\r\n"),
	      "stylesheet fixture");
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, "menu_style.mns"));
	const auto *document = dynamic_cast<const MnsDocument *>(session.document_for("menu_style.mns"));
	CHECK(document && document->rows().size() == 6,
	      "the stylesheet's six lines: a header, a variable, a comment, a blank line, a variable, a footer");
	if (!document || document->rows().size() != 6) return;
	const std::string path = document->path();
	const NodeId a = document->rows()[1]->id, b = document->rows()[4]->id, comment = document->rows()[2]->id;
	const auto text = [&]() { return document->serialize().text; };
	const std::string original = "// Header\r\nA_FG FFFFFFFF\r\n// Colours below\r\n\r\nB_FG FF000000\r\n// Footer\r\n";
	CHECK(text() == original, "read as it is, every line ending CR LF");
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	const ImGuiID styles = document_tab_id(path);
	const ImGuiTable *table = ImGui::TableFindByID(item_id(styles, {"lines"}));
	CHECK(table && table->CurrentRow + 1 == 3, "the header and two lines: the variables, not the comments or the blank line");
	const std::string frame = logged_frame(ui);
	CHECK(frame.find("Add variable") != std::string::npos && frame.find("Add comment") == std::string::npos &&
	              frame.find("Add blank line") == std::string::npos,
	      "Add variable, no Add comment, no Add blank line");
	// A line selected; a tool pressed, the requests it raised served.
	const NodeKind variable = node_kind(StyleKind::Variable);
	const auto select_line = [&](NodeId row) {
		EditorRequest select = make_request(EditorRequestKind::SelectRecord, path);
		select.edit.address = {row, variable, 0};
		session.handle(select);
		ui.frames(2);
		ui.drain();
	};
	const auto press = [&](const char *tool) {
		ui.activate(item_id(styles, {tool}));
		const std::vector<EditorRequest> requests = ui.drain();
		for (const EditorRequest &request : requests) session.handle(request);
		ui.frames(2);
		return requests;
	};
	const auto undo = [&]() {
		session.handle(make_request(EditorRequestKind::Undo, path));
		ui.frames(2);
	};
	const auto moved_to = [](const std::vector<EditorRequest> &requests) {
		const EditorRequest *move = only(requests, EditorRequestKind::EditRecord);
		return move && move->edit.operation == EditOperation::Move ? move->edit.position : SIZE_MAX;
	};
	select_line(a);
	CHECK(press("Up").empty() && text() == original, "the first listed line has no Up, the header above it");
	select_line(b);
	CHECK(press("Down").empty() && text() == original, "the last listed line has no Down, the footer below it");
	CHECK(moved_to(press("Up")) == 1 &&
	              text() == "// Header\r\nB_FG FF000000\r\nA_FG FFFFFFFF\r\n// Colours below\r\n\r\n// Footer\r\n",
	      "Up: B right above A, past the comment and the blank line, which stay after A");
	undo();
	CHECK(text() == original, "Up undone");
	select_line(a);
	CHECK(moved_to(press("Down")) == 4 &&
	              text() == "// Header\r\n// Colours below\r\n\r\nB_FG FF000000\r\nA_FG FFFFFFFF\r\n// Footer\r\n",
	      "Down: A right below B, past the comment and the blank line, which stay before B");
	undo();
	CHECK(text() == original, "Down undone");

	// The comment selected as its Problems row selects it: the first line whose end is not CR LF.
	ProblemLocation location;
	for (const Diagnostic &d : v.diagnostics)
		if (d.code == "style.line_ending" && d.asset == path && d.row_id == comment) location = problem_location(d, v);
	CHECK(!location.empty() && location.record.row == comment, "the line ending's finding goes to the comment");
	EditorRequest open = make_request(EditorRequestKind::OpenDocument, location.path);
	open.edit.address = location.record;
	open.edit.field = location.field;
	session.handle(open);
	ui.frames(2);
	ui.drain();
	CHECK(v.selection.row == comment, "the Problems row selects the comment");
	for (const char *tool : {"Duplicate", "Remove", "Up", "Down"}) CHECK(press(tool).empty(), tool);
	CHECK(text() == original, "no row tool acts on a line the table does not list");
	const std::vector<EditorRequest> added = press("Add variable");
	const EditorRequest *add = only(added, EditorRequestKind::EditRecord);
	CHECK(add && add->edit.operation == EditOperation::Add && add->edit.position == 6 && document->rows().size() == 7 &&
	              document->rows()[6]->kind == variable && text().rfind(original, 0) == 0,
	      "Add variable: at the end of the file, after the footer");
}

// Go to and the uses, through the Inspector (S12 D3), over a real session. A stylesheet
// variable's "Referenced by" rows are each a click away: the first opens the menu that names it
// at the record by its locator, the field shown. MAIN's font names a style variable: its Go to
// offers the variable where the game reads it and the .fnt its value names; the first opens the
// stylesheet at the variable, the second shows the font in Files (the editor does not edit
// fonts).
void test_go_to_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_go_to");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "GoTo"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	session.handle(make_request(EditorRequestKind::OpenDocument, "menu_style.mns"));
	const Document *style = session.document_for("menu_style.mns");
	NodeAddress large;
	CHECK(style && style->find("DEF_FONTNAME_LG", large), "the stylesheet's large font");
	if (!style || !large.row) return;
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, style->path());
	select.edit.address = large;
	session.handle(select);
	const std::vector<const GraphEdge *> users = v.graph->referrers_of(ReferenceKind::StyleVar, "DEF_FONTNAME_LG");
	CHECK(!users.empty() && !users.front()->locator.empty(), "a menu names the large font");
	if (users.empty()) return;
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	const ImGuiID inspector = Ui::window_id("Inspector");
	ui.activate(item_id(pushed(inspector, 0), {"###use"}));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *use = one(requests, EditorRequestKind::OpenDocument);
	CHECK(use && use->path == users.front()->source && use->text == users.front()->locator &&
	              use->edit.field == users.front()->field,
	      "a use opens its file at the record that makes it");

	// MAIN's font: Go to offers the variable and the font file.
	session.handle(make_request(EditorRequestKind::OpenDocument, "main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress main;
	CHECK(menu && menu->find("MAIN", main), "the menu's MAIN window");
	if (!menu || !main.row) return;
	select = make_request(EditorRequestKind::SelectRecord, menu->path());
	select.edit.address = main;
	session.handle(select);
	ui.frames(3);
	ui.drain();
	std::string key;
	for (const InspectorSection &section : plan_inspector(*menu, main, main, ""))
		for (const FieldUse &field : section.fields)
			if (field.schema->id == "font.name") key = section.key;
	CHECK(!key.empty(), "the font's section");
	FieldUse font;
	for (const FieldSchema &schema : menu->fields(main.kind))
		if (schema.id == "font.name") font = menu->field_on(main, schema);
	Value value;
	CHECK(menu->get(main, "font.name", value), "the font's value");
	const std::vector<ReferenceTarget> targets = menu->reference_targets(font, value, v);
	CHECK(targets.size() == 2 && targets[0].editable && !targets[1].editable, "the variable, then the font file");
	if (targets.size() != 2) return;
	ui.activate(item_id(inspector, {key.c_str(), "fields", "font.name", "Go to"}));
	CHECK(ui.drain().empty(), "two places: Go to opens a menu of them, going nowhere yet");
	const ImGuiID places = item_id(inspector, {key.c_str(), "fields", "font.name", "go to"});
	ui.activate(popup_item(places, targets[0].label.c_str()));
	requests = ui.drain();
	const EditorRequest *variable = one(requests, EditorRequestKind::OpenDocument);
	CHECK(variable && variable->path == style->path() && style->address_at(variable->text) == large &&
	              variable->edit.field == "name",
	      "the variable: the stylesheet opened at it, its name shown");
	ui.activate(item_id(inspector, {key.c_str(), "fields", "font.name", "Go to"}));
	ui.drain();
	ui.activate(popup_item(places, targets[1].label.c_str()));
	requests = ui.drain();
	const EditorRequest *file = one(requests, EditorRequestKind::ShowInFiles);
	CHECK(file && file->path == targets[1].file, "the font file: shown in Files, which the editor does not open");
}

// A number that names something navigates too (S12 D3 review): an item's emplacement
// attachment names another item of the same file by its integer id. With the attachment
// selected, its own form and the attachments table beside it show; neither has a picker for
// the number, but the form's Go to and the table cell's badge dot each open the table at the
// item it names, its id shown.
void test_numeric_go_to_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_numeric_go_to");
	NoProcess platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Numbers"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const AssetEntry *items_asset = v.scan.find("items.def");
	CHECK(items_asset != nullptr, "the project's item table");
	if (!items_asset) return;
	const std::string items_path = items_asset->relative_path;
	CHECK(editor_test::write_text(v.project_root + "/" + items_path,
	                              "begin \"Carrier\"\nid 100164\ntype vehicle\naddeweap ewep01 100166\nend\n"
	                              "begin \"Gun\"\nid 100166\ntype vehicle\nend\n"),
	      "an item naming another by id");
	session.handle(make_request(EditorRequestKind::Rescan));
	session.handle(make_request(EditorRequestKind::OpenDocument, items_path));
	const Document *items = session.document_for(items_path);
	NodeAddress carrier, gun;
	CHECK(items && items->find("100164", carrier) && items->find("100166", gun), "the two items");
	if (!items || !carrier.row || !gun.row) return;
	NodeAddress attachment;
	for (const Document::Collection &collection : items->collections_of(carrier))
		if (std::string(collection.spec.kind_name) == "attachment" && !collection.ids.empty())
			attachment = {carrier.row, collection.spec.kind, collection.ids.front()};
	CHECK(attachment.child != 0, "the carrier's attachment");
	if (!attachment.child) return;
	EditorRequest select = make_request(EditorRequestKind::SelectRecord, items_path);
	select.edit.address = attachment;
	session.handle(select);
	std::string form, list;
	for (const InspectorSection &section : plan_inspector(*items, attachment, carrier, "")) {
		for (const FieldUse &field : section.fields)
			if (field.schema->id == "item_id") form = section.key;
		for (const Document::Collection &collection : section.collections)
			if (std::string(collection.spec.kind_name) == "attachment") list = section.key;
	}
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	const ImGuiID inspector = Ui::window_id("Inspector");
	const auto went_to_gun = [&](const std::vector<EditorRequest> &requests) {
		const EditorRequest *go = one(requests, EditorRequestKind::OpenDocument);
		return go && go->path == items_path && items->address_at(go->text) == gun && go->edit.field == "id";
	};
	ui.activate(item_id(inspector, {form.c_str(), "fields", "item_id", "Go to"}));
	CHECK(went_to_gun(ui.drain()), "the form's Go to opens the table at the item the id names, its id shown");
	CHECK(!ui.windows.pending_requests(), "nothing else");
	// The table scrolls sideways: the item id's column brought into view (a column out of it
	// submits nothing).
	const ImGuiID table_id = item_id(inspector, {list.c_str(), "attachment", "records"});
	ImGuiTable *table = ImGui::TableFindByID(table_id);
	CHECK(table && table->ColumnsCount > 2, "the attachments table beside the form");
	if (!table || table->ColumnsCount <= 2) return;
	ImGui::SetScrollX(table->InnerWindow, table->Columns[2].MinX - table->Columns[1].MinX);
	ui.frames(2);
	ui.drain();
	ui.activate(item_id(pushed(table_id, static_cast<int>(attachment.child)), {"item_id", "go to dot"}));
	CHECK(went_to_gun(ui.drain()), "the table cell's dot goes to the same item");
	CHECK(logged_frame(ui).find("Pick") == std::string::npos, "no picker for a number");
}

int main() {
	run_workspace_tests();
	run_marker_tests();
	run_bounds_tests();
	run_field_widget_tests();
	run_reference_picker_tests();
	run_find_tests();
	run_rename_tests();
	test_requests_round_trip();
	test_menu_tree_model();
	test_inspector_plan();
	test_menu_window_ui();
	test_inspector_ui();
	test_actions_after_edits();
	test_preview_window_ui();
	test_preview_gestures_end();
	test_preview_several_windows_ui();
	test_styles_window_ui();
	test_styles_lines_listed();
	test_go_to_ui();
	test_numeric_go_to_ui();
	test_ui_kit();
	test_problems_window_ui();
	test_problems_auto_fold();
	test_problems_presses();
	test_problems_confirmation_follows();
	test_problems_narrow();
	test_problems_rewrite_hidden();
	test_problems_placeholders();
	test_problems_many();
	if (editor_ui_test::g_failures == 0) std::printf("editor_ui: all tests passed\n");
	return editor_ui_test::g_failures == 0 ? 0 : 1;
}
