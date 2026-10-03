// A model's bullet-face surfaces on its materials, and its records in words (ADR 0046 S17, Models).
//
// Synthetic: every face of a model our builder wrote has the material whose triangle it is (the
// collision LOD found); a material's surface is its faces' (one value, or mixed with the faces that
// differ); setting it gives every face made from it the surface in one batch, one undo step, the other
// materials' faces untouched, and a flag the same; a material added here, a value past a byte and a
// record that is no material are refused; the surfaces read in the game's words (14 Metal, the `metal`
// row; past 23 the `obj` row); the records read in words (a LOD by what it draws at and its parts, a
// part by the add-on's PN## name, a user point by its role, a face by its surface).
//
// Retail (a leg, OPENNOVA_JO_DIR): every model the game install serves: how many faces find their
// material, and how many materials hold faces of more than one surface (the shipped "mixed" ones).
#include <editor/documents/document_types.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/model_surfaces.h>
#include <editor/project/project_document.h>

#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/vfs/vfs.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::editor;
using namespace opennova::threedi;
namespace fs = std::filesystem;

namespace {

constexpr NodeKind kMaterial = node_kind(ModelKind::Material);
constexpr NodeKind kLod = node_kind(ModelKind::Lod);
constexpr NodeKind kFace = node_kind(ModelKind::Face);
constexpr NodeKind kPanm = node_kind(ModelKind::PartAnimation);

std::string synth_dir() { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth"; }

bool load(ModelDocument &document, const std::string &path, const std::string &name) {
	Diagnostic error;
	return document.load(path, name, AssetKind::Model, "jo", error);
}

// Every synthetic model with bullet faces: each face found its material.
int faces_find_their_materials() {
	int models = 0;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(synth_dir(), ec)) {
		if (entry.path().extension() != ".3di") continue;
		ModelDocument document;
		TEST_EXPECT(load(document, entry.path().generic_string(), entry.path().filename().generic_string()));
		const CollisionRow *collision = document.collision_row();
		if (!collision || collision->faces.empty()) continue;
		const auto made = model_face_materials(document.model_row()->base);
		TEST_EXPECT(made->material.size() == collision->faces.size());
		TEST_EXPECT(made->lod >= 0);
		if (made->matched != collision->faces.size())
			std::printf("faces: %s: %zu of %zu faces find a material\n", entry.path().filename().string().c_str(),
			            made->matched, collision->faces.size());
		// A skinned fixture's bullet faces may be a box of their own, not its skin's triangles (the person's,
		// the generator's add_face_box): only those that lie on a triangle find its material.
		if (document.model_row()->header.mesh_type == THREEDI_MESH_SKINNED) continue;
		TEST_EXPECT(made->matched == collision->faces.size());
		TEST_EXPECT(model_faces_without_material(document) == 0);
		++models;
	}
	TEST_EXPECT(models > 5);
	std::printf("faces: every face of %d synthetic models finds the material whose triangle it is\n", models);
	return 0;
}

// The first material of the document with bullet faces, and its surface.
NodeId material_with_faces(const ModelDocument &document, ModelMaterialSurface &surface) {
	const ModelRow *row = document.model_row();
	for (size_t i = 0; i < row->materials.size(); ++i) {
		const NodeId id = row->ids.lists[kModelMaterials][i].id;
		if (model_material_surface(document, id, surface) && surface.faces > 0) return id;
	}
	return 0;
}

int set_on_a_material() {
	ModelDocument document;
	TEST_EXPECT(load(document, synth_dir() + "/armory.3di", "armory.3di"));
	const ModelRow *row = document.model_row();
	const CollisionRow *collision = document.collision_row();
	TEST_EXPECT(row && collision && !collision->faces.empty());
	if (!row || !collision || collision->faces.empty()) return 1;
	const std::vector<ThreediCollisionFace> faces_before = collision->faces;
	ModelMaterialSurface surface;
	const NodeId material = material_with_faces(document, surface);
	TEST_EXPECT(material != 0 && surface.faces > 0 && !surface.mixed());
	const int64_t was = surface.common();
	const int64_t glass = was == 15 ? 14 : 15;

	// Set: every face of the material, one batch, one undo step; the others' untouched.
	std::vector<Edit> edits;
	std::string refusal;
	TEST_EXPECT(model_surface_edits(document, material, glass, edits, refusal));
	TEST_EXPECT(edits.size() == surface.faces);
	Diagnostic error;
	TEST_EXPECT(document.apply(edits, error));
	ModelMaterialSurface after;
	TEST_EXPECT(model_material_surface(document, material, after) && !after.mixed() && after.common() == glass &&
	            after.faces == surface.faces);
	size_t changed = 0;
	for (size_t f = 0; f < faces_before.size(); ++f) {
		const ThreediCollisionFace &now = document.collision_row()->faces[f];
		if (now.poly_type != faces_before[f].poly_type) ++changed;
		TEST_EXPECT(now.material_flags == faces_before[f].material_flags);
	}
	TEST_EXPECT(changed == surface.faces);
	// Set again: nothing to do.
	TEST_EXPECT(model_surface_edits(document, material, glass, edits, refusal) && edits.empty());
	document.undo();
	TEST_EXPECT(model_material_surface(document, material, after) && after.common() == was);
	TEST_EXPECT(!document.can_undo());

	// A flag: on every face of the material, then off; one step each.
	TEST_EXPECT(model_face_flag_edits(document, material, 0x100, true, edits, refusal) && !edits.empty());
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(model_material_surface(document, material, after) && after.flags.size() == 3 &&
	            after.flags[1].bit == 0x100 && after.flags[1].on == after.faces);
	document.undo();

	// Mixed: one face of the material another surface; Make all is the common one again.
	const std::vector<NodeAddress> none = model_differing_faces(document, material);
	TEST_EXPECT(none.empty());
	// The material's faces, each found its material back (model_face_material).
	std::vector<size_t> own;
	for (size_t f = 0; f < collision->faces.size(); ++f)
		if (model_face_material(document, collision->ids.lists[2][f].id).child == material) own.push_back(f);
	TEST_EXPECT(own.size() == surface.faces);
	Edit one;
	one.address = {collision->id, kFace, collision->ids.lists[2][own.front()].id};
	one.field = "poly_type";
	one.value = int64_t(17);
	TEST_EXPECT(document.apply(one, error));
	TEST_EXPECT(model_material_surface(document, material, after) && after.mixed() && after.differing.size() == 1);
	TEST_EXPECT(model_material_surface_words(after).find("Mixed:") == 0);
	const std::vector<NodeAddress> differ = model_differing_faces(document, material);
	TEST_EXPECT(differ.size() == 1 && differ[0] == one.address);
	TEST_EXPECT(model_surface_edits(document, material, after.common(), edits, refusal) && edits.size() == 1);
	TEST_EXPECT(document.apply(edits, error));
	TEST_EXPECT(model_material_surface(document, material, after) && !after.mixed());

	// Refusals: a material added here makes no face; a value past a byte; a record that is no material.
	TEST_EXPECT(document.apply([&] {
		Edit add;
		add.operation = EditOperation::Add;
		add.address = {row->id, kMaterial, 0};
		return add;
	}(), error));
	const NodeId added = document.last_added();
	TEST_EXPECT(!model_surface_edits(document, added, 14, edits, refusal) && !refusal.empty());
	TEST_EXPECT(!model_surface_edits(document, material, 256, edits, refusal));
	TEST_EXPECT(!model_surface_edits(document, row->ids.lists[kModelUserPoints].empty() ? 1 : row->ids.lists[kModelUserPoints][0].id,
	                                 14, edits, refusal));
	TEST_EXPECT(!model_face_flag_edits(document, material, 0x2, true, edits, refusal));
	std::printf("set on a material: every face made from it in one step, mixed and unified, the refusals\n");
	return 0;
}

int words() {
	// The surfaces are the game's tag table from row 4.
	TEST_EXPECT(model_surface_words(14).name == "Metal" && model_surface_words(14).tag == "metal");
	TEST_EXPECT(model_surface_words(0).tag == "obj" && model_surface_words(1).tag == "dirt");
	TEST_EXPECT(model_surface_words(18).name == "Heavy metal" && model_surface_words(18).tag == "hmetal");
	TEST_EXPECT(model_surface_words(15).tag == "glass" && !model_surface_words(15).note.empty());
	TEST_EXPECT(model_surface_words(23).tag == "uwatersurface" && model_surface_words(23).known);
	TEST_EXPECT(!model_surface_words(24).known && model_surface_words(24).tag == "obj");
	TEST_EXPECT(model_surface_choices().size() == 24 && model_surface_choices()[14].label == "Metal");

	// The records in words.
	ModelDocument document;
	TEST_EXPECT(load(document, synth_dir() + "/house_lod0_sine_rotx.3di", "house.3di"));
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && !row->lods.empty() && !row->lods[0].panm.empty());
	if (!row || row->lods.empty() || row->lods[0].panm.empty()) return 1;
	const NodeAddress lod0{row->id, kLod, row->ids.lists[kModelLods][0].id};
	const std::string lod = model_record_label(document, lod0, nullptr);
	TEST_EXPECT(lod.rfind("LOD 0: ", 0) == 0 && lod.find("part") != std::string::npos);
	TEST_EXPECT(model_part_name(*row, 0) == "PN01" && model_part_name(*row, -1) == "None" &&
	            model_part_name(*row, 255) == "None");
	TEST_EXPECT(model_part_name(*row, 200).rfind("No part 200 (LOD 0 has ", 0) == 0);
	const NodeAddress panm{row->id, kPanm, row->ids.lists[kModelLods][0].lists[kModelOwnList][0].id};
	const std::string moves = model_record_label(document, panm, nullptr);
	TEST_EXPECT(moves.rfind("PN", 0) == 0);
	std::printf("words: %s; %s\n", lod.c_str(), moves.c_str());
	TEST_EXPECT(model_user_point_role("sitex00A") == "passenger seat" && model_user_point_role("ctrlx05") == "control seat" &&
	            model_user_point_role("FLARE01") == "flare launch point" && model_user_point_role("ground ") == "ground anchor" &&
	            model_user_point_role("Fastrope").empty());

	// A LOD past one whose threshold is 0 is never drawn by distance.
	ModelRow levels = *row;
	levels.lods.resize(3, row->lods[0]);
	levels.lods[0].lod.lod_threshold = 160;
	levels.lods[1].lod.lod_threshold = 0;
	levels.lods[2].lod.lod_threshold = 0;
	TEST_EXPECT(model_lod_range(levels, 0) == "above 160 px" && model_lod_range(levels, 1) == "below 160 px");
	TEST_EXPECT(!model_lod_drawn(levels, 2) && model_lod_range(levels, 2).rfind("never", 0) == 0);

	// A face by its surface; the armory's faces.
	ModelDocument armory;
	TEST_EXPECT(load(armory, synth_dir() + "/armory.3di", "armory.3di"));
	const CollisionRow *collision = armory.collision_row();
	TEST_EXPECT(collision && !collision->faces.empty());
	if (!collision || collision->faces.empty()) return 1;
	const NodeAddress face{collision->id, kFace, collision->ids.lists[2][0].id};
	const std::string face_words = model_record_label(armory, face, nullptr);
	TEST_EXPECT(face_words.rfind("Face 1: ", 0) == 0);
	// The value words: a face's surface by name; a user point's part by the add-on's name.
	const DocumentType *type = document_type(DocumentTypeId::Model);
	TEST_EXPECT(type && type->record_label == model_record_label && type->value_label == model_value_label);
	std::printf("words: %s\n", face_words.c_str());
	return 0;
}

// Every model the game install serves: the faces that find their material, and the materials whose
// faces carry more than one surface.
int retail_surfaces() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every model's faces and their materials)");
	const ProjectDocument project;
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t models = 0, with_faces = 0, faces = 0, matched = 0, all_matched = 0;
	size_t materials = 0, mixed = 0, mixed_models = 0, differing = 0, flags_mixed = 0;
	std::map<std::string, size_t> examples;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::gameprofile::gameprofile_scr_policy_for_code(project.target_game.c_str()));
		TEST_EXPECT(game.mount_game(root, expansion) && game.has_mounted_archive());
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(fs::path(file.logical_name).extension().string()) != ".3di") continue;
			if (!seen.insert(file.source_path + "|" + normalized_logical_name(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			if (!game.read_file(file.logical_name, bytes) || bytes.size() < 4 || std::memcmp(bytes.data(), "3DI3", 4) != 0)
				continue;
			ModelDocument document;
			Diagnostic error;
			if (!document.load_bytes(bytes, file.logical_name, AssetKind::Model, project.target_game, error)) continue;
			++models;
			const CollisionRow *collision = document.collision_row();
			if (!collision || collision->faces.empty()) continue;
			++with_faces;
			const auto made = model_face_materials(document.model_row()->base);
			faces += collision->faces.size();
			matched += made->matched;
			all_matched += made->matched == collision->faces.size() ? 1 : 0;
			bool any = false;
			const ModelRow *row = document.model_row();
			for (size_t i = 0; i < row->materials.size(); ++i) {
				ModelMaterialSurface surface;
				if (!model_material_surface(document, row->ids.lists[kModelMaterials][i].id, surface) || !surface.faces) continue;
				++materials;
				for (const ModelFlagCount &flag : surface.flags) flags_mixed += flag.on && flag.on != surface.faces ? 1 : 0;
				if (!surface.mixed()) continue;
				++mixed;
				differing += surface.differing.size();
				any = true;
				if (examples.size() < 8) examples[file.logical_name] = surface.differing.size();
			}
			mixed_models += any ? 1 : 0;
		}
	}
	if (models == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's models in its archives");
	std::printf("retail surfaces: %zu models, %zu with bullet faces (%zu faces, %zu find their material, %zu models "
	            "whole); %zu materials with faces, %zu of them mixed (%zu faces differ) in %zu models; %zu material "
	            "flags mixed\n",
	            models, with_faces, faces, matched, all_matched, materials, mixed, differing, mixed_models, flags_mixed);
	for (const auto &e : examples) std::printf("retail surfaces: mixed in %s (%zu faces differ)\n", e.first.c_str(), e.second);
	// Nearly every face is a triangle of a render LOD (a first-person gun's own collision mesh aside).
	TEST_EXPECT(matched * 10 >= faces * 9);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (faces_find_their_materials() != 0) return 1;
	if (set_on_a_material() != 0) return 1;
	if (words() != 0) return 1;
	return retail_surfaces();
}
