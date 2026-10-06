// S12 D7 (ADR 0046 S12): the reference picker (editor/ui/reference_picker) in the Inspector over
// a real session's item table. Each field's popup keeps its own filter: typed into one field's
// picker, another's (another field, or the same field of another record) still lists every name,
// and the first keeps what was typed when it opens again, Escape keeping it too; the arrows move
// through the list, Enter picks, a pick is the field's Set. A Files row
// dropped on a reference's value sets the file there when the field's kind loads it, and
// nothing when it does not. A missing value's picker offers the fixes Problems offers for it. S18: a texture
// field's picture and its picker's; a texture an import makes shows how it is made in its tab, and under a
// model row's use what the texture costs the game; the texture's toolbar shows its object texture detail and
// its alpha as the use shown reads it; a texture's compare says its DXT error under the toolbar; a texture
// no use reads says so under its uses, with its Set it aside.
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_import.h>
#include <editor/documents/mission_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/import/png_encode.h>
#include <editor/preview/texture_thumbnails.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx.h>
#include <formats/tga/tga.h>
#include <editor/session/request_factories.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/reference_picker.h>
#include "editor_ui_test_support.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace editor_ui_test {

namespace {

// The item table's project: three models, a texture, and an item naming two of the models and
// a texture the project lacks.
struct PickerProject {
	editor_test::TempProjectDir dir{"opennova_editor_ui_reference_picker"};
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	const Document *items = nullptr;
	NodeAddress item;

	bool open() {
		session.handle(request::new_project(dir.file("project"), "Picker"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &v = session.view();
		const std::string repo = test_paths_repo_root(__FILE__);
		const std::vector<uint8_t> model = test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di");
		for (const char *name : {"alpha.3di", "beta.3di", "gamma.3di"})
			if (!editor_test::write_bytes(v.project.root + "/models/" + name, model)) return false;
		if (!editor_test::write_text(v.project.root + "/defs/items.def",
		                             "begin \"Picked\"\nid 100300\ntype building\ngraphic alpha\nhusk beta\n"
		                             "hud_image gone.tga\nend\n"
		                             "begin \"Other\"\nid 100301\ntype building\ngraphic gamma\nend\n"))
			return false;
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document("defs/items.def"));
		items = session.document_for("defs/items.def");
		if (!items || !find_definition(AssetGraph(), *items, "100300", item)) return false;
		EditorRequest select = request::select_record(items->path(), item);
		session.handle(select);
		return v.project.scan->find("gamma.3di") != nullptr;
	}
	// The Inspector's section holding a field of the item.
	std::string section_of(const char *field) const {
		const SessionView &v = session.view();
		for (const InspectorSection &section :
				plan_inspector(*items, v.documents.selection.primary, v.documents.selection.primary, ""))
			for (const FieldUse &use : section.fields)
				if (use.schema->id == field) return section.key;
		return std::string();
	}
};

ImGuiID field_item(const PickerProject &project, const char *field, const char *item) {
	const std::string section = project.section_of(field);
	return item_id(Ui::window_id("Inspector"), {section.c_str(), "fields", field, item});
}

// The one Set of a field among the requests: its value ("" for none).
std::string set_value(const std::vector<EditorRequest> &requests, const char *field) {
	std::string value;
	int count = 0;
	for (const EditorRequest &request : requests)
		if (request.kind == EditorRequestKind::EditRecord && edit_of(request).field == field)
			if (const auto *text = std::get_if<std::string>(&edit_of(request).value)) {
				value = *text;
				++count;
			}
	return count == 1 ? value : std::string();
}

// A field's picker opened (its Pick), `typed` typed into its filter, which has the keyboard.
void open_picker(Ui &ui, const PickerProject &project, const char *field, const char *typed = "") {
	ui.activate(field_item(project, field, "Pick"));
	ui.frames(2);
	if (*typed) {
		ImGui::GetIO().AddInputCharactersUTF8(typed);
		ui.frames(2);
	}
}

void press(Ui &ui, ImGuiKey key) {
	ui.key(key, true);
	ui.key(key, false);
}

// Frames with a Files row's path dragged (an outside source holding it while the button is
// down), the mouse wherever the test put it.
void drag_frames(Ui &ui, const std::string &path, int count) {
	for (int i = 0; i < count; ++i) {
		ImGui::NewFrame();
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)) {
			ImGui::SetDragDropPayload(kFileDragPayload, path.c_str(), path.size() + 1);
			ImGui::EndDragDropSource();
		}
		ui.windows.draw_frame(++ui.index);
		ImGui::Render();
	}
}

// A file dragged from outside and dropped at `at`: the requests the drop raised.
std::vector<EditorRequest> drop_at(Ui &ui, ImVec2 at, const std::string &path) {
	ui.mouse(at.x, at.y);
	ui.drain();
	ImGui::GetIO().AddMouseButtonEvent(0, true);
	drag_frames(ui, path, 3);
	// Released: the one frame the drop is delivered, then the source is gone.
	ImGui::GetIO().AddMouseButtonEvent(0, false);
	drag_frames(ui, path, 1);
	ui.frames(2);
	return ui.drain();
}

// Where an item of the Inspector's form is on the screen: given the keyboard (the window keeps
// the rect of the item it focuses), then let go.
ImVec2 centre_of(Ui &ui, ImGuiID id) {
	ui.activate(id);
	ImGuiWindow *window = ImGui::FindWindowByName("Inspector");
	const ImRect rect = window ? ImGui::WindowRectRelToAbs(window, window->NavRectRel[0]) : ImRect();
	ImGui::ClearActiveID();
	ui.frames(2);
	ui.drain();
	return rect.GetCenter();
}

void test_filters_apart() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	// Typed into the graphic's picker, then closed with Escape, which keeps what was typed.
	open_picker(ui, project, "graphic", "bet");
	CHECK(GImGui->OpenPopupStack.Size == 1, "the graphic's picker opens");
	press(ui, ImGuiKey_Escape);
	CHECK(GImGui->OpenPopupStack.Size == 0, "Escape closes it");
	CHECK(ui.drain().empty(), "typing a filter sets nothing");
	// The husk's picker has its own filter: Enter picks its first name, the project's first model.
	open_picker(ui, project, "husk");
	press(ui, ImGuiKey_Enter);
	CHECK(set_value(ui.drain(), "husk") == "alpha.3di", "another field's picker lists every name");
	CHECK(GImGui->OpenPopupStack.Size == 0, "a pick closes the picker");
	// The graphic's picker still narrows to what was typed into it.
	open_picker(ui, project, "graphic");
	press(ui, ImGuiKey_Enter);
	CHECK(set_value(ui.drain(), "graphic") == "beta.3di", "the first field's picker keeps its filter");
	// The arrows move through the list: Down twice, Up once, Enter the second name.
	open_picker(ui, project, "husk");
	press(ui, ImGuiKey_DownArrow);
	press(ui, ImGuiKey_DownArrow);
	press(ui, ImGuiKey_UpArrow);
	press(ui, ImGuiKey_Enter);
	CHECK(set_value(ui.drain(), "husk") == "beta.3di", "Down, Down, Up, Enter picks the second name");
	// Escape closes it, picking nothing.
	open_picker(ui, project, "husk");
	press(ui, ImGuiKey_Escape);
	CHECK(GImGui->OpenPopupStack.Size == 0 && ui.drain().empty(), "Escape closes the picker and sets nothing");
	// Another record's graphic, in the same place of the form, has a picker of its own: none of
	// the first item's filter.
	NodeAddress other;
	CHECK(find_definition(AssetGraph(), *project.items, "100301", other), "the second item");
	EditorRequest select = request::select_record(project.items->path(), other);
	project.session.handle(select);
	ui.frames(3);
	ui.drain();
	open_picker(ui, project, "graphic");
	press(ui, ImGuiKey_Enter);
	std::vector<EditorRequest> requests = ui.drain();
	CHECK(set_value(requests, "graphic") == "alpha.3di" && only(requests, EditorRequestKind::EditRecord) &&
	              edit_of(*only(requests, EditorRequestKind::EditRecord)).address == other,
	      "another record's picker lists every name");
}

void test_drop_on_value() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	const ImVec2 graphic = centre_of(ui, field_item(project, "graphic", "##value"));
	CHECK(graphic.x > 0.0f && graphic.y > 0.0f, "the graphic's value is on the screen");
	const AssetEntry *gamma = project.session.view().project.scan->find("gamma.3di");
	CHECK(gamma != nullptr, "gamma.3di in the project");
	if (!gamma) return;
	CHECK(set_value(drop_at(ui, graphic, gamma->relative_path), "graphic") == "gamma.3di",
	      "a model dropped on a model reference is set there");
	// A file of another kind is not taken.
	const AssetEntry *table = project.session.view().project.scan->find("items.def");
	CHECK(table != nullptr, "items.def in the project");
	if (table) CHECK(set_value(drop_at(ui, graphic, table->relative_path), "graphic").empty(),
	                 "an item table dropped on a model reference sets nothing");
}

void test_missing_value_fixes() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	open_picker(ui, project, "hud_image");
	CHECK(GImGui->OpenPopupStack.Size == 1, "the HUD image's picker opens");
	CHECK(logged_frame(ui).find("The value is missing:") != std::string::npos, "the footer says the value is missing");
	// The same fix Problems offers for it: the missing texture's placeholder.
	const ImGuiID popup = field_item(project, "hud_image", "references");
	ui.activate(ImHashStr("###fix", 0, popup_item(popup, "Create a placeholder gone.tga")));
	const std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *create = only(requests, EditorRequestKind::CreateFile);
	CHECK(create && create->path == "gone.tga", "the footer's fix raises the placeholder's CreateFile");
	CHECK(GImGui->OpenPopupStack.Size == 0, "a fix closes the picker");
	// A value that resolves has no footer.
	open_picker(ui, project, "graphic");
	CHECK(logged_frame(ui).find("The value is missing") == std::string::npos, "no footer for a value found");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
}

// DI-15: a click on a missing value's red dot opens what Problems offers for it, its Add it there first: the
// sound profile the item names added to SndProf.def. A value found has no such popup (its dot is its Go to).
void test_missing_dot_fixes() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	Value profile;
	CHECK(project.items->get(project.item, "sound_profile", profile), "the item's sound profile");
	project.session.handle(request::edit_record(project.items->path(), [&] {
		Edit set;
		set.address = project.item;
		set.field = "sound_profile";
		set.value = std::string("PROF_GONE");
		return set;
	}()));
	project.session.run_operations();
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	ui.activate(field_item(project, "sound_profile", "go to dot"));
	ui.frames(2);
	CHECK(GImGui->OpenPopupStack.Size == 1, "a click on the missing value's dot opens its fixes");
	const std::string shown = logged_frame(ui);
	CHECK(shown.find("The value is missing:") != std::string::npos, "the popup says the value is missing");
	const char *label = "Add PROF_GONE to SndProf.def";
	CHECK(shown.find(label) != std::string::npos, "its Add it there is offered");
	const ImGuiID popup = field_item(project, "sound_profile", "fixes");
	ui.activate(ImHashStr(label, 0, popup_item(popup, label)));
	const std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *add = only(requests, EditorRequestKind::EditRecord);
	CHECK(add && add->open_first && add->path.find("SndProf.def") != std::string::npos && !add->edits.empty() &&
	              add->edits.front().operation == EditOperation::Add,
	      "the fix raises the Add of SndProf.def");
	CHECK(GImGui->OpenPopupStack.Size == 0, "a fix closes the popup");
}

// S13 D1: a popup's list (the graph's choices, a missing value's finding and its fixes) is made
// when it opens and kept while what it reads stands: a line of Output and the status line leave
// it; an edit of its document, an edit of another that changes what the graph holds (the files
// as they were), or a file the graph gains, make it again. S13 V3: closed, the popup lets its list
// go (none held), and opened again it makes it once more. A picker of the test's own, on the
// item's model field, in a window of its own.
void test_list_kept() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(2);
	FieldUse graphic;
	for (const FieldSchema &field : project.items->fields(project.item.kind))
		if (field.id == "graphic") graphic = project.items->field_on(project.item, field);
	CHECK(graphic.reference == ReferenceKind::Model, "the model field");
	ReferencePicker picker;
	const auto draw = [&](bool open) {
		Value value;
		project.items->get(project.item, "graphic", value);
		std::string picked;
		ImGui::NewFrame();
		ImGui::Begin("Picker test");
		if (open) ImGui::OpenPopup("references");
		picker.draw(ui.windows, *project.items, project.item, graphic, value, false, picked);
		ImGui::End();
		ImGui::Render();
	};
	draw(true);
	draw(false);
	CHECK(GImGui->OpenPopupStack.Size == 1 && picker.lists_made() == 1,
			"its list made as it opens");
	project.session.handle(request::clear_output());
	// Nothing to save: the status line alone.
	project.session.handle(request::save_all());
	draw(false);
	draw(false);
	CHECK(picker.lists_made() == 1, "Output and the status line: the list kept");
	EditorRequest set = request::edit_record(project.items->path(), Edit());
	set.edits[0].address = project.item;
	set.edits[0].field = "hp";
	set.edits[0].value = int64_t(33);
	project.session.handle(set);
	draw(false);
	CHECK(picker.lists_made() == 2, "its document edited: the list made again");
	// The start menu's TITLE window renamed: a name it defines, so the graph moves, while the
	// files, the project, the settings and the item table stand.
	const ViewRevisions before = project.session.view().revisions;
	const uint64_t revision = project.items->revision();
	project.session.handle(request::open_document("main.mnu"));
	const Document *menu = project.session.document_for("main.mnu");
	NodeAddress title;
	CHECK(menu && find_definition(AssetGraph(), *menu, "TITLE", title),
			"the start menu's TITLE window");
	EditorRequest rename = request::edit_record("main.mnu", Edit());
	rename.edits[0].address = title;
	rename.edits[0].field = "name";
	rename.edits[0].value = std::string("HEADING");
	project.session.handle(rename);
	project.session.run_operations(); // the validation the edit left due (S13 A3: the polls run it)
	const ViewRevisions &after = project.session.view().revisions;
	CHECK(after.of(ViewConcern::Graph) != before.of(ViewConcern::Graph), "the graph moved");
	CHECK(after.of(ViewConcern::Files) == before.of(ViewConcern::Files) &&
			after.of(ViewConcern::Project) == before.of(ViewConcern::Project) &&
			after.of(ViewConcern::Preferences) == before.of(ViewConcern::Preferences) &&
			project.items->revision() == revision,
			"nothing else the list reads moved");
	draw(false);
	CHECK(picker.lists_made() == 3, "the graph moved: the list made again");
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(project.session.view().project.root + "/models/delta.3di",
	                               test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di")),
	      "another model");
	project.session.handle(request::rescan());
	project.session.run_operations();
	draw(false);
	draw(false);
	CHECK(picker.lists_made() == 4, "a model the graph gains: the list made again");
	CHECK(picker.lists_held() == 1, "the open popup holds its list");
	ImGui::ClosePopupsExceptModals();
	draw(false);
	draw(false);
	CHECK(picker.lists_held() == 0 && picker.lists_made() == 4, "closed: its list let go, nothing made");
	draw(true);
	draw(false);
	CHECK(picker.lists_held() == 1 && picker.lists_made() == 5, "opened again: its list made once more");
	ImGui::ClosePopupsExceptModals();
	draw(false);
}

// S13 V3: a list is held only while its popup is drawn open. Opened on one item's model field (A),
// then on another's (B) as the window stops drawing A's picker (the Inspector's selection moved),
// A's list goes as B's popup draws, though A's picker is not drawn again: one list held, and none
// once B closes. The same popup id over another record (the selection moved while the popup
// stayed open) lets the first record's list go too.
void test_lists_let_go() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	Ui ui;
	ui.windows.set_view(&project.session.view());
	ui.frames(2);
	NodeAddress other;
	CHECK(find_definition(AssetGraph(), *project.items, "100301", other), "the other item");
	const FieldSchema *graphic = nullptr;
	for (const FieldSchema &field : project.items->fields(project.item.kind))
		if (field.id == "graphic") graphic = &field;
	CHECK(graphic != nullptr, "the model field");
	if (!graphic) return;
	ReferencePicker picker;
	using Row = std::pair<const char *, const NodeAddress *>;
	// A frame drawing each row's picker under the row's id, `open` the row whose popup opens.
	const auto draw = [&](const std::vector<Row> &rows, const char *open) {
		ImGui::NewFrame();
		ImGui::Begin("Picker test");
		for (const Row &row : rows) {
			ImGui::PushID(row.first);
			Value value;
			project.items->get(*row.second, "graphic", value);
			std::string picked;
			if (open && std::strcmp(open, row.first) == 0) ImGui::OpenPopup("references");
			picker.draw(ui.windows, *project.items, *row.second, project.items->field_on(*row.second, *graphic),
			            value, false, picked);
			ImGui::PopID();
		}
		ImGui::End();
		ImGui::Render();
	};
	const std::vector<Row> both = {{"a", &project.item}, {"b", &other}};
	draw(both, "a");
	draw(both, nullptr);
	CHECK(picker.lists_held() == 1, "A's popup open: its list held");
	const std::vector<Row> b_alone = {{"b", &other}};
	draw(b_alone, "b");
	draw(b_alone, nullptr);
	CHECK(picker.lists_held() == 1, "B's popup open, A's picker no longer drawn: A's list let go");
	ImGui::ClosePopupsExceptModals();
	draw(b_alone, nullptr);
	CHECK(picker.lists_held() == 0, "B closed: no list held");
	// One popup id, the record under it changed while it stays open.
	draw({{"x", &project.item}}, "x");
	draw({{"x", &project.item}}, nullptr);
	CHECK(picker.lists_held() == 1, "the popup open over A");
	draw({{"x", &other}}, nullptr);
	draw({{"x", &other}}, nullptr);
	CHECK(picker.lists_held() == 1, "the same popup over B: A's list let go");
	ImGui::ClosePopupsExceptModals();
	draw({{"x", &other}}, nullptr);
	CHECK(picker.lists_held() == 0, "closed: none held");
}

// ADR 0046 S15: a number naming something is picked by name in its value's place. Over a project
// holding the fixture's item catalog and the minted mission with its table: the walker's Item shows
// its catalog's name, the id muted beside it; the frame opens the list of the catalog's items by name,
// typed to narrow, Enter setting the id of the one picked; an id no name is, typed, is set as typed
// ("Use"); the walker's name index shows its string under its control; the outline beside it lists the
// rows under their pools' headings.
void test_pick_by_name() {
	editor_test::TempProjectDir dir("opennova_editor_ui_pick_by_name");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Names"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/defs/items.def", test_io::read_file(repo + "/fixtures/def/items.def")) &&
	              editor_test::write_bytes(v.project.root + "/missions/synth_logic.bms",
	                                       test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")) &&
	              editor_test::write_bytes(v.project.root + "/missions/synth_logic.bin",
	                                       test_io::read_file(repo + "/fixtures/bms/synth_logic.bin")),
	      "the catalog, the mission and its table");
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("missions/synth_logic.bms"));
	const auto *mission = dynamic_cast<const MissionDocument *>(session.document_for("missions/synth_logic.bms"));
	CHECK(mission != nullptr, "the mission open");
	if (!mission) return;
	const std::vector<const Node *> organics = mission->rows_of(MissionKind::Organic);
	CHECK(!organics.empty(), "the walker");
	if (organics.empty()) return;
	const NodeAddress walker{organics[0]->id, organics[0]->kind, 0};
	session.handle(request::select_record(mission->path(), walker));
	Ui ui;
	ui.windows.set_view(&v);
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("Wire Test Rifleman") != std::string::npos && text.find("106102") != std::string::npos,
	      "the walker's Item by its catalog's name, the id beside it");
	CHECK(text.find("\"Sgt. Walker\"") != std::string::npos, "the name index by its string");
	CHECK(text.find("Organics (2)") != std::string::npos && text.find("Area triggers (2)") != std::string::npos,
	      "the outline's rows under their pools' headings, each with its count");
	std::string section;
	for (const InspectorSection &candidate : plan_inspector(*mission, walker, walker, ""))
		for (const FieldUse &use : candidate.fields)
			if (use.schema->id == "item") section = candidate.key;
	const ImGuiID combo = item_id(Ui::window_id("Inspector"), {section.c_str(), "fields", "item", "##value"});
	const auto pick = [&](const char *typed) {
		ui.activate(combo);
		ui.frames(2);
		ImGui::GetIO().AddInputCharactersUTF8(typed);
		ui.frames(2);
		press(ui, ImGuiKey_Enter);
		const std::vector<EditorRequest> requests = ui.drain();
		const EditorRequest *edit = only(requests, EditorRequestKind::EditRecord);
		const int64_t *id = edit ? std::get_if<int64_t>(&edit_of(*edit).value) : nullptr;
		return edit && edit_of(*edit).field == "item" && edit_of(*edit).address == walker && id ? *id : int64_t(0);
	};
	CHECK(pick("Pump") == 106100, "the list by name, typed to narrow: Enter sets the id of the item picked");
	CHECK(GImGui->OpenPopupStack.Size == 0, "a pick closes the list");
	CHECK(pick("123456") == 123456, "an id no name is, typed: set as typed");
}

// S18: a texture field shows the texture its loader opens under it (the session's thumbnails, made by
// its poll): a name the project has no file for says so; one it has shows the file, its size and
// what the use's loader makes of it (an item's HUD image: its alpha alone); its picker previews the
// highlighted texture beside the list. No thumbnail device here: a framed box stands in for each.
void test_texture_previews() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const std::string root = project.session.view().project.root;
	const std::vector<uint8_t> rgba(16, 200);
	std::vector<uint8_t> tga;
	std::string error;
	CHECK(opennova::tga::tga_write_rgba32(rgba.data(), 2, 2, tga, error) &&
	              editor_test::write_bytes(root + "/textures/present.tga", tga),
	      "a texture of the project");
	project.session.handle(request::rescan());
	project.session.run_operations();
	Ui ui;
	ui.pump = [&project] { project.session.poll(); };
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	std::string text = logged_frame(ui);
	CHECK(text.find("Not found") != std::string::npos &&
	              text.find("The project has no file the game loads for gone.tga.") != std::string::npos,
	      "a texture the project lacks says so under its field");
	Edit set;
	set.address = project.item;
	set.field = "hud_image";
	set.value = std::string("present.tga");
	project.session.handle(request::edit_record(project.items->path(), set));
	ui.frames(4);
	text = logged_frame(ui);
	CHECK(text.find("present.tga") != std::string::npos && text.find("2 x 2, TGA image") != std::string::npos &&
	              text.find("As this use loads it: its alpha alone, tinted by the HUD colour") != std::string::npos,
	      "a texture the project has shows its file, its size and what the HUD makes of it");
	const TextureThumbnails *thumbnails = project.session.view().documents.thumbnails.get();
	CHECK(thumbnails && thumbnails->made() == 1 && !thumbnails->pending(), "its thumbnail made once, by a poll");
	open_picker(ui, project, "hud_image");
	text = logged_frame(ui);
	CHECK(text.find("2 x 2, TGA image") != std::string::npos, "the picker previews the highlighted texture");
	ImGui::ClosePopupsExceptModals();
	ui.frames(2);
}

// S18: a texture an import makes shows, in its tab, how it is made: its source, the options that apply,
// what its uses ask (the item's HUD image names gone.tga, so the PCX the record asks for is not what it
// reads) and the one click that raises the set_import_options of what they ask.
void test_texture_import_section() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const SessionView &view = project.session.view();
	opennova::RgbaImage image;
	image.width = image.height = 4;
	image.pixels.assign(64, 180);
	const std::vector<uint8_t> png = encode_png_rgba(image.pixels.data(), 4, 4);
	const std::string art = project.dir.file("art");
	CHECK(editor_test::write_bytes(art + "/gone.png", png), "a PNG outside the project");
	const ImportResult imported =
	        import_assets({{art + "/gone.png", {}}}, ProjectPaths::for_root(view.project.root), *view.project.document, false);
	CHECK(imported.imported.size() == 1, "imported as a modder imports it");
	if (imported.imported.size() != 1) return;
	const std::string source = imported.imported[0];
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::set_import_options(source, {{"format", "pcx"}}));
	project.session.run_operations();
	const AssetEntry *output = view.project.scan ? view.project.scan->find("gone.pcx") : nullptr;
	CHECK(output && output->imported_from == source, "made a PCX, as its record asks");
	if (!output) return;
	project.session.handle(request::open_document(output->relative_path));
	Ui ui;
	ui.pump = [&project] { project.session.poll(); };
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("Made from gone.png") != std::string::npos && text.find("change how it is made here") != std::string::npos,
	      "its tab says what it is made from");
	CHECK(text.find("Its uses ask for:") != std::string::npos && text.find("format tga") != std::string::npos,
	      "what its uses ask, with why");
	CHECK(text.find("DXT5: the retail model textures' form") == std::string::npos &&
	              text.find("the source's colours when 256 or fewer") != std::string::npos,
	      "the options that apply to a PCX, the DDS's left out");
	ImGuiWindow *info = nullptr;
	for (ImGuiWindow *window : GImGui->Windows)
		if (window->Active && std::strstr(window->Name, "texture_info")) info = window;
	CHECK(info != nullptr, "the tab's info column");
	if (!info) return;
	ui.activate(ImHashStr("Make it as its uses ask", 0, info->ID));
	const std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *set = only(requests, EditorRequestKind::SetImportOptions);
	const std::vector<std::pair<std::string, std::string>> asked = {{"format", "tga"}};
	CHECK(set && set->path == source && set->values == asked, "the one click raises the set_import_options of what its uses ask");
}

// S18, what a texture costs the game: under a model row's use in the texture's tab, its device texture's bytes
// and its .dds's (documents/texture_budget); Files listing the textures by that cost (session/texture_budget_list).
void test_texture_budget_line() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const SessionView &view = project.session.view();
	const std::vector<uint8_t> rgba(size_t(2048) * 2048 * 4, 200);
	std::vector<uint8_t> big;
	std::string error;
	CHECK(opennova::tga::tga_write_rgba32(rgba.data(), 2048, 2048, big, error) &&
	              editor_test::write_bytes(view.project.root + "/textures/crate.tga", big),
	      "a 2048 x 2048 32-bit diffuse");
	const std::string scene = project.dir.file("scene");
	CHECK(editor_test::write_text(scene + "/crate.o3d",
	                              "o3d 2\nmodel CRATE\nmaterial VS_PHONGT\ntexture crate.tga 1 0\nlod 0\npart 0 0 0 0\n"
	                              "mesh 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"),
	      "a model naming it");
	const ImportResult imported =
	        import_assets({{scene + "/crate.o3d", {}}}, ProjectPaths::for_root(view.project.root), *view.project.document, false);
	CHECK(imported.imported.size() == 1, "the model imported");
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::open_document("textures/crate.tga"));
	Ui ui;
	ui.pump = [&project] { project.session.poll(); };
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("Model diffuse: material 1 of crate.3di") != std::string::npos &&
	              text.find("In the game: 21.3 MB, 5.3 MB as its .dds") != std::string::npos,
	      "under its use, what the game holds of it and what its .dds would");
	// Files listing the textures by cost (the workspace's files.by_cost): the crate first, its size the game's,
	// the list's total beside the count.
	project.session.handle(request::set_workspace(R"({"files": {"kind": "texture", "by_cost": true}})"));
	ui.frames(4);
	ui.focus("Files");
	ui.away();
	ui.drain();
	const std::string files = logged_frame(ui);
	const size_t crate = files.find("crate.tga");
	CHECK(files.find("By cost") != std::string::npos && files.find("In game") != std::string::npos &&
	              files.find("21.3 MB in the game") != std::string::npos && crate != std::string::npos &&
	              files.find("21.3 MB", crate) != std::string::npos,
	      "Files by cost: the crate's texture's size in the game, the total beside the count");
}

// S18, a texture as the game draws it: the toolbar's object texture detail where a model row costs it, the device
// texture said at the detail shown, and the alpha as the use shown reads it (a Phong diffuse's the specular
// brightness).
void test_texture_game_view_toolbar() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const SessionView &view = project.session.view();
	const std::vector<uint8_t> rgba(size_t(64) * 64 * 4, 130);
	std::vector<uint8_t> file;
	std::string error;
	CHECK(opennova::tga::tga_write_rgba32(rgba.data(), 64, 64, file, error) &&
	              editor_test::write_bytes(view.project.root + "/textures/shine.tga", file),
	      "a 64 x 64 diffuse");
	const std::string scene = project.dir.file("scene");
	CHECK(editor_test::write_text(scene + "/shine.o3d",
	                              "o3d 2\nmodel SHINE\nmaterial VS_PHONGT\ntexture shine.tga 1 0\nlod 0\npart 0 0 0 0\n"
	                              "mesh 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"),
	      "a Phong model naming it");
	CHECK(import_assets({{scene + "/shine.o3d", {}}}, ProjectPaths::for_root(view.project.root), *view.project.document, false)
	                      .imported.size() == 1,
	      "the model imported");
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::open_document("textures/shine.tga"));
	project.session.handle(request::set_viewport("textures/shine.tga", "{\"kind\":\"texture\",\"options\":{\"as_used\":0,\"detail\":1}}"));
	DrawnDevices devices;
	Ui ui;
	ui.pump = [&project, &devices] {
		project.session.poll();
		devices.sync(project.session.viewports(), project.session.view());
	};
	ui.windows.set_view(&project.session.view());
	ui.windows.set_devices(&devices.cache);
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("Detail 1") != std::string::npos, "the toolbar's object texture detail");
	CHECK(text.find("At object texture detail 1 the game makes 32 x 32, A8R8G8B8") != std::string::npos,
	      "the device texture at the detail shown");
	CHECK(text.find("Alpha: the specular brightness VS_PHONGT reads, not transparency") != std::string::npos,
	      "the alpha as the Phong use reads it");
}

// S18, each role in its own picture: a terrain blend map shown as its terrain's splat reads it, under the picture
// its legend, each channel its share and the splat detail it weighs (preview/texture_role_view).
void test_texture_role_legend() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const SessionView &view = project.session.view();
	const std::vector<uint8_t> rgba = {100, 50, 0, 255, 0, 0, 0, 255};
	std::vector<uint8_t> file;
	std::string error;
	CHECK(opennova::tga::tga_write_rgba32(rgba.data(), 2, 1, file, error) &&
	              editor_test::write_bytes(view.project.root + "/textures/blend.tga", file),
	      "a 2 x 1 blend map");
	CHECK(editor_test::write_text(view.project.root + "/terrains/isle.trn",
	                              "polytrn_detailmap detail.tga\npolytrn_polydata isle.cpt\npolytrn_sectorcount 1\n"
	                              "polytrn_sectors 0\npolytrn_colormap colour.tga\npolytrn_detailmap_c1 grass.tga\n"
	                              "polytrn_detailblendmap blend.tga\n"),
	      "a terrain naming it");
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::open_document("textures/blend.tga"));
	project.session.handle(request::set_viewport("textures/blend.tga", "{\"kind\":\"texture\",\"options\":{\"as_used\":0}}"));
	DrawnDevices devices;
	Ui ui;
	ui.pump = [&project, &devices] {
		project.session.poll();
		devices.sync(project.session.viewports(), project.session.view());
	};
	ui.windows.set_view(&project.session.view());
	ui.windows.set_devices(&devices.cache);
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("Its weights as the terrain's splat reads them") != std::string::npos, "the legend's title");
	CHECK(text.find("weighs polytrn_detailmap_c1, grass.tga") != std::string::npos &&
	              text.find("weighs polytrn_detailmap_c2, which the terrain does not name") != std::string::npos,
	      "each channel the splat detail it weighs");
}

// S18, the compare: a texture's picture beside the DXT texture made of it, its error at the level shown said
// under the toolbar (documents/texture_compare).
void test_texture_compare_line() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const SessionView &view = project.session.view();
	std::vector<uint8_t> rgba(size_t(64) * 64 * 4);
	for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t((i * 7) / 4);
	std::vector<uint8_t> file;
	std::string error;
	CHECK(opennova::tga::tga_write_rgba32(rgba.data(), 64, 64, file, error) &&
	              editor_test::write_bytes(view.project.root + "/textures/ramp.tga", file),
	      "a 64 x 64 texture");
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::open_document("textures/ramp.tga"));
	project.session.handle(request::set_viewport("textures/ramp.tga", "{\"kind\":\"texture\",\"options\":{\"compare\":\"split\"}}"));
	// The Shell's devices, which the texture's viewport follows its document by.
	DrawnDevices devices;
	Ui ui;
	ui.pump = [&project, &devices] {
		project.session.poll();
		devices.sync(project.session.viewports(), project.session.view());
	};
	ui.windows.set_view(&project.session.view());
	ui.windows.set_devices(&devices.cache);
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("Before | DXT") != std::string::npos, "the compare's view in the toolbar");
	CHECK(text.find("DXT5 (") != std::string::npos && text.find("against its own texels: PSNR ") != std::string::npos &&
	              text.find("worst block at") != std::string::npos,
	      "under the toolbar, what the DXT texture's error is at the level shown");
}

// S18: a texture no use reads (the .tga beside the .dds its model row loads) says so under its uses, and its
// Set it aside raises set_aside_texture.
void test_texture_set_aside_line() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const SessionView &view = project.session.view();
	const std::vector<uint8_t> rgba(16 * 4, 90);
	std::vector<uint8_t> tga, dds;
	std::string error;
	CHECK(opennova::tga::tga_write_rgba32(rgba.data(), 4, 4, tga, error) && opennova::dds::dds_write_a8r8g8b8(rgba.data(), 4, 4, dds, error) &&
	              editor_test::write_bytes(view.project.root + "/textures/wall.tga", tga) &&
	              editor_test::write_bytes(view.project.root + "/textures/wall.dds", dds),
	      "a .tga and the .dds of its name");
	const std::string scene = project.dir.file("scene");
	CHECK(editor_test::write_text(scene + "/wall.o3d",
	                              "o3d 2\nmodel WALL\nmaterial FF_ST_OP\ntexture wall.tga 1 0\nlod 0\npart 0 0 0 0\n"
	                              "mesh 0 0\nv 0 0 0 0 0 1 0 0\nv 1 0 0 0 0 1 1 0\nv 0 1 0 0 0 1 0 1\nt 0 1 2\n"),
	      "a model naming the .tga");
	const ImportResult imported =
	        import_assets({{scene + "/wall.o3d", {}}}, ProjectPaths::for_root(view.project.root), *view.project.document, false);
	CHECK(imported.imported.size() == 1, "the model imported");
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::open_document("textures/wall.tga"));
	Ui ui;
	ui.pump = [&project] { project.session.poll(); };
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	const std::string text = logged_frame(ui);
	CHECK(text.find("The game never reads this file: its loader opens wall.dds in its place.") != std::string::npos,
	      "under its uses, that the game never reads it");
	ImGuiWindow *info = nullptr;
	for (ImGuiWindow *window : GImGui->Windows)
		if (window->Active && std::strstr(window->Name, "texture_info")) info = window;
	CHECK(info != nullptr, "the tab's info column");
	if (!info) return;
	ui.activate(ImHashStr("Set it aside", 0, info->ID));
	const std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *aside = only(requests, EditorRequestKind::SetAsideTexture);
	CHECK(aside && aside->path == "textures/wall.tga", "Set it aside raises set_aside_texture of the file");
}

// S18: an image the OS drops on a texture's tab, and one picked by its Replace with image..., ask first
// (preview_texture_source of that texture); the dialog the preview opens shows the texture before and after
// and replaces only on its Replace, Cancel closing it; an image dropped on a texture field's value asks for
// the file the field's loader opens.
void test_texture_drop_replaces() {
	PickerProject project;
	CHECK(project.open(), "the item table's project");
	if (!project.items) return;
	const SessionView &view = project.session.view();
	opennova::RgbaImage image;
	image.width = image.height = 4;
	image.pixels.assign(64, 90);
	std::vector<uint8_t> tga;
	std::string why;
	opennova::tga::tga_write_rgba32(image.pixels.data(), 4, 4, tga, why);
	CHECK(editor_test::write_bytes(view.project.root + "/textures/plain.tga", tga), "a plain texture");
	project.session.handle(request::rescan());
	project.session.run_operations();
	project.session.handle(request::open_document("textures/plain.tga"));
	Ui ui;
	ui.pump = [&project] { project.session.poll(); };
	ui.windows.set_view(&project.session.view());
	ui.frames(6);
	ui.focus("Document");
	ui.away();
	ui.drain();
	ImGuiWindow *info = nullptr;
	for (ImGuiWindow *window : GImGui->Windows)
		if (window->Active && std::strstr(window->Name, "texture_info")) info = window;
	CHECK(info != nullptr, "the tab's info column");
	if (!info) return;
	// Dropped on the tab.
	ui.windows.drop_files({"C:/art/new.png"}, info->Pos.x + info->Size.x * 0.5f, info->Pos.y + info->Size.y * 0.5f);
	ui.frames(1);
	std::vector<EditorRequest> requests = ui.drain();
	const EditorRequest *dropped = only(requests, EditorRequestKind::PreviewTextureSource);
	CHECK(dropped && dropped->path == "textures/plain.tga" && dropped->paths == std::vector<std::string>({"C:/art/new.png"}),
	      "a drop on the tab asks to replace the texture");
	CHECK(!only(requests, EditorRequestKind::ReplaceTexture), "a drop replaces nothing before it is asked");
	// A drop elsewhere is none's.
	ui.windows.drop_files({"C:/art/new.png"}, -100.0f, -100.0f);
	ui.frames(3);
	CHECK(!only(ui.drain(), EditorRequestKind::PreviewTextureSource), "a drop on no item does nothing");
	// A drop on the tab where another window is drawn over it is that window's: the tab takes none.
	const ImVec2 middle(info->Pos.x + info->Size.x * 0.5f, info->Pos.y + info->Size.y * 0.5f);
	const auto covered_frame = [&] {
		ImGui::NewFrame();
		ui.windows.draw_frame(++ui.index);
		ImGui::SetNextWindowPos(ImVec2(middle.x - 40.0f, middle.y - 40.0f));
		ImGui::SetNextWindowSize(ImVec2(80.0f, 80.0f));
		ImGui::Begin("Cover", nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
		ImGui::End();
		ImGui::Render();
	};
	covered_frame();
	covered_frame();
	ui.windows.drop_files({"C:/art/new.png"}, middle.x, middle.y);
	covered_frame();
	covered_frame();
	CHECK(!only(ui.drain(), EditorRequestKind::PreviewTextureSource), "a drop on a window drawn over the tab is not the tab's");
	ui.frames(2);
	ui.drain();
	// Picked through Replace with image...: the pick asks the Shell, its answer asks to replace.
	ui.activate(ImHashStr("Replace with image...", 0, info->ID));
	requests = ui.drain();
	const EditorRequest *pick = only(requests, EditorRequestKind::PickFile);
	CHECK(pick && pick->purpose == PickPurpose::TextureImage, "the button asks the Shell for an image");
	ui.windows.deliver_pick(PickPurpose::TextureImage, "C:/art/picked.tga");
	requests = ui.drain();
	const EditorRequest *picked = only(requests, EditorRequestKind::PreviewTextureSource);
	CHECK(picked && picked->path == "textures/plain.tga" && picked->paths == std::vector<std::string>({"C:/art/picked.tga"}),
	      "the image picked asks to replace the texture the button was for");

	// The dialog over a real image: what it shows, Replace raising the replace, Cancel closing it.
	opennova::RgbaImage other;
	other.width = other.height = 4;
	other.pixels.assign(64, 30);
	std::vector<uint8_t> other_tga;
	opennova::tga::tga_write_rgba32(other.pixels.data(), 4, 4, other_tga, why);
	const std::string picture = project.dir.file("new.tga");
	CHECK(editor_test::write_bytes(picture, other_tga), "an image outside the project");
	project.session.handle(request::preview_texture_source("textures/plain.tga", picture));
	CHECK(view.dialogs.texture_source.open && view.dialogs.texture_source.refusal.empty() &&
	              view.dialogs.texture_source.before && view.dialogs.texture_source.after,
	      "the preview opens with the texture before and after");
	ui.frames(3);
	ImGuiWindow *dialog = ImGui::FindWindowByName("Make a texture from an image###texture_source");
	CHECK(dialog && dialog->Active, "the dialog shows");
	if (!dialog) return;
	const std::string text = logged_frame(ui);
	CHECK(text.find("Replace plain.tga with new.tga?") != std::string::npos && text.find("Now") != std::string::npos &&
	              text.find("Then") != std::string::npos,
	      "it says what it replaces with what, before and after");
	ui.drain();
	// With the dialog open, a drop on the tab behind it is none's.
	ui.windows.drop_files({"C:/art/other.png"}, middle.x, middle.y);
	ui.frames(3);
	CHECK(!only(ui.drain(), EditorRequestKind::PreviewTextureSource), "a drop behind the open dialog is none's");
	ui.activate(ImHashStr("Replace", 0, dialog->ID));
	requests = ui.drain();
	const EditorRequest *replace = only(requests, EditorRequestKind::ReplaceTexture);
	CHECK(replace && replace->path == "textures/plain.tga" && replace->paths == std::vector<std::string>({picture}),
	      "its Replace replaces the texture with the image");
	ui.activate(ImHashStr("Cancel", 0, dialog->ID));
	requests = ui.drain();
	CHECK(only(requests, EditorRequestKind::CancelTextureSource) != nullptr, "its Cancel closes it");
	project.session.handle(request::cancel_texture_source());
	ui.frames(3);
	CHECK(!view.dialogs.texture_source.open && !GImGui->OpenPopupStack.Size, "closed, the dialog goes");

	// Dropped on a texture field's value (the item's HUD image, which names gone.tga): the file the field's
	// loader opens.
	project.session.handle(request::open_document("defs/items.def"));
	EditorRequest select = request::select_record(project.items->path(), project.item);
	project.session.handle(select);
	ui.frames(3);
	ui.focus("Inspector");
	ui.away();
	ui.drain();
	ImVec2 hud = centre_of(ui, field_item(project, "hud_image", "##value"));
	ImGuiWindow *inspector = ImGui::FindWindowByName("Inspector");
	CHECK(inspector != nullptr, "the Inspector");
	if (!inspector) return;
	// Below the Inspector's fold: a drop there lands on no window (the field does not show), so none's.
	if (hud.y >= inspector->OuterRectClipped.Max.y) {
		ui.windows.drop_files({"C:/art/hud.png"}, hud.x, hud.y);
		ui.frames(3);
		CHECK(!only(ui.drain(), EditorRequestKind::PreviewTextureSource), "a drop on a field scrolled out of sight is none's");
		ImGui::SetScrollY(inspector, inspector->Scroll.y + hud.y - (inspector->Pos.y + inspector->Size.y * 0.5f));
		ui.frames(3);
		hud = centre_of(ui, field_item(project, "hud_image", "##value"));
	}
	CHECK(hud.y > inspector->OuterRectClipped.Min.y && hud.y < inspector->OuterRectClipped.Max.y, "the HUD image's value shows");
	ui.windows.drop_files({"C:/art/hud.png"}, hud.x, hud.y);
	ui.frames(1);
	requests = ui.drain();
	const EditorRequest *field = only(requests, EditorRequestKind::PreviewTextureSource);
	CHECK(field && field->path == "gone.tga" && field->paths == std::vector<std::string>({"C:/art/hud.png"}),
	      "an image dropped on a texture field asks to make the file its loader opens");
}

} // namespace

void run_reference_picker_tests() {
	test_filters_apart();
	test_drop_on_value();
	test_missing_value_fixes();
	test_missing_dot_fixes();
	test_list_kept();
	test_lists_let_go();
	test_pick_by_name();
	test_texture_previews();
	test_texture_import_section();
	test_texture_budget_line();
	test_texture_game_view_toolbar();
	test_texture_role_legend();
	test_texture_compare_line();
	test_texture_set_aside_line();
	test_texture_drop_replaces();
}

} // namespace editor_ui_test
