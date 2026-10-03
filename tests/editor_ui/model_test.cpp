// A model's part of the Inspector over a real session (ui/model_inspector_view, ADR 0046 S17, Models):
// a material selected heads the Inspector with its bullet faces' surface by name and their flags; a
// mixed material counts its surfaces, Make all is one EditRecord of every face that differs (one undo
// step) and Select the faces that differ one SelectRecord of them; a bullet face names the material it
// was made from; a LOD says what it draws at; a user point what the game reads it as; a collision
// record (a volume, a face, the collision row) what the game does with it.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/model_collision_words.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/model_surfaces.h>

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

} // namespace

void run_model_tests() { test_material_surface(); }

} // namespace editor_ui_test
