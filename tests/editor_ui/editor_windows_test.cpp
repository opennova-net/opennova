// The editor's windows over a null ImGui backend (ADR 0046 d11); the S11d workspace's shell (the
// windows and their layout, the Document window's tabs, Files, the menus, the modals) is
// workspace_test.cpp's. S9h2: the menu view's tree (built once from the Windows collections,
// the Moves a drop, an Indent and an Outdent make) and the inspector's plan (groups by dotted
// id, the block toggle first, collections in the group their token names,
// ignored-and-left-out fields hidden); then, driven through the real windows, a click on a
// tree row in the menu's Document tab selects it (Ctrl+click toggles), a drag drops a window
// inside, before or after another, the toolbar's buttons and Ctrl+C / X / V raise their
// requests, and the inspector's tables add, move and edit rows in place. S9k1, S13 V2: the
// Preview window's menu pane over a fake device backed by the headless render, one smoke test
// of its canvas (a click, a drag, Preview closed mid-drag, the held state, the pane hidden
// taking no key, Ctrl+wheel, the empty states); the canvas's rules are
// tests/editor/canvas_test.cpp's. S11a: a frame's
// Save goes after the frame's edits, and the save
// shortcuts work while a text field has the keyboard. S11c: the Problems window, pressed
// with the mouse where a user presses (the filters, the grouping and folding, the fixes and
// what asks before it acts, a confirmation following its project and its findings, nothing
// past its cell in a narrow dock, Only fixable, a thousand findings clipped, flat and grouped;
// its rules are its model's, S13 V1: tests/editor/problems_list_test.cpp) and the pieces it
// draws with (ui_kit). S12 D3: the
// Inspector's Go to (a menu of the places a font through a style variable leads) and its
// clickable "Referenced by" rows. S13 V1: a window a list's part holds is not copyable from the
// tree (the one clipboard rule, tests/editor/mnu_clipboard_test.cpp). ADR 0046 S14: the Inspector's
// shared form over records of kinds whose fields are alike. Each group of the
// editor_ui ctest is a row of its own (main: the group named on the command line).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/model_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/menu_screen_render.h>
#include <editor/preview/menu_viewport.h>
#include <editor/model/field_text.h>
#include <editor/session/problem_query.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>
#include "../editor/editor_test_support.h"
#include "../editor/menu_test_support.h"
#include "../editor/pool_document.h"
#include "common/test_paths.h"
#include "editor_ui_test_support.h"
#include <editor/ui/document_window.h>
#include <editor/ui/editor_windows.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/inspector_window.h>
#include <editor/ui/menu_view.h>
#include <editor/ui/output_window.h>
#include <editor/ui/preview_window.h>
#include <editor/ui/styles_view.h>
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
	windows.request(request::open_project("C:/mods/A"));
	EditorRequest set = request::of(EditorRequestKind::ApplyProjectSettings);
	set.settings.mission = true;
	windows.request(set);
	CHECK(windows.pending_requests() == 2, "two queued");
	EditorRequest out;
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::OpenProject && out.dir == "C:/mods/A",
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
	CHECK(windows.take_request(out) && out.kind == EditorRequestKind::OpenProject && out.dir == "C:/mods/B",
	      "open pick");
	windows.deliver_pick(PickPurpose::RuntimeExecutable, "C:/tools/opennova.exe");
	windows.deliver_pick(PickPurpose::GameInstall, "C:/games/Joint Operations");
	CHECK(!windows.take_request(out), "the settings' picks fill its fields, they apply nothing by themselves");
	windows.deliver_pick(PickPurpose::OpenProject, "");
	CHECK(!windows.take_request(out), "a cancelled pick raises nothing");
	windows.deliver_pick(PickPurpose::NewProjectLocation, "C:/mods/New");
	CHECK(!windows.take_request(out) && std::string(windows.new_project_form().folder()) == "C:/mods/New",
	      "a location pick fills the form, it does not open");
}

// The frame bracket reads the request table (S13 A4: acts_on_saved_files and
// names_the_active_document were switches in editor_windows.cpp): raised inside a frame, a request
// of a kind that acts on the files as saved waits for the frame's other requests (a Select raised
// after it is taken first), any other goes in order; raised with no path, one whose empty path
// names the active document carries the one active when it was raised. The lists are the ones the
// switches held, every kind asked.
void test_frame_bracket_follows_the_table() {
	using K = EditorRequestKind;
	const std::vector<K> saved = {K::NewProject, K::OpenProject, K::CloseProject, K::Rescan, K::ImportFiles,
	                              K::Build, K::Play, K::Export, K::ReloadDocument, K::CloseDocument, K::Save, K::SaveAll,
	                              K::ResolveUnsaved, K::RenameAsset, K::AssignRequirement, K::RenameSymbol, K::RenameBack,
	                              K::Quit};
	// S13 V7: a viewport's change and an edit in a viewport name the active document's, as every
	// pathless request does.
	const std::vector<K> active = {K::OpenDocument, K::ReloadDocument, K::CloseDocument, K::SelectRecord, K::EditRecord,
	                               K::RevertToSaved, K::EndEdit, K::Copy, K::Cut, K::Paste, K::Duplicate, K::Save,
	                               K::Undo, K::Redo, K::SetViewport, K::EditInViewport};
	const auto listed = [](const std::vector<K> &kinds, K kind) {
		return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
	};
	SessionView v;
	v.documents.active = "menus/main.mnu";
	size_t deferred = 0, named = 0;
	for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
		const auto kind = static_cast<K>(i);
		const RequestKindRow &row = request_kind_row(kind);
		CHECK(row.acts_on_saved == listed(saved, kind), row.token);
		CHECK(row.names_active == listed(active, kind), row.token);
		EditorWindows windows;
		windows.set_view(&v);
		windows.begin_frame();
		windows.request(request::of(kind));
		windows.request(request::select_record("menus/other.mnu", {1, 1, 0}));
		windows.end_frame();
		EditorRequest first, second;
		CHECK(windows.take_request(first) && windows.take_request(second) && !windows.take_request(second), row.token);
		const EditorRequest &raised = first.kind == kind ? first : second;
		const bool waited = first.kind == K::SelectRecord && kind != K::SelectRecord;
		CHECK(waited == row.acts_on_saved, row.token);
		CHECK(raised.path == (row.names_active ? v.documents.active : std::string()), row.token);
		deferred += waited ? 1 : 0;
		named += raised.path.empty() ? 0 : 1;
	}
	CHECK(deferred == saved.size() && named == active.size(), "every kind asked");
	// Outside a frame nothing waits: a Save raised then is taken first.
	EditorWindows windows;
	windows.set_view(&v);
	windows.request(request::save());
	windows.request(request::select_record("menus/other.mnu", {1, 1, 0}));
	EditorRequest out;
	CHECK(windows.take_request(out) && out.kind == K::Save && out.path == v.documents.active, "outside a frame, in order");
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
	const auto item_rows = [&](const Document::Collection &c) {
		return std::string(document->kind_token(c.spec.kind)) == "items.item" && c.ids.size() == 2;
	};
	CHECK(items && items->has_toggle && has_field(items, "items.multiselect") &&
	              std::any_of(items->collections.begin(), items->collections.end(), item_rows),
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
			const NodeId id = request.address.child;
			CHECK(request.address.kind == kWindow && request.mode == SelectMode::Replace, "a click selects a window");
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
	CHECK(toggle && toggle->address == named(*document, "TITLE") && toggle->mode == SelectMode::Toggle,
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
			ranged.emplace_back(menu_test::window_of(*document, request.address)->name, request.mode);
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
		v.documents.open.push_back(extra);
		v.documents.active = extra->path();
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
		CHECK(plain && plain->path == extra->path() && plain->address == named(*extra, "BACK") &&
		              plain->mode == SelectMode::Replace,
		      "another file: no range from a row of the first");
		v.documents.open.pop_back();
		v.documents.active = document->path();
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
	              edit_of(dropped[0]).operation == EditOperation::Move && edit_of(dropped[0]).address == back &&
	              edit_of(dropped[0]).parent == title.child && edit_of(dropped[0]).position == SIZE_MAX,
	      "dropped on a window's middle: inside it, at the end");
	dropped = drag("BACK", bottom("TITLE"));
	CHECK(dropped.size() == 1 && edit_of(dropped[0]).operation == EditOperation::Move && edit_of(dropped[0]).parent == main.child &&
	              edit_of(dropped[0]).position == 2,
	      "dropped on a window's lower edge: after it");
	dropped = drag("TITLE", top("PANEL"));
	CHECK(dropped.size() == 1 && edit_of(dropped[0]).operation == EditOperation::Move && edit_of(dropped[0]).address == title &&
	              edit_of(dropped[0]).parent == main.child && edit_of(dropped[0]).position == 1,
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
	CHECK(indent && edit_of(*indent).operation == EditOperation::Move && edit_of(*indent).address == named(*document, "PANEL") &&
	              edit_of(*indent).parent == back.child,
	      "Indent: into the window above");
	ui.activate(item_id(menu_id, {"Duplicate"}));
	requests = ui.drain();
	const EditorRequest *duplicate = only(requests, EditorRequestKind::Duplicate);
	CHECK(duplicate && duplicate->path == document->path() && !only(requests, EditorRequestKind::EditRecord),
	      "Duplicate: the session's, each selected window right after itself");
	ui.activate(item_id(menu_id, {"Add window"}));
	requests = ui.drain();
	const EditorRequest *add = only(requests, EditorRequestKind::EditRecord);
	CHECK(add && edit_of(*add).operation == EditOperation::Add && edit_of(*add).address.kind == kWindow &&
	              edit_of(*add).parent == named(*document, "PANEL").child && edit_of(*add).field == "type" &&
	              std::get<std::string>(edit_of(*add).value) == "static",
	      "Add window: a window of the picked type inside the selection, one edit");
	select_in(v, named(*document, "CHOICES"));
	ui.frames(2);
	ui.activate(item_id(menu_id, {"Outdent"}));
	requests = ui.drain();
	const EditorRequest *outdent = only(requests, EditorRequestKind::EditRecord);
	CHECK(outdent && edit_of(*outdent).operation == EditOperation::Move && edit_of(*outdent).parent == main.child &&
	              edit_of(*outdent).position == 2,
	      "Outdent: right after the window that held it");
	ui.activate(item_id(menu_id, {"Duplicate screen"}));
	requests = ui.drain();
	const EditorRequest *copy_screen = only(requests, EditorRequestKind::EditRecord);
	const NodeAddress screen_address{screen.id, kScreen, 0};
	CHECK(copy_screen && edit_of(*copy_screen).operation == EditOperation::Duplicate &&
	              edit_of(*copy_screen).address == screen_address && edit_of(*copy_screen).position == 1,
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
	// INPART, a window the list's scrollbar part holds: not copyable (menu_clipboard's rule, the
	// canvas's too).
	NodeAddress in_part;
	const auto find_in_part = [&](const NodeAddress &record, const Document::Placement &) {
		if (record.kind == kWindow && menu_test::window_of(*document, record)->name == "INPART")
			in_part = record;
		return !in_part.child;
	};
	document->walk_records(*document->rows()[0], find_in_part);
	select_in(v, in_part);
	ui.frames(2);
	ui.drain();
	CHECK(!only(chord(ImGuiKey_C), EditorRequestKind::Copy) &&
	              !only(chord(ImGuiKey_D), EditorRequestKind::Duplicate),
	      "a window a part holds: no Copy or Duplicate");
	select_in(v, back);
	ui.frames(2);
	ui.drain();
	v.documents.clipboard = "\xEF\xBB\xBF<SCREEN></SCREEN>";
	ui.frames();
	// Where a Paste goes is the window's to say: after the selected window among its siblings.
	auto pasted_at = [&](const std::vector<EditorRequest> &raised, NodeId parent, size_t position) {
		const EditorRequest *paste = only(raised, EditorRequestKind::Paste);
		return paste && paste->path == document->path() && paste->paste_at.row == screen.id &&
		       paste->paste_at.parent == parent && paste->paste_at.position == position;
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
// The tooltips the last frame showed, each one's content height: a tooltip set in place of
// another hides that one.
std::vector<float> tooltips_shown() {
	std::vector<float> out;
	for (const ImGuiWindow *window : GImGui->Windows)
		if ((window->Flags & ImGuiWindowFlags_Tooltip) && window->Active && !window->Hidden)
			out.push_back(window->ContentSize.y);
	return out;
}

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
	CHECK(add && edit_of(*add).operation == EditOperation::Add && edit_of(*add).address.kind == menu_kind("action") &&
	              edit_of(*add).parent == back.child,
	      "a collection's Add goes into the record shown");

	// A row of it selected: the leaf's form, and the rows beside it with their toolbar.
	const NodeAddress second = menu_test::child_of(*document, back, "action", 1);
	select_in(v, second);
	ui.frames(3);
	ui.drain();
	ui.activate(item_id(actions, {"Up"}));
	requests = ui.drain();
	const EditorRequest *up = only(requests, EditorRequestKind::EditRecord);
	CHECK(up && edit_of(*up).operation == EditOperation::Move && edit_of(*up).address == second && edit_of(*up).position == 0,
	      "Up moves the selected row");
	ui.activate(item_id(actions, {"Duplicate"}));
	requests = ui.drain();
	const EditorRequest *duplicate = only(requests, EditorRequestKind::EditRecord);
	CHECK(duplicate && edit_of(*duplicate).operation == EditOperation::Duplicate && edit_of(*duplicate).position == 2,
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
	CHECK(virtual_key && edit_of(*virtual_key).address == hotkey && edit_of(*virtual_key).field == "virtual" &&
	              std::get<int64_t>(edit_of(*virtual_key).value) == 1,
	      "a switch in a cell sets the row's field");
	// The Virtual key heading, cut to its switch's narrow column, hovered: one tooltip, the
	// field's words, at once and still once the header's own tooltip for the label it cut
	// (shown after a delay) would show: in its place, not added to it.
	const ImGuiTable *keys = ImGui::TableFindByID(hotkeys);
	int switch_column = -1;
	for (int column = 0; keys && column < keys->ColumnsCount; ++column)
		if (std::strcmp(ImGui::TableGetColumnName(keys, column), "Virtual key") == 0)
			switch_column = column;
	CHECK(switch_column > 0, "the hotkeys' Virtual key column");
	if (switch_column > 0) {
		const ImGuiTableColumn &cut = keys->Columns[switch_column];
		CHECK(ui_kit::text_width("Virtual key") > cut.WorkMaxX - cut.WorkMinX,
		      "its heading is cut");
		ui.mouse(cut.WorkMinX + 2.0f, keys->OuterRect.Min.y + 4.0f);
		ui.frames(3);
		const std::vector<float> at_once = tooltips_shown();
		ui.frames(40);
		const std::vector<float> later = tooltips_shown();
		CHECK(at_once.size() == 1 && later.size() == 1 && later[0] == at_once[0],
		      "a cut heading hovered: one tooltip, the field's words alone");
		ui.away();
	}
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
	CHECK(verb && edit_of(*verb).address == second && edit_of(*verb).field == "type" &&
	              std::get<std::string>(edit_of(*verb).value) == "POP_SCREEN",
	      "a choice picked by its readable name writes its token");

	// TITLE's STRING switch, the first row of its block's form: the block left out.
	select_in(v, named(*document, "TITLE"));
	ui.frames(3);
	ui.drain();
	ui.activate(item_id(inspector, {"string", "fields", "string", "##value"}));
	requests = ui.drain();
	const EditorRequest *block = only(requests, EditorRequestKind::EditRecord);
	CHECK(block && edit_of(*block).field == "string" && std::get<int64_t>(edit_of(*block).value) == 0, "a block's switch");

	// S9k2: BACK and TITLE selected together (BACK the primary): the fields they share, the
	// type marked mixed; a switch sets both in one batch.
	const NodeAddress title = named(*document, "TITLE");
	v.documents.selection.primary = back;
	v.documents.selection.records = {back, title};
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
	CHECK(requests.size() == 2 && requests[0].kind == EditorRequestKind::EditRecord && edit_of(requests[0]).field == "hidden" &&
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
	v.documents.open.push_back(other);
	// A Problems row opens a file of the project the editor opens (problem_location): the
	// scan lists this one.
	AssetEntry other_entry;
	other_entry.logical_name = "extra.mnu";
	other_entry.relative_path = other->path();
	other_entry.kind = AssetKind::Menu;
	editor_test::own(v.project.scan).entries.push_back(other_entry);
	editor_test::own(v.project.scan).index();
	// A required file the project lacks names no file of it: its row (showing the file it is
	// about) opens nothing.
	Diagnostic lacking = editor_test::finding_of(DiagnosticSeverity::Error, "requirement.missing", "Missing required file gametext.bin.");
	lacking.subject = RequirementSubject{"gametext", "gametext.bin"};
	v.findings.diagnostics = {lacking};
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
	editor_test::own(v.project.scan).entries.push_back(font_entry);
	editor_test::own(v.project.scan).index();
	v.findings.diagnostics = {editor_test::finding_of(DiagnosticSeverity::Warning, "graph.unreadable", "The font could not be read.",
	                                 font_entry.relative_path)};
	v.revisions.touch(ViewConcern::Files);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.focus("Problems");
	ui.drain();
	ui.click(problems_lines().at(0, 2));
	requests = ui.drain();
	CHECK(requests.size() == 1 && requests[0].kind == EditorRequestKind::ShowInFiles &&
	              requests[0].path == font_entry.relative_path && !requests[0].ask_name,
	      "a font's row shows it in Files");
	Diagnostic finding = editor_test::finding_of(DiagnosticSeverity::Error, "menu.duplicate_screen", "A finding in the other menu.", other->path());
	finding.row_id = other->rows()[0]->id;
	finding.record_kind = kScreen;
	v.findings.diagnostics = {finding};
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
	v.dialogs.unsaved_prompt.open = true;
	v.dialogs.unsaved_prompt.action = EditorRequestKind::Quit;
	v.dialogs.unsaved_prompt.files = {document->path()};
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
	v.dialogs.unsaved_prompt = DialogsView::UnsavedPrompt();
	v.revisions.touch(ViewConcern::Dialogs);
	ui.frames(2);
	ui.drain();
}

// --- S9k1, S13 V2, V5: the preview window's canvas ------------------------------------------

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

// The Preview window over a hand-made view of the layout menu (S13 V5): the menu open and active,
// its first screen the Preview's target, the viewports the view shares pumped before each frame as
// the Shell pumps them (the SetViewports the windows raise served), Preview focused and, unless
// asked to Fit, at 100% (a pixel a design unit). The menu's viewport, its device, and the scope of
// its view's items in the Preview window.
struct LayoutPreview {
	editor_test::TempProjectDir dir;
	std::shared_ptr<MnuDocument> document = std::make_shared<MnuDocument>();
	SessionView v;
	HandViewports shell;
	Ui ui;
	const MenuViewport *menu = nullptr;
	DrawnDevice *device = nullptr;
	ImGuiID scope = 0;

	explicit LayoutPreview(const char *name) : dir(name) {}
	bool start(bool fit = false) {
		Diagnostic error;
		CHECK(editor_test::write_text(dir.file("layout.mnu"), kLayoutMenu), "layout fixture");
		CHECK(document->load(dir.file("layout.mnu"), "layout.mnu", AssetKind::Menu, "jo", error), error.message.c_str());
		if (document->rows().empty()) return false;
		const Node &screen = *document->rows()[0];
		v = menu_view(document);
		v.documents.previews[ViewportKind::Menu].path = document->path();
		v.documents.previews[ViewportKind::Menu].part = screen.id;
		// The kind the Preview window shows, as the session derives it with its targets.
		v.documents.preview_shown = preview_kind(v.documents, v.documents.preview_shown);
		select_in(v, {screen.id, kScreen, 0});
		shell.bind(v);
		ui.windows.set_view(&v);
		ui.windows.set_devices(&shell.devices.cache);
		ui.pump = [this] { shell.pump(ui.windows, v); };
		ui.frames(6);
		ui.focus("Preview");
		menu = static_cast<const MenuViewport *>(shell.find(document->path(), ViewportKind::Menu));
		device = shell.device(document->path(), ViewportKind::Menu);
		CHECK(menu && menu->status() == ViewportStatus::Ready && device && device->width > 0,
				"the screen compiled, drawn on its device");
		if (!menu || !device) return false;
		scope = item_id(Ui::window_id("Preview"), {"menu", document->path().c_str()});
		if (!fit) {
			ui.activate(item_id(scope, {"Zoom"}));
			ui.activate(item_id(ImHashStr("##Combo_00"), {"100%"}));
			ui.frames(2);
		}
		ui.drain();
		return true;
	}
	// A design point's pixel on the screen.
	ImVec2 at(float x, float y) const {
		return ImVec2(device->origin.x + x * float(device->width) / 800.0f, device->origin.y + y * float(device->height) / 600.0f);
	}
	// The pump back, after a test held it.
	void pumping() { ui.pump = [this] { shell.pump(ui.windows, v); }; }
};

// A Set of one field.
Edit set_edit(const NodeAddress &address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// S13 V2, V5: the canvas through the menu's viewport view in the Preview window, on the null
// backend (its rules are its portable half's, tests/editor/canvas_test.cpp: preview/canvas_gesture,
// menu_canvas), over the viewports a hand-made view shares and a device drawing an invisible
// button: Fit at the design's 4:3, then 100% from the toolbar; a click selects what the game's hit
// test finds; a drag of the selected window is one gesture of Sets on the grid, then its end; the
// arrows through the canvas's key channel (Right a unit, a nudge one gesture while held and ended
// when let go, Shift+Up 8, the focus taken mid-nudge ending it once); a stale picture (the menu
// edited since the pump) maps nothing; the held state follows the selection, Checked only where the
// type has one, each a SetViewport of the options; Ctrl+wheel steps the zoom about the mouse; a
// viewport with nothing to show says why; with no device the canvas draws nothing and raises
// nothing. The gestures' ends when the view stops drawing, and the view hidden taking no key, are
// test_preview_gestures_end's; the several windows, the clipboard's keys and Arrange
// test_preview_several_windows_ui's.
void test_preview_canvas_smoke() {
	LayoutPreview preview("opennova_editor_ui_preview_test");
	if (!preview.start(true)) return;
	Ui &ui = preview.ui;
	SessionView &v = preview.v;
	const std::shared_ptr<MnuDocument> &document = preview.document;
	DrawnDevice &fake = *preview.device;
	const MenuViewport &menu = *preview.menu;
	CHECK(ui.windows.pending_requests() == 0, "drawing the preview raises nothing");
	CHECK(fake.width * 3 == fake.height * 4, "Fit: the design's 4:3");
	// 100% from the toolbar: a pixel is a design unit (ImGui floors the mouse to pixels).
	ui.activate(item_id(preview.scope, {"Zoom"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"100%"}));
	ui.frames(2);
	CHECK(fake.width == 800 && fake.height == 600, "100%");
	ui.drain();
	const NodeAddress box = named(*document, "BOX"), other = named(*document, "OTHER");
	auto at = [&](float x, float y) { return preview.at(x, y); };
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
	ui.click(at(200.0f, 150.0f));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *picked = only(requests, EditorRequestKind::SelectRecord);
	CHECK(picked && picked->address == box && picked->mode == SelectMode::Replace, "a click selects BOX");
	CHECK(!only(requests, EditorRequestKind::EditRecord), "a click edits nothing");

	// BOX selected: dragged by (40, 21), snapped: one gesture of Sets, then its end.
	select_in(v, box);
	ui.frames(2);
	ui.drain();
	requests = drag(at(200.0f, 150.0f), 40.0f, 21.0f);
	uint64_t gesture = 0;
	size_t count = 0;
	std::vector<Edit> last = batches(requests, gesture, count);
	CHECK(count >= 2 && gesture != 0, "a drag's steps share one gesture");
	CHECK(set_value(last, "position.left") == 144 && set_value(last, "position.top") == 120,
			"the last step: moved and snapped on the grid of 8");
	CHECK(!requests.empty() && requests.back().kind == EditorRequestKind::EndEdit && requests.back().path == document->path(),
	      "release ends the gesture");

	// The arrows through the canvas's key channel: Right moves BOX a unit, one gesture while held
	// and its end when let go; Shift+Up moves it 8.
	auto nudge = [&](ImGuiKey key, bool shift) {
		if (shift)
			ui.key(ImGuiMod_Shift, true);
		ui.key(key, true);
		ui.key(key, false);
		if (shift)
			ui.key(ImGuiMod_Shift, false);
		return ui.drain();
	};
	requests = nudge(ImGuiKey_RightArrow, false);
	last = batches(requests, gesture, count);
	CHECK(count == 1 && gesture != 0 && set_value(last, "position.left") == 101 &&
					set_value(last, "position.right") == 301,
			"Right moves BOX a unit");
	CHECK(!requests.empty() && requests.back().kind == EditorRequestKind::EndEdit,
			"letting go ends the nudge");
	requests = nudge(ImGuiKey_UpArrow, true);
	last = batches(requests, gesture, count);
	CHECK(set_value(last, "position.top") == 92 && set_value(last, "position.bottom") == 192,
			"Shift+Up moves it 8");
	// An arrow held while another window takes the focus: the nudge's one end then; let go
	// after, nothing.
	ui.key(ImGuiKey_RightArrow, true);
	CHECK(only(ui.drain(), EditorRequestKind::EditRecord) != nullptr, "a nudge's first step");
	ui.focus("Document");
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::EndEdit) && requests[0].path == document->path(),
			"the focus taken mid-nudge: its one end");
	ui.key(ImGuiKey_RightArrow, false);
	CHECK(ui.drain().empty(), "letting go of the arrow raises nothing");
	ui.focus("Preview");
	ui.drain();

	// A picture of another revision maps nothing, through the view either: the menu edited, no pump
	// since; undone, the picture is the menu's again.
	ui.pump = nullptr;
	Diagnostic error;
	CHECK(document->apply(set_edit(box, "position.bottom", int64_t(208)), error), "the menu edited");
	ui.click(at(500.0f, 350.0f));
	CHECK(!only(ui.drain(), EditorRequestKind::SelectRecord), "a stale picture selects nothing");
	requests = drag(at(200.0f, 150.0f), 40.0f, 0.0f);
	CHECK(!only(requests, EditorRequestKind::EditRecord), "a stale picture drags nothing");
	document->undo();
	preview.pumping();

	// The held state follows the selection; Checked only where the type has one (each a SetViewport
	// of the options, which the pump serves as the session does).
	ui.focus("Preview");
	CHECK(preview.shell.set(v, document->path(),
				  (R"({"options": {"force_state": "mouseover", "force_id": )" + std::to_string(box.child) + "}}").c_str()),
			"BOX held under the mouse");
	select_in(v, other);
	ui.frames(2);
	CHECK(menu.options().force_window == other.child && menu.options().force_state == opennova::menu::kStateMouseover,
	      "the held state moves to the selected window");
	ui.activate(item_id(preview.scope, {"Checked"}));
	ui.frames();
	CHECK(menu.options().checked && menu.options().force_window == other.child, "a check box can be held checked");
	select_in(v, box);
	ui.frames(2);
	CHECK(menu.options().force_window == box.child && !menu.options().checked, "a plain window lets the check go");
	ui.drain();

	// Ctrl+wheel over the picture steps the zoom about the mouse: 100% to 150%.
	const ImVec2 over = at(100.0f, 100.0f);
	ui.mouse(over.x, over.y);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
	ImGui::GetIO().AddMouseWheelEvent(0.0f, 1.0f);
	ui.frames(2);
	ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
	ui.frames(2);
	CHECK(fake.width == 1200 && fake.height == 900, "Ctrl+wheel zooms in to 150%");
	ui.drain();

	// A viewport with nothing to show says why (its reason's sentence): no project; a menu the game
	// could not read, until it changes.
	v.project.open = false;
	v.revisions.touch(ViewConcern::Project);
	CHECK(logged_frame(ui).find(menu_screen_status_message(MenuScreenStatus::NoProject, std::string())) != std::string::npos,
			"no project: what to open");
	v.project.open = true;
	v.revisions.touch(ViewConcern::Project);
	CHECK(document->apply(set_edit(box, "string.justify", std::string("CEN\"TER")), error), "a justify the game cannot read");
	CHECK(logged_frame(ui).find("The game could not read this menu as it stands") != std::string::npos,
			"a menu the game could not read: why");
	document->undo();
	ui.frames(2);
	CHECK(menu.status() == ViewportStatus::Ready, "undone: the screen again");
	// No device (a headless Shell): the canvas keeps the picture's room, draws nothing, raises nothing.
	ui.windows.set_devices(nullptr);
	const int draws = fake.draws;
	ui.frames(3);
	CHECK(fake.draws == draws && ui.windows.pending_requests() == 0, "no device: nothing drawn, nothing raised");
	ui.windows.set_devices(&preview.shell.devices.cache);
	ui.frames(2);
	CHECK(fake.draws > draws, "the device again: drawn");
}

// S11d, S13 V5: a gesture the menu's view began ends once, for the menu it began in, whenever the
// view stops drawing mid-gesture: the model's viewport shown (a model made the active document)
// during a drag or a held nudge, and Preview closed during either (the workspace's frame bracket
// ends what a window the pass skipped left open); after that, letting go raises nothing. The view
// not drawn takes no key: with the model's shown, an arrow, Esc and Space raise nothing.
void test_preview_gestures_end() {
	LayoutPreview preview("opennova_editor_ui_preview_gestures_test");
	if (!preview.start()) return;
	Ui &ui = preview.ui;
	SessionView &v = preview.v;
	const std::shared_ptr<MnuDocument> &document = preview.document;
	auto model = std::make_shared<ModelDocument>();
	Diagnostic error;
	CHECK(model->load(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/armory.3di", "models/armory.3di",
	                  AssetKind::Model, "jo", error),
	      "a model");
	select_in(v, named(*document, "BOX"));
	ui.frames(2);
	ui.drain();
	devtools::Window *preview_window = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count(); ++i)
		if (std::strcmp(ui.windows.pass().window(i).title(), "Preview") == 0) preview_window = &ui.windows.pass().window(i);
	CHECK(preview_window && preview_window->is_closeable(), "Preview has a close button");
	if (!preview_window) return;
	// The model the active document, or the menu again.
	const auto show_model = [&](bool on) {
		v.documents.open = on ? std::vector<std::shared_ptr<const DocumentBase>>{document, model}
		                      : std::vector<std::shared_ptr<const DocumentBase>>{document};
		v.documents.previews[ViewportKind::Model].path = on ? model->path() : std::string();
		v.documents.active = on ? model->path() : document->path();
		v.documents.preview_shown = preview_kind(v.documents, v.documents.preview_shown);
		v.revisions.touch(ViewConcern::Documents);
		v.revisions.touch(ViewConcern::Selection);
		ui.frames(2);
	};
	// Exactly one request, the menu's gesture's end.
	const auto one_end = [&](const std::vector<EditorRequest> &requests) {
		return one(requests, EditorRequestKind::EndEdit) && requests[0].path == document->path();
	};
	const ImVec2 box = preview.at(200.0f, 150.0f); // BOX at 100%
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
	CHECK(preview.shell.find(model->path(), ViewportKind::Model) &&
					preview.shell.device(model->path(), ViewportKind::Model),
			"the model's viewport shown, on a device of its own");
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
	CHECK(!preview.shell.find(model->path(), ViewportKind::Model), "the model closed: its viewport gone");

	// Preview closed mid-drag, then mid-nudge: the frame bracket ends each once.
	ui.focus("Preview");
	ui.drain();
	CHECK(start_drag(), "a drag's first step");
	preview_window->open = false;
	ui.frames(2);
	CHECK(one_end(ui.drain()), "Preview closed mid-drag: the drag's one end");
	ui.button(false);
	ui.frames(2);
	preview_window->open = true;
	ui.frames(3);
	CHECK(ui.drain().empty(), "let go and opened again: nothing");
	ui.focus("Preview");
	ui.drain();
	CHECK(start_nudge(), "a nudge's first step");
	preview_window->open = false;
	ui.frames(2);
	CHECK(one_end(ui.drain()), "Preview closed mid-nudge: the nudge's one end");
	ui.key(ImGuiKey_RightArrow, false);
	preview_window->open = true;
	ui.frames(3);
	CHECK(ui.drain().empty(), "let go and opened again: nothing");

	// The menu's view hidden behind the model's: its keys do nothing.
	show_model(true);
	ui.focus("Preview");
	ui.drain();
	for (const ImGuiKey key : {ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_Escape, ImGuiKey_Space}) {
		ui.key(key, true);
		ui.key(key, false);
	}
	CHECK(ui.drain().empty(), "the hidden menu view takes no key");
}

// S9k2: several windows on the canvas. Shift+click adds a window, Ctrl+click toggles one; a
// drag from the screen's background selects what its box touches, in one selection (none: the
// screen); a drag of a selected window moves every selected one in one batch per step, one
// gesture; the arrows nudge them all; Ctrl+C / X / V / D; the toolbar's Arrange aligns them.
void test_preview_several_windows_ui() {
	LayoutPreview preview("opennova_editor_ui_preview_multi_test");
	if (!preview.start()) return;
	Ui &ui = preview.ui;
	SessionView &v = preview.v;
	const std::shared_ptr<MnuDocument> &document = preview.document;
	const Node &screen = *document->rows()[0];
	const NodeAddress main = named(*document, "MAIN"), box = named(*document, "BOX"), other = named(*document, "OTHER");
	select_in(v, box);
	ui.frames(2);
	ui.drain();
	const ImGuiID preview_id = preview.scope;
	auto at = [&](float x, float y) { return preview.at(x, y); };
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
			if (request.kind == EditorRequestKind::SelectRecord) out.emplace_back(request.address, request.mode);
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
	v.documents.selection.primary = other;
	v.documents.selection.records = {box, other};
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

	// A drag from MAIN's empty part (a root window not selected) selects what the box touches, in
	// one selection (S13 D7, V5): BOX and OTHER, the last the primary.
	requests = drag(at(50.0f, 500.0f), 400.0f, -390.0f);
	const EditorRequest *boxed = only(requests, EditorRequestKind::SelectRecord);
	CHECK(boxed && boxed->address == other && boxed->mode == SelectMode::Replace &&
	              boxed->records == (std::vector<NodeAddress>{box, other}) && !only(requests, EditorRequestKind::EditRecord),
	      "the marquee selects BOX and OTHER in one selection, not TINY or MAIN");
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
	v.documents.clipboard = "\xEF\xBB\xBF<SCREEN></SCREEN>";
	ui.frames();
	requests = chord(ImGuiKey_V);
	const EditorRequest *paste = only(requests, EditorRequestKind::Paste);
	CHECK(paste && paste->paste_at.row == screen.id && paste->paste_at.parent == main.child && paste->paste_at.position == 2,
	      "Ctrl+V pastes after the primary window (OTHER)");
	// Copy, Cut and Duplicate take the selection as it is: with the screen among it (or a
	// window's list row) the preview raises none of them, as the menu view does not.
	v.documents.selection.records = {screen_address, box, other};
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(2);
	ui.drain();
	CHECK(!only(chord(ImGuiKey_C), EditorRequestKind::Copy) && !only(chord(ImGuiKey_X), EditorRequestKind::Cut) &&
	              !only(chord(ImGuiKey_D), EditorRequestKind::Duplicate),
	      "the screen selected with the windows: no Copy, Cut or Duplicate");
	v.documents.selection.records = {box, other};
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
	v.documents.selection.primary = other;
	v.documents.selection.records = {main, other};
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
	// A path cut in its middle, its start and its end kept (the review's L9: the end tells two folders apart).
	{
		const std::string path = "C:/Users/someone/Documents/My Mods/Builds/For Players";
		const float narrow = ImGui::CalcTextSize("C:/Users/some...For Players").x;
		const std::string cut = ui_kit::fit_middle(path, narrow);
		CHECK(cut.size() < path.size() && cut.find("...") != std::string::npos && cut.rfind("C:/", 0) == 0 &&
		              cut.size() >= 7 && cut.compare(cut.size() - 7, 7, "Players") == 0 &&
		              ImGui::CalcTextSize(cut.c_str()).x <= narrow + 0.5f,
		      "a path cut in its middle");
		CHECK(ui_kit::fit_middle(path, 10000.0f) == path, "a path that fits is whole");
	}
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
	// The manifest row's failure class: whether the boot stops without it (the summary's first line).
	if (const opennova::gameprofile::RequiredResource *manifest = opennova::gameprofile::gameprofile_required_resource_by_role(role))
		row.severity = manifest->severity;
	return row;
}

Diagnostic missing_finding(const char *role, const char *name) {
	Diagnostic d = editor_test::finding_of(DiagnosticSeverity::Error, "requirement.missing", std::string("Missing required file ") + name + ".");
	d.subject = RequirementSubject{role, name};
	return d;
}

constexpr const char *kGametext = "Missing required file gametext.bin";
constexpr const char *kMainMenu = "Missing required file main.mnu";

// Two required files the project lacks, each made by a factory (the game data has
// gametext.bin; a spare string table and two menus could stand in for them), a catalog
// error on a record's field, a warning in the active menu and a note in the other open one.
SessionView problems_view(const std::shared_ptr<MnuDocument> &a, const std::shared_ptr<MnuDocument> &b) {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Problems";
	editor_test::own(v.project.scan).entries = {file_entry("items.def", "defs/items.def", AssetKind::ItemDefs), file_entry("a.mnu", "menus/a.mnu", AssetKind::Menu),
	                  file_entry("b.mnu", "menus/b.mnu", AssetKind::Menu),
	                  file_entry("spare.bin", "strings/spare.bin", AssetKind::Strings)};
	editor_test::own(v.project.scan).index();
	editor_test::own(v.project.requirements).rows = { missing_row("gametext", "gametext.bin",
															  AssetKind::Strings),
		missing_row("main_menu", "main.mnu", AssetKind::Menu) };
	editor_test::own(v.project.requirements).required_total = 2;
	editor_test::own(v.project.requirements).required_missing = 2;
	v.project.retail_files = {"gametext.bin"};
	v.documents.open = {a, b};
	v.documents.active = a->path();
	Diagnostic type = editor_test::finding_of(DiagnosticSeverity::Error, "catalog.item_type", "Alpha: choose an item type.",
	                                          "defs/items.def", "type");
	type.record = "Marker";
	type.line = 12;
	type.row_id = 4;
	type.record_kind = 2;
	v.findings.diagnostics = {missing_finding("gametext", "gametext.bin"), missing_finding("main_menu", "main.mnu"), type,
	                 editor_test::finding_of(DiagnosticSeverity::Warning, "menu.duplicate_window", "Bravo: two windows are named GO.",
	                                         "menus/a.mnu"),
	                 editor_test::finding_of(DiagnosticSeverity::Info, "style.unused", "Charlie: nothing uses it.", "menus/b.mnu")};
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
	CHECK(text.find("The game will not start: 2 required files are missing.") != std::string::npos, "the summary");
	CHECK(text.find("items.def:12 - Marker - type") != std::string::npos, "where a finding is");
	CHECK(text.find("Import gametext.bin from the game data...") != std::string::npos,
	      "a required file's first fix: the game's own copy, which the game data has");

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
	ui.activate(item_id(window, {"a.mnu###scope_active"}));
	CHECK(listed(ui) == List({"Bravo:"}), "the active file's");
	ui.activate(item_id(window, {"Open files###scope_open"}));
	CHECK(listed(ui) == List({"Bravo:", "Charlie:"}), "the open files'");
	ui.activate(item_id(window, {"Whole project###scope_project"}));
	CHECK(listed(ui).size() == 5, "the project's");

	// What a build is refused for, shown whatever hid it (the review's M7): the active file's scope (a required
	// file's finding names no file) and the errors hidden; "Show them" (the build result's, the menu bar's) lists
	// every refusal, and turning "Blocks the build" off puts the filters back.
	ui.activate(item_id(window, {"a.mnu###scope_active"}));
	ui.activate(item_id(window, {"###errors"}));
	CHECK(listed(ui) == List({"Bravo:"}), "the active file's, errors hidden");
	auto *problems = const_cast<ProblemsWindow *>(dynamic_cast<const ProblemsWindow *>(find_window(ui.windows.pass(), "Problems")));
	CHECK(problems != nullptr, "the Problems window");
	if (problems) problems->show_blocking();
	ui.frames(2);
	CHECK(listed(ui) == List({kGametext, kMainMenu}), "every refusal shown");
	ui.activate(item_id(window, {"###blocking"}));
	CHECK(listed(ui) == List({"Bravo:"}), "turned off: the filters as they were");
	ui.activate(item_id(window, {"###errors"}));
	ui.activate(item_id(window, {"Whole project###scope_project"}));
	CHECK(listed(ui).size() == 5, "the project's again");

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
	// Apply the game's own copy of what the game data has and a Create of the rest. The other
	// groups, a finding each with no fix, have none.
	CHECK(ui.drain().empty(), "folding raises nothing");
	ui.click(problems_lines().fix(0));
	ui.frames(2);
	CHECK(confirmation() && ui.drain().empty(), "Fix all asks first");
	text = logged_frame(ui);
	CHECK(text.find("Import gametext.bin from the game data") != std::string::npos &&
	              text.find("Create main.mnu. It starts as placeholder content") != std::string::npos &&
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
	CHECK(requests.size() == 2 && requests[0].kind == EditorRequestKind::CreateMissing &&
	              requests[0].roles == List({"main_menu"}) && requests[1].kind == EditorRequestKind::PreviewInstallImport &&
	              requests[1].names == List({"gametext.bin"}),
	      "Apply: main.mnu created, then the game's own gametext.bin (its preview last: an operation)");
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
	CHECK(in_order(text, {kGametext, "Import gametext.bin from the game data...", "Create a placeholder gametext.bin",
	                      "Use spare.bin as gametext.bin", "Renames spare.bin to gametext.bin.", "requirement.missing"}),
	      "the selected row: every fix with what it does, and its code");
	CHECK(text.find("As the original was seen to do:") != std::string::npos, "the manifest's own record of it, the cited detail");
	CHECK(text.find("Blocks the build") != std::string::npos, "a row a build is refused for says so");
	ui.click(problems_lines().at(0, 2));
	ui.away();
	CHECK(logged_frame(ui).find("requirement.missing") == std::string::npos, "a second click folds it back");
	ui.click(problems_lines().at(2, 2));
	requests = ui.drain();
	const EditorRequest *opened = one(requests, EditorRequestKind::OpenDocument);
	CHECK(opened && opened->path == "defs/items.def" && opened->address == (NodeAddress{4, 2, 0}) && opened->field == "type",
	      "a catalog finding opens its record at the field");
	ui.click(problems_lines().at(2, 2));
	ui.drain();

	// A required file's Fix brings the game's own; its More lists every fix; a Use fix waits for Apply.
	ui.click(problems_lines().fix(0));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PreviewInstallImport) && requests[0].names == List({"gametext.bin"}),
	      "a required file's Fix imports the game's own");
	ui.click(problems_lines().more(0, "Import gametext.bin from the game data..."));
	text = logged_frame(ui);
	CHECK(text.find("Create a placeholder gametext.bin") != std::string::npos &&
	              text.find("Use spare.bin as gametext.bin") != std::string::npos,
	      "More lists the placeholder and Use");
	ui.activate(popup_item(item_id(window, {"more"}), "Use spare.bin as gametext.bin"));
	CHECK(confirmation() && ui.drain().empty(), "a Use fix waits for Apply");
	ui.away();
	CHECK(logged_frame(ui).find("Renames spare.bin to gametext.bin.") != std::string::npos, "saying what it renames");
	ui.click(confirmation_button(false));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::AssignRequirement) && requests[0].path == "strings/spare.bin" &&
	              requests[0].role == "gametext",
	      "Apply renames it");

	// The summary from the gate (what stops the game, then what it starts without) and its Fix alls:
	// one import list for what the game data has (the game's own copies first), one Create for the
	// rest, each asking first.
	editor_test::own(v.project.requirements)
			.rows.push_back(missing_row("cmap_menu", "cmap.mnu", AssetKind::Menu));
	editor_test::own(v.project.requirements).required_missing = 3;
	v.project.retail_files = {"cmap.mnu", "gametext.bin"};
	v.findings.diagnostics.push_back(missing_finding("cmap_menu", "cmap.mnu"));
	v.revisions.touch(ViewConcern::Files);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	CHECK(in_order(logged_frame(ui), {"The game will not start: 2 required files are missing.",
	                                  "1 more file the game reads is missing: part of it will not work.",
	                                  "Import 2 from the game data...", "Create 1 placeholder"}),
	      "the summary's Fix alls");
	const ImGuiID summary = item_id(window, {v.project.root.c_str(), "required"});
	ui.activate(item_id(pushed(summary, static_cast<int>(EditorRequestKind::CreateMissing)), {"###fix"}));
	CHECK(confirmation() && ui.drain().empty(), "the summary's Create asks first");
	ui.click(confirmation_button(false));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].roles == List({"main_menu"}),
	      "one Create for what the game data lacks");
	ui.frames(2);
	ui.activate(item_id(pushed(summary, static_cast<int>(EditorRequestKind::PreviewInstallImport)), {"###fix"}));
	ui.click(confirmation_button(false));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::PreviewInstallImport) && requests[0].names == List({"gametext.bin", "cmap.mnu"}),
	      "one import list for what the game data has");

	// An optional file the project lacks is a note with the same fixes: its Fix creates it.
	editor_test::own(v.project.requirements).rows.push_back(missing_row("brand_style", "brand.mns", AssetKind::MenuStyle, false));
	Diagnostic optional = editor_test::finding_of(DiagnosticSeverity::Info, "requirement.optional_missing",
	                                              "Optional file brand.mns is not in the project.");
	optional.subject = RequirementSubject{"brand_style", "brand.mns"};
	v.findings.diagnostics.push_back(optional);
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	ui.away();
	// Errors (the three required files and the catalog's), the warning, then the notes.
	CHECK(in_order(logged_frame(ui), {"Charlie:", "Optional file brand.mns", "Create brand.mns"}), "the optional file's note");
	ui.drain();
	ui.click(problems_lines().fix(6));
	requests = ui.drain();
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].roles == List({"brand_style"}),
	      "an optional file's Fix creates it");
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
	v.project.retail_files.clear(); // no game install: placeholders are the Fix all
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
	const std::string root = v.project.root;
	v.project.root = "C:/mods/Another";
	v.revisions.touch(ViewConcern::Project);
	ui.frames(2);
	CHECK(!confirmation(), "another project closes it");
	v.project.root = root;
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
	v.findings.diagnostics.erase(v.findings.diagnostics.begin() + 1);
	editor_test::own(v.project.requirements).rows[1].state = RequirementState::Present;
	editor_test::own(v.project.requirements).required_missing = 1;
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
	CHECK(one(requests, EditorRequestKind::CreateMissing) && requests[0].roles == List({"gametext"}), "then the new list alone");

	// More's list follows its finding: main.mnu's again, gametext's gone before it.
	replace_view(v, problems_view(menu_at(dir, "a.mnu", "menus/a.mnu"),
			menu_at(dir, "b.mnu", "menus/b.mnu")));
	ui.activate(item_id(window, {"Group"}));
	ui.activate(item_id(ImHashStr("##Combo_00"), {"None"}));
	ui.frames(2);
	ui.click(problems_lines().more(1, "Create main.mnu"));
	ui.away();
	CHECK(logged_frame(ui).find("Use a.mnu as main.mnu") != std::string::npos, "More lists main.mnu's fixes");
	v.findings.diagnostics.erase(v.findings.diagnostics.begin());
	v.revisions.touch(ViewConcern::Findings);
	ui.frames(2);
	text = logged_frame(ui);
	CHECK(text.find("Use a.mnu as main.mnu") != std::string::npos && text.find("Use spare.bin") == std::string::npos,
	      "still main.mnu's when a finding before it goes");
	v.findings.diagnostics.erase(v.findings.diagnostics.begin());
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
	v.project.open = true;
	v.project.root = "C:/mods/Rewrite";
	v.findings.diagnostics = {editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.ignored_input", "Delta: a key the game ignores.",
	                                 "defs/weapon.def"),
	                 editor_test::finding_of(DiagnosticSeverity::Error, "catalog.unserializable", "Echo: this cannot be written.",
	                                         "defs/weapon.def"),
	                 editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.ignored_input", "Foxtrot: a key the game ignores.",
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

// A thousand findings in fifty catalogs: only the lines that show are drawn and only their
// fixes asked (fixes_for is never run for a line not drawn). Scrolled to the middle, a click
// opens the finding under the mouse; one expanded there, the lines after it sit right under
// it and those before where they were. Grouped by file (a header before each fifty's twenty),
// a click in the middle opens the finding under it too.
void test_problems_many() {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Many";
	for (int file = 0; file < 50; ++file) {
		const std::string name = "f" + std::to_string(file) + ".def";
		editor_test::own(v.project.scan)
				.entries.push_back(file_entry(name, "defs/" + name, AssetKind::ItemDefs));
	}
	for (size_t i = 0; i < 1000; ++i) {
		Diagnostic d = editor_test::finding_of(DiagnosticSeverity::Warning, "catalog.ignored_input",
		                                       "Finding " + std::to_string(i) + ": a line the game ignores.",
		                                       v.project.scan->entries[i / 20].relative_path, "name");
		d.row_id = i + 1;
		d.record_kind = 2;
		d.line = i + 1;
		v.findings.diagnostics.push_back(d);
	}
	editor_test::own(v.project.scan).index();
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
		return open ? open->address.row : 0;
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

// ADR 0046 S15: the findings about the game's own data apart. Two findings in a file of the
// modder's and three in one the project holds as the game install serves it: the modder's listed
// first with no header, the game's own data's under their group last, folded at first (its findings
// not drawn); the severities count the modder's alone and the shown count says the rest. A click on
// the group's header opens it. The active file's alone, one click away ("Only <file>"), and back.
void test_problems_original() {
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Original";
	for (const char *name : {"mine.def", "shipped.def"})
		editor_test::own(v.project.scan).entries.push_back(file_entry(name, std::string("defs/") + name, AssetKind::ItemDefs));
	editor_test::own(v.project.scan).index();
	// Missing textures: findings no build gates on (a gating one is never folded).
	const auto add = [&v](DiagnosticSeverity severity, const char *message, const char *path) {
		Diagnostic d = editor_test::finding_of(severity, "reference.missing", message, path, "name");
		d.subject = ReferenceSubject{ReferenceKind::Texture, "skin.tga", std::string(), 0};
		v.findings.diagnostics.push_back(d);
	};
	add(DiagnosticSeverity::Error, "Alpha: yours.", "defs/mine.def");
	add(DiagnosticSeverity::Warning, "Bravo: yours too.", "defs/mine.def");
	add(DiagnosticSeverity::Error, "Charlie: the game's.", "defs/shipped.def");
	add(DiagnosticSeverity::Error, "Delta: the game's.", "defs/shipped.def");
	add(DiagnosticSeverity::Warning, "Echo: the game's.", "defs/shipped.def");
	const OriginalData shipped = editor_test::originals_of(v.findings.diagnostics, {"defs/shipped.def"});
	v.findings.originals = std::make_shared<const OriginalData>(shipped);
	v.documents.active = "defs/mine.def";
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Problems");
	problems_grouping(ui, "None");
	ui.away();
	ui.drain();
	std::string text = logged_frame(ui);
	CHECK(text.find("Alpha:") != std::string::npos && text.find("Bravo:") != std::string::npos, "the modder's findings listed");
	CHECK(text.find(kOriginalGroupTitle) != std::string::npos && text.find("Charlie:") == std::string::npos,
	      "the game's own data's under their group, folded at first");
	CHECK(text.find("Errors 1") != std::string::npos && text.find("Warnings 1") != std::string::npos,
	      "the severities count the modder's alone");
	CHECK(text.find("5 of 5 (3 in the game's own data)") != std::string::npos, "the shown count says the rest");
	ui.click(problems_lines().at(2, 1)); // the group's header, after the modder's two
	ui.away();
	ui.frames(2);
	text = logged_frame(ui);
	CHECK(text.find("Charlie:") != std::string::npos && text.find("Echo:") != std::string::npos, "opened: its findings");
	// The scope, one control: the active file by its name, one click; the whole project's again.
	ui.activate(item_id(Ui::window_id("Problems"), {"mine.def###scope_active"}));
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Alpha:") != std::string::npos && text.find("Charlie:") == std::string::npos &&
	              text.find(kOriginalGroupTitle) == std::string::npos,
	      "mine.def: the active file's alone, one click");
	ui.activate(item_id(Ui::window_id("Problems"), {"Whole project###scope_project"}));
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Charlie:") != std::string::npos, "the whole project's again");
	CHECK(ui.windows.pending_requests() == 0, "drawing raises nothing");
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
	v.project.open = true;
	v.project.root = dir.root();
	AssetEntry entry;
	entry.logical_name = "menu_style.mns";
	entry.relative_path = "menu_style.mns";
	entry.kind = AssetKind::MenuStyle;
	editor_test::own(v.project.scan).entries.push_back(entry);
	editor_test::own(v.project.scan).index();
	v.documents.open.push_back(document);
	v.documents.active = document->path();
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
	CHECK(add && edit_of(*add).operation == EditOperation::Add && edit_of(*add).address.kind == node_kind(StyleKind::Variable) &&
	              edit_of(*add).position == 2,
	      "Add variable goes after the selected line");
	ui.activate(item_id(styles, {"Remove"}));
	requests = ui.drain();
	const EditorRequest *remove = only(requests, EditorRequestKind::EditRecord);
	CHECK(remove && edit_of(*remove).operation == EditOperation::Remove && edit_of(*remove).address.row == fg.id,
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Styles"));
	session.run_operations();
	const SessionView &v = session.view();
	CHECK(editor_test::write_text(v.project.root + "/menu_style.mns",
	                              "// Header\r\nA_FG FFFFFFFF\r\n// Colours below\n\r\nB_FG FF000000\r\n// Footer\r\n"),
	      "stylesheet fixture");
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("menu_style.mns"));
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
	// S13 V3: what each line's value is used as is made once for the document's revision and the
	// graph, kept across frames and a line of Output, and made again for an edit.
	const StylesView *view = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count() && !view; ++i)
		if (auto *window = dynamic_cast<DocumentWindow *>(&ui.windows.pass().window(i)))
			view = dynamic_cast<const StylesView *>(window->view_of(path));
	CHECK(view != nullptr, "the stylesheet's view");
	const size_t made = view ? view->uses_made() : 0;
	ui.frames(3);
	session.handle(request::clear_output());
	ui.frames(2);
	CHECK(view && made > 0 && view->uses_made() == made, "the value uses kept across frames and a line of Output");
	// S13 V8: a value's edit follows its change set: A's use made again, B's kept, the uses not
	// started again; its undo the same.
	{
		const size_t used = view ? view->lines_used() : 0;
		EditorRequest recolour = request::edit_record(path, Edit());
		recolour.edits[0].address = {a, node_kind(StyleKind::Variable), 0};
		recolour.edits[0].field = "value";
		recolour.edits[0].value = std::string("FF00FF00");
		session.handle(recolour);
		ui.frames(2);
		CHECK(view && view->uses_made() == made && view->lines_used() == used + 1,
		      "a value's edit: its line's use made again alone");
		session.handle(request::undo(path));
		ui.frames(2);
		CHECK(view && view->uses_made() == made && view->lines_used() == used + 2 && text() == original,
		      "its undo: the line's use made again alone");
	}
	const std::string frame = logged_frame(ui);
	CHECK(frame.find("Add variable") != std::string::npos && frame.find("Add comment") == std::string::npos &&
	              frame.find("Add blank line") == std::string::npos,
	      "Add variable, no Add comment, no Add blank line");
	// A line selected; a tool pressed, the requests it raised served.
	const NodeKind variable = node_kind(StyleKind::Variable);
	const auto select_line = [&](NodeId row) {
		EditorRequest select = request::select_record(path, {row, variable, 0});
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
		session.handle(request::undo(path));
		ui.frames(2);
	};
	const auto moved_to = [](const std::vector<EditorRequest> &requests) {
		const EditorRequest *move = only(requests, EditorRequestKind::EditRecord);
		return move && edit_of(*move).operation == EditOperation::Move ? edit_of(*move).position : SIZE_MAX;
	};
	select_line(a);
	CHECK(press("Up").empty() && text() == original, "the first listed line has no Up, the header above it");
	select_line(b);
	CHECK(press("Down").empty() && text() == original, "the last listed line has no Down, the footer below it");
	CHECK(moved_to(press("Up")) == 1 &&
	              text() == "// Header\r\nB_FG FF000000\r\nA_FG FFFFFFFF\r\n// Colours below\r\n\r\n// Footer\r\n",
	      "Up: B right above A, past the comment and the blank line, which stay after A");
	CHECK(view && view->uses_made() > made, "an edit: the value uses made again");
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
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code() == "style.line_ending" && d.asset == path && d.row_id == comment) location = problem_location(d, v);
	CHECK(!location.empty() && location.record.row == comment, "the line ending's finding goes to the comment");
	session.handle(request::open_record(location.path, location.record, location.field));
	ui.frames(2);
	ui.drain();
	CHECK(v.documents.selection.primary.row == comment, "the Problems row selects the comment");
	for (const char *tool : {"Duplicate", "Remove", "Up", "Down"}) CHECK(press(tool).empty(), tool);
	CHECK(text() == original, "no row tool acts on a line the table does not list");
	const std::vector<EditorRequest> added = press("Add variable");
	const EditorRequest *add = only(added, EditorRequestKind::EditRecord);
	CHECK(add && edit_of(*add).operation == EditOperation::Add && edit_of(*add).position == 6 && document->rows().size() == 7 &&
	              document->rows()[6]->kind == variable && text().rfind(original, 0) == 0,
	      "Add variable: at the end of the file, after the footer");
}

// Go to and the uses, through the Inspector (S12 D3), over a real session. A stylesheet
// variable's "Referenced by" rows are each a click away: the first opens the menu that names it
// at the record by its locator, the field shown; its lines are made once for the record and kept
// across frames and a line of Output (S13 V3). MAIN's font names a style variable: its Go to
// offers the variable where the game reads it and the .fnt its value names; the first opens the
// stylesheet at the variable, the second shows the font in Files (the editor does not edit
// fonts).
void test_go_to_ui() {
	editor_test::TempProjectDir dir("opennova_editor_ui_go_to");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "GoTo"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	session.handle(request::open_document("menu_style.mns"));
	const Document *style = session.document_for("menu_style.mns");
	NodeAddress large;
	CHECK(style && find_definition(AssetGraph(), *style, "DEF_FONTNAME_LG", large),
			"the stylesheet's large font");
	if (!style || !large.row) return;
	EditorRequest select = request::select_record(style->path(), large);
	session.handle(select);
	const std::vector<const GraphEdge *> users = v.findings.graph->referrers_of(ReferenceKind::StyleVar, "DEF_FONTNAME_LG");
	CHECK(!users.empty() && !users.front()->locator.empty(), "a menu names the large font");
	if (users.empty()) return;
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	const ImGuiID inspector = Ui::window_id("Inspector");
	InspectorWindow *inspector_window = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count() && !inspector_window; ++i)
		inspector_window = dynamic_cast<InspectorWindow *>(&ui.windows.pass().window(i));
	CHECK(inspector_window != nullptr, "the Inspector");
	if (inspector_window) {
		const size_t made = inspector_window->users_made();
		ui.frames(4);
		session.handle(request::clear_output());
		ui.frames(2);
		CHECK(made > 0 && inspector_window->users_made() == made,
		      "Referenced by: made for the record, kept across frames and a line of Output");
	}
	ui.activate(item_id(pushed(inspector, 0), {"###use"}));
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *use = one(requests, EditorRequestKind::OpenDocument);
	CHECK(use && use->path == users.front()->source && use->locator == users.front()->locator &&
	              use->field == users.front()->field,
	      "a use opens its file at the record that makes it");

	// MAIN's font: Go to offers the variable and the font file.
	session.handle(request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress main;
	CHECK(menu && find_definition(AssetGraph(), *menu, "MAIN", main), "the menu's MAIN window");
	if (!menu || !main.row) return;
	select = request::select_record(menu->path(), {});
	select.address = main;
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
	const std::vector<ReferenceTarget> targets =
			reference_targets(*v.findings.graph, *v.project.scan, font, value);
	CHECK(targets.size() == 2 && targets[0].editable && !targets[1].editable, "the variable, then the font file");
	if (targets.size() != 2) return;
	ui.activate(item_id(inspector, {key.c_str(), "fields", "font.name", "Go to"}));
	CHECK(ui.drain().empty(), "two places: Go to opens a menu of them, going nowhere yet");
	const ImGuiID places = item_id(inspector, {key.c_str(), "fields", "font.name", "go to"});
	ui.activate(popup_item(places, targets[0].label.c_str()));
	requests = ui.drain();
	const EditorRequest *variable = one(requests, EditorRequestKind::OpenDocument);
	CHECK(variable && variable->path == style->path() && style->address_at(variable->locator) == large &&
	              variable->field == "name",
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
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Numbers"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const AssetEntry *items_asset = v.project.scan->find("items.def");
	CHECK(items_asset != nullptr, "the project's item table");
	if (!items_asset) return;
	const std::string items_path = items_asset->relative_path;
	CHECK(editor_test::write_text(v.project.root + "/" + items_path,
	                              "begin \"Carrier\"\nid 100164\ntype vehicle\naddeweap ewep01 100166\nend\n"
	                              "begin \"Gun\"\nid 100166\ntype vehicle\nend\n"),
	      "an item naming another by id");
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document(items_path));
	const Document *items = session.document_for(items_path);
	NodeAddress carrier, gun;
	CHECK(items && find_definition(AssetGraph(), *items, "100164", carrier) &&
					find_definition(AssetGraph(), *items, "100166", gun),
			"the two items");
	if (!items || !carrier.row || !gun.row) return;
	NodeAddress attachment;
	for (const Document::Collection &collection : items->collections_of(carrier))
		if (std::string(items->kind_token(collection.spec.kind)) == "attachment" && !collection.ids.empty())
			attachment = {carrier.row, collection.spec.kind, collection.ids.front()};
	CHECK(attachment.child != 0, "the carrier's attachment");
	if (!attachment.child) return;
	EditorRequest select = request::select_record(items_path, attachment);
	session.handle(select);
	std::string form, list;
	for (const InspectorSection &section : plan_inspector(*items, attachment, carrier, "")) {
		for (const FieldUse &field : section.fields)
			if (field.schema->id == "item_id") form = section.key;
		for (const Document::Collection &collection : section.collections)
			if (std::string(items->kind_token(collection.spec.kind)) == "attachment") list = section.key;
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
		return go && go->path == items_path && items->address_at(go->locator) == gun && go->field == "id";
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

namespace {

// The groups of the editor_ui ctest, each a row of its own (tests/CMakeLists.txt names them):
// a failure names its group, and the rows run in parallel.
struct Group {
	const char *name;
	void (*run)();
};
// S13 V8: the menu view's tree of the selected screen follows the menu's change sets: a window of
// another screen moved keeps it, one of the screen moved makes it again.
void test_menu_tree_follows_changes() {
	editor_test::TempProjectDir dir("opennova_editor_ui_menu_tree_changes");
	std::shared_ptr<MnuDocument> document = load_menu(dir);
	Diagnostic error;
	Edit add;
	add.operation = EditOperation::Add;
	add.address = {0, kScreen, 0};
	CHECK(document->apply(add, error) && document->rows().size() == 2, "a second screen added");
	const Node &shown = *document->rows()[0];
	const Node &other = *document->rows()[1];
	const std::vector<NodeId> others = document->collections_of({other.id, kScreen, 0}).front().ids;
	CHECK(!others.empty(), "the second screen holds a window");
	if (others.empty()) return;
	SessionView v = menu_view(document);
	select_in(v, {shown.id, kScreen, 0});
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Document");
	ui.drain();
	const MenuView *view = nullptr;
	for (int i = 0; i < ui.windows.pass().window_count() && !view; ++i)
		if (auto *window = dynamic_cast<DocumentWindow *>(&ui.windows.pass().window(i)))
			view = dynamic_cast<const MenuView *>(window->view_of(document->path()));
	CHECK(view != nullptr && view->trees_made() > 0, "the menu's view, its tree made");
	if (!view) return;
	const size_t made = view->trees_made();
	Edit move;
	move.address = {other.id, kWindow, others.front()};
	move.field = "position.left";
	move.value = int64_t(24);
	CHECK(document->apply(move, error), "the second screen's window moved");
	ui.frames(2);
	CHECK(view->trees_made() == made, "another screen's window moved: the tree kept");
	move.address = named(*document, "TITLE");
	CHECK(document->apply(move, error), "the shown screen's window moved");
	ui.frames(2);
	CHECK(view->trees_made() == made + 1, "the shown screen's window moved: the tree made again");
}

// ADR 0046 S14: several records of kinds whose fields are alike take the Inspector's shared form
// (the pool document's crates and barrels, two kinds over one field table, as a mission's four
// entity pools are): its heading counts them by kind, a field that differs is marked, and a change
// is one batch over every one, each Set naming its record's own kind. A record of another kind among
// them: the primary's own form, under the note that the kinds differ.
void test_inspector_kinds_alike() {
	using editor_test::PoolDocument;
	auto pool = std::make_shared<PoolDocument>();
	Diagnostic error;
	const std::string text = "C alpha 5\nB bravo 3\nC charlie 0\nN delta words\nB echo 7\n";
	CHECK(pool->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "pool.txt", AssetKind::Unknown, "jo", error),
	      "the pool loads");
	const auto row_at = [&](size_t i) { return NodeAddress{pool->rows()[i]->id, pool->rows()[i]->kind, 0}; };
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Pool";
	editor_test::own(v.project.document).title = "Pool";
	editor_test::own(v.project.scan).entries.push_back(file_entry("pool.txt", pool->path(), AssetKind::Unknown));
	editor_test::own(v.project.scan).index();
	v.documents.open.push_back(pool);
	v.documents.active = pool->path();
	v.documents.selection.select(pool->path(), row_at(0), {row_at(1), row_at(4)}, SelectMode::Replace);
	v.revisions.touch(ViewConcern::Selection);
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	std::string shown = logged_frame(ui);
	CHECK(shown.find("3 records selected (1 Crate, 2 Barrel): a change here sets every one of them.") != std::string::npos,
	      "a crate and two barrels: the shared form, counted by kind");
	CHECK(shown.find("of different kinds") == std::string::npos && shown.find("(mixed)") != std::string::npos,
	      "no note that the kinds differ; what differs is marked");
	// The weight typed: one batch, a Set on each record under its own kind.
	const ImGuiID inspector = Ui::window_id("Inspector");
	ImGui::ActivateItemByID(item_id(inspector, {"", "fields", "weight", "##value"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	ui.key(ImGuiMod_Ctrl, true);
	ui.key(ImGuiKey_A, true);
	ui.key(ImGuiKey_A, false);
	ui.key(ImGuiMod_Ctrl, false);
	ImGui::GetIO().AddInputCharactersUTF8("9");
	ui.frames(2);
	ui.key(ImGuiKey_Enter, true);
	ui.key(ImGuiKey_Enter, false);
	ui.frames(2);
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *batch = nullptr;
	for (const EditorRequest &request : requests)
		if (request.kind == EditorRequestKind::EditRecord) batch = &request;
	CHECK(batch && batch->edits.size() == 3 && batch->edits[0].address == row_at(0) &&
	              batch->edits[1].address == row_at(1) && batch->edits[2].address == row_at(4) &&
	              batch->edits[1].address.kind == editor_test::kPoolBarrel && batch->edits[0].field == "weight" &&
	              std::get<int64_t>(batch->edits[2].value) == 9,
	      "a change is one batch over every record, each Set naming its record's own kind");
	ImGui::ClearActiveID();
	ui.frames(2);
	ui.drain();

	// A note among them: the kinds differ, the primary's own form.
	v.documents.selection.select(pool->path(), row_at(0), {row_at(3)}, SelectMode::Replace);
	v.revisions.touch(ViewConcern::Selection);
	ui.frames(3);
	shown = logged_frame(ui);
	CHECK(shown.find("2 records of different kinds selected") != std::string::npos &&
	              shown.find("a change here sets every one") == std::string::npos,
	      "a note with a crate: the kinds differ");
	ui.drain();
}

void run_menu_tests() {
	test_requests_round_trip();
	test_frame_bracket_follows_the_table();
	test_menu_tree_model();
	test_menu_window_ui();
	test_menu_tree_follows_changes();
	test_actions_after_edits();
}
void run_inspector_tests() {
	test_inspector_plan();
	test_inspector_ui();
	test_inspector_kinds_alike();
	test_go_to_ui();
	test_numeric_go_to_ui();
}
void run_preview_tests() {
	test_preview_canvas_smoke();
	test_preview_gestures_end();
	test_preview_several_windows_ui();
}
void run_styles_tests() {
	test_styles_window_ui();
	test_styles_lines_listed();
}
// Output's folded lines (the UX round's problems lane): an import's files under its one line and the
// game's log under its own, a click opening one; the rows the window draws, opened and not; the log's
// fold_into making a line's text and folded lines grow in place, one past its hold refused.
void test_output_folded() {
	OutputLog log;
	log.append("Opened Folded.");
	const uint64_t import = log.append_folded("Imported 3 files (1.0 KB): Menu 3.", {"Imported a.mnu", "Imported b.mnu", "Imported c.mnu"});
	const uint64_t game = log.append_folded("Running: OpenNova on the build.", {"Command line: x"});
	CHECK(log.fold_into(game, "Running: OpenNova on the build. Its log: 2 lines, 1 shown below.", {"banner", "error: x"}),
	      "the game's line grows in place");
	log.append("game: error: x");
	CHECK(log.folded_at(game).size() == 3 && log.at(game).find("2 lines") != std::string::npos, "its text and lines");
	CHECK(OutputWindow::rows(log, {}).size() == 4, "folded: one row a line");
	const std::vector<std::pair<uint64_t, int64_t>> open = OutputWindow::rows(log, {import});
	CHECK(open.size() == 7 && open[2] == std::make_pair(import, int64_t(0)) && open[4] == std::make_pair(import, int64_t(2)),
	      "an import opened: its files under it");
	CHECK(game_line_matters("USER ERROR: x") && game_line_matters("boot-required resource missing: main.mnu") &&
	              !game_line_matters("PFF LOADED FILE: _ffp.fx") && !game_line_matters("Godot Engine v4.6.1"),
	      "what of the game's log shows: errors, warnings, refusals and missing files");
	log.clear();
	CHECK(!log.fold_into(game, "gone", {"x"}), "a line the log no longer holds takes nothing");

	// Drawn: the folded line with its count, a click opening it.
	SessionView v;
	v.project.open = true;
	v.project.root = "C:/mods/Folded";
	v.activity.output.append("Opened Folded.");
	v.activity.output.append_folded("Imported 2 files (1.0 KB): Menu 2.", {"Imported a.mnu", "Imported b.mnu"});
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Output");
	ui.away();
	std::string text = logged_frame(ui);
	CHECK(text.find("Imported 2 files (1.0 KB): Menu 2.  (2 lines)") != std::string::npos && text.find("Imported a.mnu") == std::string::npos,
	      "the import's one line, its files folded");
	CHECK(ui.windows.pending_requests() == 0, "drawing raises nothing");
	// Its rows made again only when the log or the lines opened move, not every frame (the review's L10).
	const auto *output = dynamic_cast<const OutputWindow *>(find_window(ui.windows.pass(), "Output"));
	CHECK(output != nullptr, "the Output window");
	if (!output) return;
	const size_t made = output->rows_made();
	ui.frames(5);
	CHECK(output->rows_made() == made, "five frames of the same log: the rows kept");
	v.activity.output.append("Saved a.mnu.");
	ui.frames(2);
	CHECK(output->rows_made() == made + 1, "a line more: made again, once");
}

void run_problems_tests() {
	test_ui_kit();
	test_output_folded();
	test_problems_window_ui();
	test_problems_confirmation_follows();
	test_problems_narrow();
	test_problems_rewrite_hidden();
	test_problems_many();
	test_problems_original();
}
constexpr Group kGroups[] = {
	{"workspace", run_workspace_tests},   {"markers", run_marker_tests},
	{"bounds", run_bounds_tests},         {"field_widgets", run_field_widget_tests},
	{"reference_picker", run_reference_picker_tests}, {"find", run_find_tests},
	{"rename", run_rename_tests},         {"menu", run_menu_tests},
	{"inspector", run_inspector_tests},   {"preview", run_preview_tests},
	{"styles", run_styles_tests},         {"problems", run_problems_tests},
	{"gate", run_gate_tests},             {"logic", run_logic_tests},
};

} // namespace

// `editor_ui_test <group>` runs one group; `--groups <names...>` checks the names are exactly
// the groups (the ctest list and this table stay one list); no argument runs every group.
int main(int argc, char **argv) {
	if (argc >= 2 && std::strcmp(argv[1], "--groups") == 0) {
		std::vector<std::string> named(argv + 2, argv + argc), known;
		for (const Group &group : kGroups) known.push_back(group.name);
		std::sort(named.begin(), named.end());
		std::sort(known.begin(), known.end());
		if (named == known) return 0;
		std::printf("FAIL: the ctest rows and the groups differ\n");
		return 1;
	}
	bool ran = false;
	for (const Group &group : kGroups) {
		if (argc >= 2 && std::strcmp(argv[1], group.name) != 0) continue;
		group.run();
		ran = true;
	}
	if (!ran) {
		std::printf("FAIL: no group named %s\n", argv[1]);
		return 1;
	}
	if (editor_ui_test::g_failures == 0)
		std::printf("editor_ui %s: all tests passed\n", argc >= 2 ? argv[1] : "(every group)");
	return editor_ui_test::g_failures == 0 ? 0 : 1;
}
