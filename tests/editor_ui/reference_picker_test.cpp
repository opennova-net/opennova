// S12 D7 (ADR 0046 S12): the reference picker (editor/ui/reference_picker) in the Inspector over
// a real session's item table. Each field's popup keeps its own filter: typed into one field's
// picker, another's (another field, or the same field of another record) still lists every name,
// and the first keeps what was typed when it opens again, Escape keeping it too; the arrows move
// through the list, Enter picks, a pick is the field's Set. A Files row
// dropped on a reference's value sets the file there when the field's kind loads it, and
// nothing when it does not. A missing value's picker offers the fixes Problems offers for it.
#include <string>
#include <vector>

#include <editor/graph/reference_queries.h>
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
				plan_inspector(*items, v.documents.selection, v.documents.selection, ""))
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

} // namespace

void run_reference_picker_tests() {
	test_filters_apart();
	test_drop_on_value();
	test_missing_value_fixes();
	test_list_kept();
}

} // namespace editor_ui_test
