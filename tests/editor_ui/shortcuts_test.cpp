// DI-18 (ADR 0046, "Shortcuts and context jumps"): the common jumps a keystroke or a right-click away, over a
// real session holding the fixture's item catalog, the minted mission and the synth models it draws. Go to file
// (Ctrl+P) and Go to name (Ctrl+T) open the project's finder in that scope, typing narrows it, Enter goes to the
// first; the Go menu lists every jump with its keys; Go to definition (F12) and Find usages (Shift+F12) act on a
// reference field under the pointer (the Inspector's: a Ctrl+click on its value goes too, its right-click menu
// offers the three jumps), on a Files row, else on the selection (a mission's entity: its item; a record: who
// names it); the right-click menus of Files, Problems, the outline (an item record's Place in mission) and the
// mission's picture (Go to item, Go to model, Show model in Files, Select all like this, Place another); every
// jump an OpenDocument, a step of the navigation history.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_viewport.h>
#include <editor/session/request_factories.h>
#include <editor/ui/inspector_layout.h>

#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

using FindScope = WorkspaceView::FindScope;

constexpr const char *kMission = "missions/synth_logic.bms";

// The fixture's item catalog, the minted mission and its table, and the synth models the mission's items draw.
struct JumpsProject {
	editor_test::TempProjectDir dir{"opennova_editor_ui_shortcuts"};
	editor_test::FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string items;

	bool open() {
		session.handle(request::new_project(dir.file("project"), "Jumps"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &v = session.view();
		const AssetEntry *catalog = v.project.scan ? v.project.scan->find("items.def") : nullptr;
		if (!catalog) return false;
		items = catalog->relative_path;
		const std::string repo = test_paths_repo_root(__FILE__);
		if (!editor_test::write_bytes(v.project.root + "/" + items, test_io::read_file(repo + "/fixtures/def/items.def")) ||
		    !editor_test::write_bytes(v.project.root + "/" + kMission, test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")) ||
		    !editor_test::write_bytes(v.project.root + "/missions/synth_logic.bin",
		                              test_io::read_file(repo + "/fixtures/bms/synth_logic.bin")))
			return false;
		for (const char *model : {"pump", "armory", "shed"})
			if (!editor_test::write_bytes(v.project.root + "/models/" + model + ".3di",
			                              test_io::read_file(repo + "/fixtures/threedi/synth/" + model + ".3di")))
				return false;
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document(kMission));
		session.handle(request::open_document(items));
		return session.document_for(kMission) && session.document_for(items);
	}
	// The record defining `name` (an item's id) in the document at `path`, and its locator.
	NodeAddress record(const std::string &path, const char *name) {
		const Document *document = session.document_for(path);
		NodeAddress out;
		if (document) find_definition(AssetGraph(), *document, name, out);
		return out;
	}
	std::string locator(const std::string &path, const NodeAddress &address) {
		const Document *document = session.document_for(path);
		return document ? document->locator(address) : std::string();
	}
	// The mission's entity placing `item` (the first in the file).
	NodeAddress entity_of(int64_t item) {
		const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for(kMission));
		if (!mission) return NodeAddress();
		for (const MissionKind kind : {MissionKind::Item, MissionKind::Building, MissionKind::Organic, MissionKind::Marker})
			for (const Node *row : mission->rows_of(kind)) {
				Value value;
				const NodeAddress address{row->id, row->kind, 0};
				if (mission->get(address, "item", value) && std::get_if<int64_t>(&value) && std::get<int64_t>(value) == item)
					return address;
			}
		return NodeAddress();
	}
	// A request served as the Shell serves it, and the frames after it.
	void serve(Ui &ui, const EditorRequest &request) {
		platform.clock += 10000;
		session.handle(request);
		session.run_operations();
		ui.frames(2);
	}
};

// The workspace over the project, the windows' set_workspace requests served through the session, its polls
// pumped.
void attach(Ui &ui, JumpsProject &project) {
	ui.session = &project.session;
	ui.pump = [&project] { project.session.poll(); };
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.away();
	ui.drain();
}

void press(Ui &ui, ImGuiKey key) {
	ui.key(key, true);
	ui.key(key, false);
}

// The project's finder while it shows (its id "###project_find" whatever its scope's title), null otherwise.
const ImGuiWindow *finder() {
	if (GImGui->OpenPopupStack.Size != 1 || !GImGui->OpenPopupStack[0].Window) return nullptr;
	const ImGuiWindow *window = GImGui->OpenPopupStack[0].Window;
	const std::string name = window->Name;
	return name.size() >= 15 && name.compare(name.size() - 15, 15, "###project_find") == 0 ? window : nullptr;
}

// The one OpenDocument among `requests` going to `path` (at `locator` when one is given).
const EditorRequest *goes(const std::vector<EditorRequest> &requests, const std::string &path,
                          const std::string &locator = std::string()) {
	const EditorRequest *open = only(requests, EditorRequestKind::OpenDocument);
	return open && open->path == path && (locator.empty() || open->locator == locator) ? open : nullptr;
}

// The Inspector's section holding `field` of the selected record of `document`.
std::string section_of(const SessionView &v, const Document &document, const char *field) {
	for (const InspectorSection &section :
	     plan_inspector(document, v.documents.selection.primary, v.documents.selection.primary, ""))
		for (const FieldUse &use : section.fields)
			if (use.schema->id == field) return section.key;
	return std::string();
}

// Where an item of the Inspector's form is on the screen: given the keyboard (the window keeps the rect of the
// item it focuses), then let go (a list its activation opened closed again).
ImVec2 centre_of(Ui &ui, ImGuiID id) {
	ui.activate(id);
	ImGuiWindow *window = ImGui::FindWindowByName("Inspector");
	const ImRect rect = window ? ImGui::WindowRectRelToAbs(window, window->NavRectRel[0]) : ImRect();
	ImGui::ClearActiveID();
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
	ui.drain();
	return rect.GetCenter();
}

// A right click at `at`: the frame after it, its text.
std::string right_click(Ui &ui, ImVec2 at) {
	ui.mouse(at.x, at.y);
	ui.button(true, ImGuiMouseButton_Right);
	ui.button(false, ImGuiMouseButton_Right);
	return logged_frame(ui);
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

// Go to file (Ctrl+P) and Go to name (Ctrl+T): the finder opened in that scope, its text empty, a name typed
// narrowing it to the files (or the names) alone, ranked, Enter going to the first (an OpenDocument, the modal
// closed); Ctrl+T over Go to file switches its scope, the text starting afresh; Find in project (Ctrl+Shift+F)
// lists both. The Go menu: each jump with its keys, Go to name... opening the finder in its scope.
void test_go_to_file_and_name() {
	JumpsProject project;
	CHECK(project.open(), "the jumps project");
	if (project.items.empty()) return;
	Ui ui;
	attach(ui, project);
	const SessionView &v = project.session.view();
	const WorkspaceView::ProjectFind &find = v.workspace.project_find;
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_P});
	ui.frames(2);
	CHECK(find.open && find.scope == FindScope::Files && find.text.empty() && finder() != nullptr, "Ctrl+P: Go to file");
	CHECK(logged_frame(ui).find("Go to file") != std::string::npos, "its title says its scope");
	ImGui::GetIO().AddInputCharactersUTF8("pump");
	ui.frames(3);
	std::string text = logged_frame(ui);
	CHECK(text.find("pump.3di") != std::string::npos && text.find("(item, ") == std::string::npos,
	      "the files alone: pump.3di, no item naming it");
	press(ui, ImGuiKey_Enter);
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(goes(requests, "models/pump.3di") != nullptr, "Enter goes to the file first listed");
	ui.frames(2);
	CHECK(!find.open && finder() == nullptr, "going closes the finder");
	if (const EditorRequest *open = goes(requests, "models/pump.3di")) project.serve(ui, *open);
	CHECK(v.documents.active == "models/pump.3di" && !v.navigation.back.empty(), "the model open, a step of the history");

	// Ctrl+T: the names alone, an item by its catalog's name.
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_T});
	ui.frames(2);
	CHECK(find.open && find.scope == FindScope::Names && find.text.empty(), "Ctrl+T: Go to name, its text empty");
	ImGui::GetIO().AddInputCharactersUTF8("rifleman");
	ui.frames(3);
	text = logged_frame(ui);
	CHECK(text.find("Wire Test Rifleman") != std::string::npos && text.find("Go to name") != std::string::npos,
	      "the rifleman by its catalog's name");
	// Ctrl+P while it shows: Go to file in its place, its text afresh.
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_P});
	ui.frames(2);
	CHECK(find.open && find.scope == FindScope::Files && find.text.empty(), "Ctrl+P over Go to name: Go to file, afresh");
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_T});
	ui.frames(2);
	ImGui::GetIO().AddInputCharactersUTF8("rifleman");
	ui.frames(3);
	press(ui, ImGuiKey_Enter);
	requests = ui.drain();
	const NodeAddress rifleman = project.record(project.items, "106102");
	CHECK(goes(requests, project.items, project.locator(project.items, rifleman)) != nullptr,
	      "Enter goes to the record defining it");
	ui.frames(2);
	// Down moves the mark: Go to name over several, Enter on the second.
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_T});
	ui.frames(2);
	ImGui::GetIO().AddInputCharactersUTF8("Wire Test");
	ui.frames(3);
	press(ui, ImGuiKey_DownArrow);
	press(ui, ImGuiKey_Enter);
	requests = ui.drain();
	const EditorRequest *second = only(requests, EditorRequestKind::OpenDocument);
	CHECK(second && second->path == project.items && !second->locator.empty(), "Down, Enter: the second name's record");
	ui.frames(2);

	// The Go menu: each jump with its keys; Go to name... opens the finder in its scope.
	ui.activate(item_id(menu_bar_id(), {"Go"}));
	text = logged_frame(ui);
	CHECK(in_order(text, {"Back", "Alt+Left", "Forward", "Alt+Right", "Go to file...", "Ctrl+P", "Go to name...", "Ctrl+T",
	                      "Find in project...", "Ctrl+Shift+F", "Go to definition", "F12", "Find usages", "Shift+F12"}),
	      "the Go menu: every jump with its keys");
	ui.activate(item_id(ImHashStr("##Menu_00"), {"Go to name..."}));
	ui.frames(2);
	CHECK(find.open && find.scope == FindScope::Names, "Go > Go to name... opens it in its scope");
	press(ui, ImGuiKey_Escape);
	ui.frames(2);
	CHECK(!find.open, "Escape closes it");
	// Find in project: both.
	ui.chord({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_F});
	ui.frames(2);
	ImGui::GetIO().AddInputCharactersUTF8("pump");
	ui.frames(3);
	text = logged_frame(ui);
	CHECK(find.scope == FindScope::All && text.find("Find in project") != std::string::npos && text.find("pump.3di") != std::string::npos &&
	              text.find("Wire Test Pump") != std::string::npos,
	      "Ctrl+Shift+F: the files and the names");
	press(ui, ImGuiKey_Escape);
	ui.frames(2);
}

// F12 and Shift+F12 over the selection: a mission's entity selected, F12 goes to its item's record (what it
// names first); Shift+F12 over the item there lists its uses (the mission's entities placing it), Down and Enter
// going to one; Back returns.
void test_jumps_from_the_selection() {
	JumpsProject project;
	CHECK(project.open(), "the jumps project");
	if (project.items.empty()) return;
	Ui ui;
	attach(ui, project);
	const SessionView &v = project.session.view();
	const NodeAddress walker = project.entity_of(106102);
	CHECK(walker.row != 0, "the walker placing the rifleman");
	project.serve(ui, request::open_document(kMission));
	project.serve(ui, request::select_record(kMission, walker));
	ui.away();
	ui.drain();
	CHECK(ui.windows.jump_subject().definition.size() == 1, "the walker's subject: its item");
	std::vector<EditorRequest> requests = ui.chord({ImGuiKey_F12});
	const NodeAddress rifleman = project.record(project.items, "106102");
	const EditorRequest *open = goes(requests, project.items, project.locator(project.items, rifleman));
	CHECK(open != nullptr, "F12: the walker's item, its record");
	if (open) project.serve(ui, *open);
	CHECK(v.documents.active == project.items && v.documents.selection.primary == rifleman, "the item selected there");
	// Shift+F12 over the pump's record: the three pumps of the mission.
	const NodeAddress pump = project.record(project.items, "106100");
	project.serve(ui, request::select_record(project.items, pump));
	ui.away();
	ui.chord({ImGuiMod_Shift, ImGuiKey_F12});
	ui.frames(3);
	const WorkspaceView::ProjectFind &find = v.workspace.project_find;
	CHECK(find.open && find.scope == FindScope::Usages && find.path == project.items &&
	              find.locator == project.locator(project.items, pump),
	      "Shift+F12: Find usages of the pump's record");
	std::string text = logged_frame(ui);
	CHECK(text.find("Find usages") != std::string::npos && text.find("3 uses") != std::string::npos &&
	              text.find("synth_logic.bms") != std::string::npos,
	      "the three pumps the mission places");
	// The text filters them.
	ImGui::GetIO().AddInputCharactersUTF8("zzz");
	ui.frames(3);
	CHECK(logged_frame(ui).find("0 of 3 uses") != std::string::npos, "a filter none holds");
	press(ui, ImGuiKey_Backspace);
	press(ui, ImGuiKey_Backspace);
	press(ui, ImGuiKey_Backspace);
	ui.frames(2);
	press(ui, ImGuiKey_DownArrow);
	press(ui, ImGuiKey_Enter);
	requests = ui.drain();
	open = goes(requests, kMission);
	CHECK(open && !open->locator.empty() && open->field == "item", "Down, Enter: the second pump, at its item");
	ui.frames(2);
	CHECK(!find.open, "going closes it");
	if (open) project.serve(ui, *open);
	CHECK(v.documents.active == kMission, "the mission, at the pump");
	project.serve(ui, request::navigate_back());
	CHECK(v.documents.active == project.items, "Back: the catalog again");
}

// The Inspector's reference fields: a text reference's value (an item's graphic) under the pointer, F12 goes to
// the model it names; Ctrl held over it a click goes there too, the text box left alone; its right-click menu
// offers Go to definition, Find usages and Show in Files; a number picked by name (the walker's item) the same,
// its list left shut by a Ctrl+click.
void test_reference_field_jumps() {
	JumpsProject project;
	CHECK(project.open(), "the jumps project");
	if (project.items.empty()) return;
	Ui ui;
	attach(ui, project);
	const SessionView &v = project.session.view();
	const NodeAddress pump = project.record(project.items, "106100");
	project.serve(ui, request::open_document(project.items));
	project.serve(ui, request::select_record(project.items, pump));
	ui.focus("Inspector");
	const Document *items = project.session.document_for(project.items);
	if (!items) return;
	const ImGuiID inspector = Ui::window_id("Inspector");
	const std::string section = section_of(v, *items, "graphic");
	const ImGuiID graphic = item_id(inspector, {section.c_str(), "fields", "graphic", "##value"});
	const ImVec2 at = centre_of(ui, graphic);
	CHECK(at.x > 0.0f, "the graphic's value on the screen");
	ui.mouse(at.x, at.y);
	ui.frames(1);
	std::vector<EditorRequest> requests = ui.chord({ImGuiKey_F12});
	CHECK(goes(requests, "models/pump.3di") != nullptr, "F12 over the graphic: the model it names");
	// Ctrl+click: the same Go to; the text box does not take the click.
	ui.key(ImGuiMod_Ctrl, true);
	ui.mouse(at.x, at.y);
	ui.frames(2);
	ui.button(true);
	ui.button(false);
	ui.key(ImGuiMod_Ctrl, false);
	requests = ui.drain();
	CHECK(goes(requests, "models/pump.3di") != nullptr && only(requests, EditorRequestKind::EditRecord) == nullptr,
	      "Ctrl+click on the value: the model");
	CHECK(GImGui->ActiveId != graphic, "the text box left alone");
	// The right-click menu.
	std::string text = right_click(ui, at);
	CHECK(in_order(text, {"Go to definition", "F12", "Find usages", "Shift+F12", "Show in Files"}),
	      "the reference's menu: its jumps with their keys");
	const ImGuiID menu = item_id(inspector, {section.c_str(), "fields", "graphic", "reference jumps"});
	ui.activate(popup_item(menu, "Find usages"));
	ui.frames(2);
	const WorkspaceView::ProjectFind &find = v.workspace.project_find;
	CHECK(find.open && find.scope == FindScope::Usages && find.path == "models/pump.3di" && find.locator.empty(),
	      "Find usages: who names the model");
	text = logged_frame(ui);
	CHECK(text.find("pump.3di") != std::string::npos && text.find("Wire Test Pump") != std::string::npos,
	      "the items drawing it");
	press(ui, ImGuiKey_Escape);
	ui.frames(2);
	right_click(ui, at);
	ui.activate(popup_item(menu, "Show in Files"));
	requests = ui.drain();
	const EditorRequest *shown = only(requests, EditorRequestKind::ShowInFiles);
	CHECK(shown && shown->path == "models/pump.3di", "Show in Files: the model");
	ui.away();

	// A number picked by name: the walker's item, a Ctrl+click going to its record, its list left shut.
	const NodeAddress walker = project.entity_of(106102);
	project.serve(ui, request::open_document(kMission));
	project.serve(ui, request::select_record(kMission, walker));
	ui.focus("Inspector");
	const Document *mission = project.session.document_for(kMission);
	if (!mission) return;
	const std::string entity_section = section_of(v, *mission, "item");
	const ImVec2 item = centre_of(ui, item_id(inspector, {entity_section.c_str(), "fields", "item", "##value"}));
	ui.key(ImGuiMod_Ctrl, true);
	ui.mouse(item.x, item.y);
	ui.frames(2);
	ui.button(true);
	ui.button(false);
	ui.key(ImGuiMod_Ctrl, false);
	requests = ui.drain();
	const NodeAddress rifleman = project.record(project.items, "106102");
	CHECK(goes(requests, project.items, project.locator(project.items, rifleman)) != nullptr,
	      "Ctrl+click on the walker's item: the rifleman's record");
	CHECK(GImGui->OpenPopupStack.Size == 0, "its list left shut");
	ui.away();
}

// The right-click menus: a Files row's Find usages (and Shift+F12 over the row); a Problems row's Go to, Show in
// Files, Find usages and its fixes; an outline line's Go to definition, Find usages and, over an item record, Place
// in mission, which arms the mission's Place tool with the item (the definition viewport's place_in_mission, one
// EditInViewport); the Inspector's Place in mission over the item likewise.
void test_context_menus() {
	JumpsProject project;
	CHECK(project.open(), "the jumps project");
	if (project.items.empty()) return;
	Ui ui;
	attach(ui, project);
	const SessionView &v = project.session.view();
	const WorkspaceView::ProjectFind &find = v.workspace.project_find;

	// Files: the pump model's row.
	ui.focus("Files");
	const ImGuiID table = item_id(Ui::window_id("Files"), {"project_files"});
	const ImGuiWindow *files = ImGui::FindWindowByName("Files");
	CHECK(files != nullptr, "the Files window");
	if (!files) return;
	ImVec2 row;
	CHECK(hover_find(ui, item_id(table, {"models", "models/pump.3di", "##row"}), files->Pos.x + files->Size.x * 0.3f, files->Pos.y,
	                 files->Pos.y + files->Size.y, row),
	      "pump.3di's row");
	std::vector<EditorRequest> requests = ui.chord({ImGuiMod_Shift, ImGuiKey_F12});
	ui.frames(2);
	CHECK(find.open && find.scope == FindScope::Usages && find.path == "models/pump.3di", "Shift+F12 over the row: who names it");
	press(ui, ImGuiKey_Escape);
	ui.frames(2);
	std::string text = right_click(ui, row);
	CHECK(in_order(text, {"Open", "Rename...", "About this file...", "Find usages", "Shift+F12", "Show in folder"}),
	      "the file's menu: Find usages with its keys");
	ui.activate(popup_item(item_id(table, {"models", "models/pump.3di", "file_menu"}), "Find usages"));
	ui.frames(2);
	CHECK(find.open && find.scope == FindScope::Usages && find.path == "models/pump.3di", "Find usages from the file's menu");
	press(ui, ImGuiKey_Escape);
	ui.frames(2);
	ui.away();

	// Problems: its rows ungrouped, the first one's menu.
	project.serve(ui, request::set_workspace(R"({"problems": {"group": "none"}})"));
	ui.focus("Problems");
	const ImGuiTable *problems = ImGui::TableFindByID(item_id(Ui::window_id("Problems"), {"problems"}));
	CHECK(problems != nullptr && !v.findings.diagnostics.empty(), "Problems lists the project's findings");
	if (!problems) return;
	const float line = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
	const float top = problems->InnerWindow->Pos.y - problems->InnerWindow->Scroll.y;
	const ImGuiTableColumn &column = problems->Columns[1];
	text = right_click(ui, ImVec2((column.MinX + column.MaxX) * 0.5f, top + line * 0.5f));
	CHECK(in_order(text, {"Go to", "Show in Files", "Find usages", "Shift+F12"}), "a finding's menu: its jumps");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
	ui.away();
	ui.drain();

	// The outline: the pump's line (the filter keeping it alone), its menu, Place in mission.
	project.serve(ui, request::open_document(kMission));
	project.serve(ui, request::open_document(project.items));
	project.serve(ui, request::set_workspace(R"({"document": {"path": ")" + project.items + R"(", "filter": "Wire Test Pump"}})"));
	ui.focus("Document");
	const ImGuiWindow *document = ImGui::FindWindowByName("Document");
	CHECK(document != nullptr, "the Document window");
	if (!document) return;
	const NodeAddress pump = project.record(project.items, "106100");
	// Below the tab bar (a right click on another document's tab would show it): the first line whose right click
	// opens the item's menu.
	bool opened = false;
	for (float y = document->Pos.y + 70.0f; y < document->Pos.y + document->Size.y && !opened; y += 6.0f) {
		ui.mouse(document->Pos.x + 60.0f, y);
		if (GImGui->HoveredId == 0) continue;
		text = right_click(ui, ImVec2(document->Pos.x + 60.0f, y));
		opened = text.find("Place in mission") != std::string::npos && GImGui->OpenPopupStack.Size == 1;
		if (!opened) {
			ImGui::ClosePopupsExceptModals();
			ui.frames(1);
		}
	}
	CHECK(opened, "the pump's line opens its menu");
	CHECK(in_order(text, {"Go to definition", "F12", "Find usages", "Shift+F12", "Place in mission"}),
	      "the line's menu: its jumps, then the item's Place in mission");
	requests = ui.drain();
	const EditorRequest *selected = only(requests, EditorRequestKind::SelectRecord);
	CHECK(selected && selected->path == project.items && selected->address == pump, "the right click selects the line's record");
	const ImGuiWindow *popup = GImGui->OpenPopupStack.Size ? GImGui->OpenPopupStack.back().Window : nullptr;
	CHECK(popup != nullptr, "the menu open");
	if (!popup) return;
	ui.activate(item_id(popup->ID, {"Place in mission"}));
	requests = ui.drain();
	const EditorRequest *place = only(requests, EditorRequestKind::EditInViewport);
	CHECK(place && place->path == project.items && place->command.name == "place_in_mission" &&
	              place->command.kind == ViewportKind::Definition && place->command.ids == std::vector<NodeId>{pump.row},
	      "Place in mission: the definition viewport's place_in_mission over the row");
	if (place) project.serve(ui, *place);
	const auto *viewport = static_cast<const MissionViewport *>(project.session.viewports().find(kMission, ViewportKind::Mission));
	CHECK(v.documents.active == kMission && viewport && viewport->options().tool == MissionTool::Place &&
	              viewport->options().item == 106100,
	      "the mission active, its Place tool armed with the pump");
	// The Inspector's Place in mission over the item likewise.
	project.serve(ui, request::open_document(project.items));
	project.serve(ui, request::select_record(project.items, pump));
	ui.focus("Inspector");
	CHECK(logged_frame(ui).find("Place in mission") != std::string::npos, "the Inspector heads the item with Place in mission");
	ui.away();
}

// The mission's picture: a right click on a pump's mark opens its menu with the entity's jumps (Go to item with
// its key, Go to model, Show model in Files, Place another, Select all like this, Find usages with its key); Go to
// item opens the catalog at the pump's record, Go to model the model, Show model in Files selects it there, Place
// another arms the Place tool with the pump; Find usages opens the finder on the entity; Go to in outline is a Go
// to of the mission itself at the entity.
void test_mission_entity_menu() {
	JumpsProject project;
	CHECK(project.open(), "the jumps project");
	if (project.items.empty()) return;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	DrawnDevices devices;
	session.viewports().set_devices(&devices.cache);
	Ui ui;
	ImGui::GetIO().DisplaySize = ImVec2(4096.0f, 1600.0f);
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	// The Shell's pump: each request the windows raise served, the devices following the viewports.
	std::vector<EditorRequest> raised;
	const auto settle = [&] {
		for (int i = 0; i < 4; ++i) {
			for (const EditorRequest &request : ui.drain()) {
				raised.push_back(request);
				project.platform.clock += 10000;
				session.handle(request);
			}
			session.run_operations();
			devices.sync(session.viewports(), v);
			ui.frames(1);
		}
	};
	const auto take = [&] {
		std::vector<EditorRequest> out;
		out.swap(raised);
		return out;
	};
	session.handle(request::open_document(kMission));
	settle();
	ui.focus("Document");
	settle();
	take();
	const auto *viewport = static_cast<const MissionViewport *>(session.viewports().find(kMission, ViewportKind::Mission));
	DrawnDevice *device = devices.held(kMission, ViewportKind::Mission);
	CHECK(viewport && device && device->width > 0, "the mission drawn through its device");
	if (!viewport || !device) return;
	std::vector<MissionMark> marks = viewport->marks(device->width, device->height, device);
	int pump = -1;
	for (size_t i = 0; i < marks.size() && pump < 0; ++i)
		if (marks[i].shown && marks[i].entity >= 0 && viewport->scene().entities()[size_t(marks[i].entity)].item == 106100 &&
		    pick_mission_mark(marks, viewport->camera(), device->width, device->height, marks[i].x, marks[i].y, nullptr,
		                      MissionPick::Press) == int(i))
			pump = int(i);
	CHECK(pump >= 0, "a pump's mark on the picture");
	if (pump < 0) return;
	const ImVec2 at(device->origin.x + marks[size_t(pump)].x, device->origin.y + marks[size_t(pump)].y);
	const auto column = [] {
		const ImGuiWindow *found = nullptr;
		for (const ImGuiWindow *window : GImGui->Windows)
			if (window->Active && !window->Hidden && std::strstr(window->Name, "/viewport_column_")) found = window;
		return found;
	};
	// Its menu, opened on the pump's mark, an item of it chosen: the requests it raised, served.
	const auto choose = [&](const char *item) {
		ui.mouse(at.x, at.y);
		ui.button(true, ImGuiMouseButton_Right);
		ui.button(false, ImGuiMouseButton_Right);
		settle();
		take();
		const ImGuiWindow *in = column();
		if (!in) return std::vector<EditorRequest>();
		ui.activate(popup_item(item_id(in->ID, {"mission_canvas_menu"}), item));
		settle();
		ui.away();
		return take();
	};
	ui.mouse(at.x, at.y);
	ui.button(true, ImGuiMouseButton_Right);
	ui.button(false, ImGuiMouseButton_Right);
	settle();
	const std::string text = logged_frame(ui);
	CHECK(in_order(text, {"Go to item", "F12", "Go to model", "Show model in Files", "Place another", "Select all like this",
	                      "Go to in outline", "Find usages", "Shift+F12", "Show events using this"}),
	      "the entity's menu: its jumps with their keys");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
	take();
	const NodeAddress pump_record = project.record(project.items, "106100");
	std::vector<EditorRequest> requests = choose("Go to item");
	CHECK(goes(requests, project.items, project.locator(project.items, pump_record)) != nullptr && v.documents.active == project.items,
	      "Go to item: the catalog at the pump's record");
	session.handle(request::navigate_back());
	settle();
	CHECK(v.documents.active == kMission, "Back: the mission");
	requests = choose("Go to model");
	CHECK(goes(requests, "models/pump.3di") != nullptr, "Go to model: the model the pump draws");
	session.handle(request::navigate_back());
	settle();
	requests = choose("Show model in Files");
	const EditorRequest *shown = only(requests, EditorRequestKind::ShowInFiles);
	CHECK(shown && shown->path == "models/pump.3di", "Show model in Files");
	if (v.documents.active != kMission) {
		session.handle(request::open_document(kMission));
		settle();
	}
	ui.focus("Document");
	settle();
	requests = choose("Place another");
	CHECK(only(requests, EditorRequestKind::SetViewport) != nullptr && viewport->options().tool == MissionTool::Place &&
	              viewport->options().item == 106100,
	      "Place another: the Place tool with the pump");
	session.handle(request::set_viewport(kMission, R"({"kind": "mission", "options": {"tool": "select"}})"));
	settle();
	requests = choose("Find usages");
	const WorkspaceView::ProjectFind &find = v.workspace.project_find;
	CHECK(find.open && find.scope == FindScope::Usages && find.path == kMission && !find.locator.empty(),
	      "Find usages: who names the entity");
	session.handle(request::set_workspace(R"({"project_find": {"open": false}})"));
	settle();
	requests = choose("Go to in outline");
	const EditorRequest *outline = goes(requests, kMission);
	CHECK(outline && !outline->locator.empty() && outline->field == "item", "Go to in outline: a Go to of the entity, at its item");
	ui.away();
}

// The keys keep out of each other's way: F12 and Shift+F12 do nothing while a dialog that takes the whole editor
// shows; Ctrl+P typed into Files' filter opens Go to file, the filter keeping what it had; F5 and Alt+F5 still
// Play and Play from here (DI-26).
void test_keys_apart() {
	JumpsProject project;
	CHECK(project.open(), "the jumps project");
	if (project.items.empty()) return;
	Ui ui;
	attach(ui, project);
	const SessionView &v = project.session.view();
	const NodeAddress walker = project.entity_of(106102);
	project.serve(ui, request::open_document(kMission));
	project.serve(ui, request::select_record(kMission, walker));
	ui.away();
	project.serve(ui, request::set_workspace(R"({"new_project": {"open": true}})"));
	CHECK(ui.chord({ImGuiKey_F12}).empty() && !v.workspace.project_find.open, "a dialog shows: no F12");
	ui.chord({ImGuiMod_Shift, ImGuiKey_F12});
	CHECK(!v.workspace.project_find.open, "nor Shift+F12");
	project.serve(ui, request::set_workspace(R"({"new_project": {"open": false}})"));
	ui.drain();
	// Ctrl+P while Files' filter has the keyboard.
	ui.focus("Files");
	const ImGuiID files = Ui::window_id("Files");
	ImGui::ActivateItemByID(item_id(files, {"##filter"}));
	GImGui->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	ui.frames(2);
	ImGui::GetIO().AddInputCharactersUTF8("mo");
	ui.frames(2);
	ui.chord({ImGuiMod_Ctrl, ImGuiKey_P});
	ui.frames(2);
	CHECK(v.workspace.project_find.open && v.workspace.project_find.scope == FindScope::Files, "Ctrl+P from Files' filter");
	CHECK(v.workspace.files.filter == "mo", "the filter keeps its text");
	press(ui, ImGuiKey_Escape);
	ui.frames(2);
	// F5: Play; Alt+F5: Play from here, the mission active.
	project.serve(ui, request::open_document(kMission));
	ui.away();
	ui.drain();
	std::vector<EditorRequest> requests = ui.chord({ImGuiKey_F5});
	CHECK(only(requests, EditorRequestKind::Play) != nullptr, "F5: Play");
	requests = ui.chord({ImGuiMod_Alt, ImGuiKey_F5});
	CHECK(only(requests, EditorRequestKind::EditInViewport) != nullptr &&
	              only(requests, EditorRequestKind::EditInViewport)->command.name == "play_from_here",
	      "Alt+F5: Play from here");
}

} // namespace

void run_shortcuts_tests() {
	test_go_to_file_and_name();
	test_jumps_from_the_selection();
	test_reference_field_jumps();
	test_context_menus();
	test_mission_entity_menu();
	test_keys_apart();
}

} // namespace editor_ui_test
