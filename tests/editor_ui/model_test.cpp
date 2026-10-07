// A model's part of the Inspector over a real session (ui/model_inspector_view, ADR 0046 S17, Models):
// a material selected heads the Inspector with its bullet faces' surface by name and their flags; a
// mixed material counts its surfaces, Make all is one EditRecord of every face that differs (one undo
// step) and Select the faces that differ one SelectRecord of them; a bullet face names the material it
// was made from; a LOD says what it draws at; a user point what the game reads it as; a collision
// record (a volume, a face, the collision row) what the game does with it.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/model_collision_words.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/model_surfaces.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/model_viewport.h>

#include "editor_ui_test_support.h"

namespace editor_ui_test {

namespace {

struct ModelRun {
	ProjectSession &session;
	Ui &ui;
	std::vector<EditorRequest> raised;
	void settle(int rounds = 4) {
		for (int i = 0; i < rounds; ++i) {
			ui.frames();
			EditorRequest request;
			while (ui.windows.take_request(request)) {
				raised.push_back(request);
				session.handle(request);
			}
			session.run_operations();
		}
	}
	std::vector<EditorRequest> take() {
		std::vector<EditorRequest> out;
		out.swap(raised);
		return out;
	}
};

void test_material_surface() {
	editor_test::TempProjectDir dir("opennova_editor_ui_model");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Model"));
	session.run_operations();
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/models/armory.3di", test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di")),
	      "the model written");
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("models/armory.3di"));
	session.run_operations();
	const auto *model = dynamic_cast<const ModelDocument *>(session.document_for("models/armory.3di"));
	CHECK(model != nullptr, "the model open");
	if (!model) return;
	const std::string path = model->path();
	const ModelRow *row = model->model_row();
	const CollisionRow *collision = model->collision_row();
	// The first material with bullet faces.
	NodeAddress material;
	ModelMaterialSurface surface;
	for (size_t i = 0; i < row->materials.size() && !material.child; ++i) {
		const NodeId id = row->ids.lists[kModelMaterials][i].id;
		if (model_material_surface(*model, id, surface) && surface.faces > 1) material = {row->id, node_kind(ModelKind::Material), id};
	}
	CHECK(material.child != 0 && !surface.mixed(), "a material with faces of one surface");
	if (!material.child) return;
	const std::string common = model_surface_words(surface.common()).name;
	Ui ui;
	ui.windows.set_view(&v);
	ModelRun run{session, ui, {}};

	// The material: its faces' surface by name and the flags, at the Inspector's top.
	session.handle(request::select_record(path, material));
	ui.focus("Inspector");
	run.settle();
	run.take();
	ui.away();
	std::string text = logged_frame(ui);
	const std::string count = std::to_string(surface.faces) + " faces made from this material";
	CHECK(in_order(text, {"Bullet faces", count.c_str(), "Surface", common.c_str(), "Both sides", "Bullets pass", "Front only"}),
	      "the material's bullet faces: their count, their surface by name, the flags");
	CHECK(text.find("Mixed") == std::string::npos && text.find("Make all") == std::string::npos, "one surface: not mixed");

	// Mixed: one face another surface; the count of each, Make all and Select the faces that differ.
	size_t face = 0;
	for (size_t f = 0; f < collision->faces.size(); ++f)
		if (model_face_material(*model, collision->ids.lists[2][f].id) == material) face = f;
	Edit glass;
	glass.address = {collision->id, node_kind(ModelKind::Face), collision->ids.lists[2][face].id};
	glass.field = "poly_type";
	glass.value = int64_t(surface.common() == 15 ? 14 : 15);
	session.handle(request::edit_record(path, glass));
	run.settle();
	run.take();
	ui.away();
	text = logged_frame(ui);
	const std::string make_all = "Make all " + common;
	CHECK(in_order(text, {"Surface", "Mixed:", make_all.c_str(), "Select the faces that differ"}), "mixed: counted, with its fixes");
	const ImGuiID inspector = Ui::window_id("Inspector");
	ui.activate(item_id(inspector, {"model_material", make_all.c_str()}));
	run.settle();
	std::vector<EditorRequest> raised = run.take();
	const EditorRequest *unify = only(raised, EditorRequestKind::EditRecord);
	CHECK(unify && unify->edits.size() == 1 && unify->edits[0].address == glass.address, "Make all: one EditRecord of the face");
	CHECK(model_material_surface(*model, material.child, surface) && !surface.mixed(), "one surface again");
	// A flag's box: set on every face made from the material, one EditRecord.
	ui.activate(item_id(inspector, {"model_material", "bullets_pass", "Bullets pass"}));
	run.settle();
	raised = run.take();
	const EditorRequest *pass = only(raised, EditorRequestKind::EditRecord);
	CHECK(pass && pass->edits.size() == surface.faces && model_material_surface(*model, material.child, surface) &&
	              surface.flags.size() == 3 && surface.flags[1].on == surface.faces,
	      "Bullets pass: one EditRecord setting it on every face of the material");
	// Mixed again: Select the faces that differ selects the face under Collision > Bullet faces.
	session.handle(request::edit_record(path, glass));
	run.settle();
	run.take();
	ui.activate(item_id(inspector, {"model_material", "Select the faces that differ"}));
	run.settle();
	raised = run.take();
	const EditorRequest *select = only(raised, EditorRequestKind::SelectRecord);
	CHECK(select && select->records.size() == 1 && select->records[0] == glass.address,
	      "Select the faces that differ: one SelectRecord of the face");

	// A bullet face: the material it was made from.
	session.handle(request::select_record(path, glass.address));
	run.settle();
	run.take();
	ui.away();
	text = logged_frame(ui);
	const std::string made_from = model_record_label(*model, material, nullptr);
	CHECK(in_order(text, {"Made from", made_from.c_str()}), "a face names its material");

	// A LOD: what it draws at.
	const NodeAddress lod{row->id, node_kind(ModelKind::Lod), row->ids.lists[kModelLods][0].id};
	session.handle(request::select_record(path, lod));
	run.settle();
	run.take();
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Drawn " + model_lod_range(*model->model_row(), 0) + ".") != std::string::npos, "a LOD's range in words");

	// The collision (S17): a volume by what its type does in the game, the face's words after its
	// material, the collision row's.
	CHECK(!collision->volumes.empty(), "the armory has volumes");
	if (!collision->volumes.empty()) {
		const NodeAddress volume{collision->id, node_kind(ModelKind::Volume), collision->ids.lists[kCollisionVolumes][0].id};
		session.handle(request::select_record(path, volume));
		run.settle();
		run.take();
		ui.away();
		text = logged_frame(ui);
		const ModelVolumeType &type = model_volume_type(collision->volumes[0].collidable_type);
		const std::string words = std::string(type.code) + ", " + type.words;
		CHECK(in_order(text, {"In the game", words.c_str(), "convex"}), "a volume by what its type does");
		// A volume holds no list: the lists beside it are its owner's, headed so.
		CHECK(in_order(text, {"Collision: Sections", "Collision: Volumes"}), "the owner's lists headed as the owner's");
		CHECK(text.find("[orig:") == std::string::npos, "the citations in the tooltip, not the text");
	}
	session.handle(request::select_record(path, glass.address));
	run.settle();
	run.take();
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"Made from", "In the game", "A triangle"}), "a face's words");
	session.handle(request::select_record(path, NodeAddress{collision->id, node_kind(ModelKind::Collision), 0}));
	run.settle();
	run.take();
	ui.away();
	text = logged_frame(ui);
	CHECK(in_order(text, {"In the game", "What the game"}), "the collision row's words");
	CHECK(overflowing().empty(), "nothing runs past its window");
}

// The model preview's damage states (DI-10, preview/model_damage) over a real session: the toolbar's
// Damage button where an item names the model, its popup naming the item and the husk, the death in order
// in words, Play destroy the destroyed state from the death (the clock at 0, run); the Registers popup
// grouped by what drives each register in the game.
void test_damage_popup() {
	editor_test::TempProjectDir dir("opennova_editor_ui_model_damage");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(session.handle(request::new_project(dir.file("project"), "Damage")), "a project");
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/models/crate.3di",
	                               test_io::read_file(repo + "/fixtures/threedi/synth/crate.3di")) &&
	              editor_test::write_bytes(v.project.root + "/models/armory.3di",
	                                       test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di")) &&
	              editor_test::write_text(v.project.root + "/defs/items.def",
	                                      "begin \"Pump station\"\nid 100500\ntype object\ngraphic crate\n"
	                                      "ai_function gnrc\nhusk armory\nsounddeath EXPLO_PUMP\nend\n"),
	      "the models and the item");
	editor_test::handle_to_end(session, request::rescan());
	while (v.activity.validation.running) session.poll();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	const auto serve = [&]() {
		EditorRequest request;
		while (ui.windows.take_request(request)) session.handle(request);
		devices.sync(session.viewports(), v);
	};
	const auto settle = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			serve();
			ui.frames(1);
		}
		serve();
	};
	const std::string path = "models/crate.3di";
	session.handle(request::open_document(path));
	session.handle(request::set_viewport(path, R"({"clock": {"playing": false, "ticks": 0}})"));
	settle(3);
	const auto *model = static_cast<const ModelViewport *>(session.viewports().find(path, ViewportKind::Model));
	CHECK(model && model->damage_uses().size() == 1, "the crate is the pump's graphic");
	if (!model) return;
	const ImGuiID scope = item_id(Ui::window_id("Preview"), {"model", path.c_str()});
	ui.activate(item_id(scope, {"Damage###damage"}));
	settle(1);
	ui.away();
	std::string text = logged_frame(ui);
	CHECK(text.find("Pump station (items.def) draws it intact") != std::string::npos, "the item naming the model");
	CHECK(text.find("Its husk: armory.3di") != std::string::npos, "the husk the game swaps in");
	CHECK(text.find("The husk swap: armory is drawn in the item's place from here on.") != std::string::npos,
	      "the death in order, in words");
	ui.activate(popup_item(ImHashStr("damage", 0, scope), "Play destroy"));
	settle(2);
	CHECK(model->options().damage.state == DamageState::Destroyed, "Play destroy: the destroyed state");
	CHECK(session.viewports().clock().playing(), "Play destroy: the clock runs from the death");
	ImGui::ClosePopupsExceptModals();
	settle(1);
	// The armory's Registers: FLICKER under the lights, in the game's words.
	session.handle(request::open_document("models/armory.3di"));
	settle(3);
	const ImGuiID armory = item_id(Ui::window_id("Preview"), {"model", "models/armory.3di"});
	ui.activate(item_id(armory, {"Registers"}));
	settle(1);
	ui.away();
	text = logged_frame(ui);
	CHECK(text.find("Lights, sky and weather") != std::string::npos && text.find("Flicker (FLICKER)") != std::string::npos,
	      "the register in its group, in the game's words");
}

// A definition table's record in the Preview (DI-21, preview/definition_viewport) over a real session: the
// record's picture beside the table, its line naming the model it draws with a Go to of the model's file (an
// OpenDocument, a step of the navigation history), Replay the clock at 0 and run.
void test_definition_view() {
	editor_test::TempProjectDir dir("opennova_editor_ui_definition");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(session.handle(request::new_project(dir.file("project"), "Definition")), "a project");
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	CHECK(editor_test::write_bytes(v.project.root + "/models/crate.3di",
	                               test_io::read_file(repo + "/fixtures/threedi/synth/crate.3di")) &&
	              editor_test::write_bytes(v.project.root + "/models/armory.3di",
	                                       test_io::read_file(repo + "/fixtures/threedi/synth/armory.3di")) &&
	              editor_test::write_text(v.project.root + "/defs/items.def",
	                                      "begin \"Pump station\"\nid 100500\ntype object\ngraphic crate\n"
	                                      "ai_function gnrc\nhusk armory\nend\n"),
	      "the models and the item");
	editor_test::handle_to_end(session, request::rescan());
	while (v.activity.validation.running) session.poll();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	std::vector<EditorRequest> taken;
	const auto serve = [&]() {
		EditorRequest request;
		while (ui.windows.take_request(request)) {
			taken.push_back(request);
			session.handle(request);
		}
		devices.sync(session.viewports(), v);
	};
	const auto settle = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			serve();
			ui.frames(1);
		}
		serve();
	};
	const std::string path = "defs/items.def";
	session.handle(request::open_document(path));
	const Document *table = session.document_for(path);
	CHECK(table && !table->rows().empty(), "the table open");
	if (!table || table->rows().empty()) return;
	session.handle(request::select_record(path, NodeAddress{table->rows()[0]->id, table->rows()[0]->kind, 0}));
	session.handle(request::set_viewport(path, R"({"kind": "definition", "clock": {"playing": false, "ticks": 5}})"));
	settle(3);
	CHECK(v.documents.preview_shown == ViewportKind::Definition, "the record's picture is the Preview's");
	std::string text = logged_frame(ui);
	CHECK(text.find("Draws crate (graphic)") != std::string::npos, "its line names the model it draws");
	const ImGuiID scope = item_id(Ui::window_id("Preview"), {"definition", path.c_str()});
	taken.clear();
	ui.activate(item_id(scope, {"Replay"}));
	settle(1);
	CHECK(session.viewports().clock().playing() && session.viewports().clock().ticks() < 5, "Replay: the clock at 0, run");
	taken.clear();
	ui.activate(item_id(scope, {"Go to"}));
	settle(1);
	const bool opened = std::any_of(taken.begin(), taken.end(), [](const EditorRequest &request) {
		return request.kind == EditorRequestKind::OpenDocument && request.path == "models/crate.3di";
	});
	CHECK(opened && v.documents.active == "models/crate.3di", "Go to opens the model's file");
}

// Place in mission on the model preview (DI-12, preview/model_placement) over a real session: with the mission
// open before the model, the toolbar's button raises one EditInViewport of the model's place_in_mission, which
// makes the crate's item in items.def and arms the mission's Place tool with it, the mission made active; for a
// model two items draw the button offers them, the one picked armed (an OpenDocument of the mission and a
// SetViewport of its tool).
void test_place_in_mission() {
	editor_test::TempProjectDir dir("opennova_editor_ui_place_in_mission");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	CHECK(session.handle(request::new_project(dir.file("project"), "Place")), "a project");
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string repo = test_paths_repo_root(__FILE__);
	const AssetEntry *items = v.project.scan ? v.project.scan->find("items.def") : nullptr;
	CHECK(items != nullptr, "the project's items.def");
	if (!items) return;
	const std::string catalog = items->relative_path;
	CHECK(editor_test::write_bytes(v.project.root + "/models/crate.3di", test_io::read_file(repo + "/fixtures/threedi/synth/crate.3di")) &&
	              editor_test::write_bytes(v.project.root + "/models/pump.3di", test_io::read_file(repo + "/fixtures/threedi/synth/pump.3di")) &&
	              editor_test::write_bytes(v.project.root + "/missions/synth_logic.bms",
	                                       test_io::read_file(repo + "/fixtures/bms/synth_logic.bms")) &&
	              editor_test::write_text(v.project.root + "/" + catalog,
	                                      "begin \"Null\"\nid 100000\ntype marker\nend\n"
	                                      "begin \"Drop Pump\"\nid 106100\ntype object\ngraphic pump\nend\n"
	                                      "begin \"Drop Scaled Pump\"\nid 106103\ntype object\ngraphic pump\nend\n"),
	      "the models, the mission and the items");
	editor_test::handle_to_end(session, request::rescan());
	while (v.activity.validation.running) session.poll();
	DrawnDevices devices;
	Ui ui;
	ui.windows.set_view(&v);
	ui.windows.set_devices(&devices.cache);
	std::vector<EditorRequest> taken;
	const auto count_of = [&](EditorRequestKind kind) {
		return std::count_if(taken.begin(), taken.end(), [kind](const EditorRequest &request) { return request.kind == kind; });
	};
	const auto serve = [&]() {
		EditorRequest request;
		while (ui.windows.take_request(request)) {
			taken.push_back(request);
			session.handle(request);
		}
		session.run_operations();
		devices.sync(session.viewports(), v);
	};
	const auto settle = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			serve();
			ui.frames(1);
		}
		serve();
	};
	const std::string mission = "missions/synth_logic.bms";
	session.handle(request::open_document(mission));
	settle(2);
	const auto armed = [&](int64_t item) {
		const auto *viewport = static_cast<const MissionViewport *>(session.viewports().find(mission, ViewportKind::Mission));
		return viewport && viewport->options().tool == MissionTool::Place && viewport->options().item == item &&
		       v.documents.active == mission;
	};
	session.handle(request::open_document("models/crate.3di"));
	settle(3);
	const ImGuiID crate = item_id(Ui::window_id("Preview"), {"model", "models/crate.3di"});
	taken.clear();
	ui.activate(item_id(crate, {"Place in mission"}));
	settle(2);
	const EditorRequest *placed = only(taken, EditorRequestKind::EditInViewport);
	CHECK(placed && placed->command.name == "place_in_mission" && placed->path == "models/crate.3di",
	      "Place in mission: one EditInViewport of the model's place_in_mission");
	const Document *table = session.document_for(catalog);
	int64_t made = 0;
	for (size_t i = 0; table && i < table->rows().size(); ++i)
		if (table->rows()[i]->name() == "crate") {
			Value id;
			if (table->get({table->rows()[i]->id, table->rows()[i]->kind, 0}, "id", id)) made = std::get<int64_t>(id);
		}
	CHECK(made > 100000 && armed(made), "the crate's item made, the mission's Place tool armed with it");
	// The pump, which two items draw: the button offers them.
	session.handle(request::open_document("models/pump.3di"));
	settle(3);
	const ImGuiID pump = item_id(Ui::window_id("Preview"), {"model", "models/pump.3di"});
	taken.clear();
	ui.activate(item_id(pump, {"Place in mission"}));
	settle(1);
	CHECK(count_of(EditorRequestKind::EditInViewport) == 0, "several items: a choice, nothing raised yet");
	ui.activate(popup_item(ImHashStr("place_in_mission", 0, pump), "Drop Scaled Pump (106103)"));
	settle(2);
	CHECK(count_of(EditorRequestKind::OpenDocument) == 1 && count_of(EditorRequestKind::SetViewport) == 1 &&
	              armed(106103),
	      "the one picked armed in the mission's Place tool");
	ImGui::ClosePopupsExceptModals();
	settle(1);
}

} // namespace

void run_model_tests() {
	test_material_surface();
	test_damage_popup();
	test_definition_view();
	test_place_in_mission();
}

} // namespace editor_ui_test
