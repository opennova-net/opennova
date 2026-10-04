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
// material, and how many materials hold faces of more than one surface (the shipped "mixed" ones); every
// face lying on a triangle it was made from (the triangle's corners on the 8.8 grid the face's, wound with
// it) takes that triangle's material.
#include <editor/documents/document_types.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/model_surfaces.h>
#include <editor/project/project_document.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_o3d_read.h>
#include <formats/threedi/threedi_strip_decode.h>
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/material_texture.h>

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
	int models = 0, checked = 0;
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
		// The right material, not just one: where a face's corners are a triangle's of the collision LOD
		// (corner for corner, within the 8.8 grid), that triangle's material is the face's.
		const Threedi3di3 &base = *document.model_row()->base;
		std::vector<ThreediCollisionObjectRun> runs(base.collision->object_count);
		TEST_EXPECT(threedi_collision_object_runs(base.collision, runs.data()));
		const ThreediLod &lod = base.lods[made->lod];
		std::vector<uint16_t> tris;
		for (size_t o = 0; o < base.collision->object_count; ++o)
			for (int32_t f = 0; f < base.collision->objects[o].num_faces; ++f) {
				const size_t face = size_t(runs[o].face_start + f);
				const ThreediCollisionFace &cf = base.collision->faces[face];
				int material = -2;
				for (size_t s = 0; s < lod.strip_count && material == -2; ++s) {
					if (!threedi_decode_strip_indices(lod, lod.strips[s], tris)) continue;
					for (size_t t = 0; t + 2 < tris.size() && material == -2; t += 3) {
						// Each face corner one of the triangle's (in either winding, from any corner: the
						// synthetic models hold no sheet stored both ways; two_sided_sheet tests that).
						ThreediBuildVec3 corner[3];
						for (int k = 0; k < 3; ++k) {
							const float *p = lod.vertices.items[lod.strips[s].start_vertex + tris[t + size_t(k)]].position;
							corner[k] = threedi_build_to_mission(ThreediBuildVec3{p[0], p[1], p[2]});
						}
						bool same = true;
						for (int k = 0; k < 3 && same; ++k) {
							const float *q = base.collision->vertices[size_t(runs[o].vertex_start + cf.vert_index[k])].position;
							bool found = false;
							for (int j = 0; j < 3 && !found; ++j)
								found = std::fabs(corner[j].x - q[0]) < 0.004 && std::fabs(corner[j].y - q[1]) < 0.004 &&
								        std::fabs(corner[j].z - q[2]) < 0.004;
							same = found;
						}
						if (same) material = threedi_material_array_index_for_id(base, lod.strips[s].material_index);
					}
				}
				if (material == -2) continue;
				TEST_EXPECT(made->material[face] == material);
				++checked;
			}
		++models;
	}
	TEST_EXPECT(models > 5 && checked > 100);
	std::printf("faces: every face of %d synthetic models finds the material whose triangle it is (%d checked corner "
	            "for corner)\n",
	            models, checked);
	return 0;
}

// A sheet stored in both windings, each side its own material, with a bullet face per side: each face is
// its own side's (the old rule, the nearest middle, gave both faces the later triangle's material). The
// faces are listed back side first so a rule by order alone would also get them wrong.
int two_sided_sheet() {
	std::istringstream text(R"(o3d 1
model TWOSIDE
material FF_MT_OP
texture front.tga 1 0 0 0
material FF_MT_OP
texture back.tga 1 0 0 0
lod 200 two
part 0 0 0 0
strip 0 0
v 0 -1 0 1 0 0 0 1
v 0 1 0 1 0 0 1 1
v 0 1 2 1 0 0 1 0
t 0 1 2
strip 1 0
v 0 -1 0 -1 0 0 0 1
v 0 1 0 -1 0 0 1 1
v 0 1 2 -1 0 0 1 0
t 0 2 1
panm 0 0
cobj 0 0 0 0
cv 0 -1 0
cv 0 1 0
cv 0 1 2
cf 0 2 1 15
cf 0 1 2 14
)");
	std::vector<uint8_t> bytes;
	std::vector<SceneFinding> findings;
	const bool built = threedi_o3d_build(text, opennova::renderer::material_descriptor_tangent_lookup,
	                                     opennova::renderer::material_texture_dds_only, bytes, findings);
	for (const SceneFinding &f : findings) std::printf("two-sided: line %d: %s\n", f.line, f.message.c_str());
	TEST_EXPECT(built);
	ModelDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes, "twoside.3di", AssetKind::Model, "jo", error));
	const CollisionRow *collision = document.collision_row();
	TEST_EXPECT(collision && collision->faces.size() == 2);
	if (!collision || collision->faces.size() != 2) return 1;
	const auto made = model_face_materials(document.model_row()->base);
	TEST_EXPECT(made->matched == 2 && made->ambiguous == 2 && made->against == 0);
	// Face 0 is wound as the back (material 1), face 1 as the front (material 0).
	TEST_EXPECT(made->material.size() == 2 && made->material[0] == 1 && made->material[1] == 0);
	// So the front material's surface is face 1's alone, and setting it writes face 1 alone.
	const ModelRow *row = document.model_row();
	const NodeId front = row->ids.lists[kModelMaterials][0].id, back = row->ids.lists[kModelMaterials][1].id;
	ModelMaterialSurface surface;
	TEST_EXPECT(model_material_surface(document, front, surface) && surface.faces == 1 && surface.common() == 14);
	TEST_EXPECT(model_material_surface(document, back, surface) && surface.faces == 1 && surface.common() == 15);
	std::vector<Edit> edits;
	std::string refusal;
	TEST_EXPECT(model_surface_edits(document, front, 17, edits, refusal) && edits.size() == 1 &&
	            edits[0].address.child == collision->ids.lists[kCollisionFaces][1].id);
	std::printf("two-sided: each face of a sheet stored both ways is its own side's material\n");
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
	// The last LOD draws at every size below the one before it whatever its own threshold (the walk past
	// the last draws the last, @ 0x5c3b58), and a single LOD at any size (18 retail models end on a
	// positive threshold; Wcrate3X's one LOD is 30).
	const std::vector<int32_t> ending = {160, 64, 19, 6};
	TEST_EXPECT(model_lod_range(ending, 3) == "below 19 px" && model_lod_range(ending, 2) == "19 to 64 px");
	TEST_EXPECT(model_lod_range(std::vector<int32_t>{30}, 0) == "at any size");
	TEST_EXPECT(model_lod_range(std::vector<int32_t>{30, 10}, 0) == "above 30 px");
	// What rounds do with a surface: the five a round goes on through, with their cost.
	for (const int64_t soft : {7, 15, 16, 17, 19})
		TEST_EXPECT(model_surface_words(soft).passes && model_surface_words(soft).energy_cost > 0.0 &&
		            model_surface_words(soft).note.find("Rounds go on through") == 0 &&
		            model_surface_label(soft).find("(rounds pass)") != std::string::npos);
	TEST_EXPECT(model_surface_words(15).energy_cost == 10.0 && model_surface_words(16).energy_cost == 4.0 &&
	            model_surface_words(17).energy_cost == 8.0);
	TEST_EXPECT(!model_surface_words(14).passes && model_surface_label(14) == "Metal");
	TEST_EXPECT(model_surface_choices()[16].label == "Cloth (rounds pass)");

	// A face by its surface; the armory's faces.
	ModelDocument armory;
	TEST_EXPECT(load(armory, synth_dir() + "/armory.3di", "armory.3di"));
	const CollisionRow *collision = armory.collision_row();
	TEST_EXPECT(collision && !collision->faces.empty());
	if (!collision || collision->faces.empty()) return 1;
	// A material's record name (the graph's paths, the import plan's Needed by) is its place, not its
	// shader tag; its words are its title.
	const ModelRow *armory_row = armory.model_row();
	const NodeAddress material{armory_row->id, kMaterial, armory_row->ids.lists[kModelMaterials][0].id};
	TEST_EXPECT(armory.record_name(material) == "Material 1");
	TEST_EXPECT(armory.record_path(material).find(
	                    opennova::strutil::fixed_string(armory_row->materials[0].material.shader_name,
	                                                    sizeof(armory_row->materials[0].material.shader_name))) ==
	            std::string::npos);
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
// The faces of `base` lying corner for corner (within the 8.8 grid) on a triangle of the collision LOD
// wound with them: each must have taken one such triangle's material (a sheet stored both ways puts two
// triangles on one face's corners, only one wound with it). Counts the faces checked and those wrong.
void faces_on_their_own_triangles(const Threedi3di3 &base, const ModelFaceMaterials &made, size_t &checked,
                                  size_t &wrong, std::string &first_wrong) {
	if (made.lod < 0 || !base.collision) return;
	struct Tri {
		ThreediBuildVec3 corner[3];
		ThreediBuildVec3 normal;
		int material;
	};
	const auto sub = [](const ThreediBuildVec3 &a, const ThreediBuildVec3 &b) {
		return ThreediBuildVec3{a.x - b.x, a.y - b.y, a.z - b.z};
	};
	const auto cross = [](const ThreediBuildVec3 &a, const ThreediBuildVec3 &b) {
		return ThreediBuildVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	};
	const auto key = [](const ThreediBuildVec3 &m) {
		return std::make_tuple(int(std::floor(m.x * 64)), int(std::floor(m.y * 64)), int(std::floor(m.z * 64)));
	};
	std::vector<Tri> tris;
	std::map<std::tuple<int, int, int>, std::vector<size_t>> cells;
	const ThreediLod &lod = base.lods[made.lod];
	std::vector<uint16_t> index;
	for (size_t s = 0; s < lod.strip_count; ++s) {
		if (!threedi_decode_strip_indices(lod, lod.strips[s], index)) continue;
		for (size_t t = 0; t + 2 < index.size(); t += 3) {
			Tri tri;
			tri.material = threedi_material_array_index_for_id(base, lod.strips[s].material_index);
			for (int k = 0; k < 3; ++k) {
				const float *p = lod.vertices.items[lod.strips[s].start_vertex + index[t + size_t(k)]].position;
				tri.corner[k] = threedi_build_to_mission(ThreediBuildVec3{p[0], p[1], p[2]});
			}
			// Counter-clockwise in model axes, the mirror of mission: its outward normal in mission axes.
			tri.normal = cross(sub(tri.corner[2], tri.corner[0]), sub(tri.corner[1], tri.corner[0]));
			const ThreediBuildVec3 m{(tri.corner[0].x + tri.corner[1].x + tri.corner[2].x) / 3,
			                         (tri.corner[0].y + tri.corner[1].y + tri.corner[2].y) / 3,
			                         (tri.corner[0].z + tri.corner[1].z + tri.corner[2].z) / 3};
			cells[key(m)].push_back(tris.size());
			tris.push_back(tri);
		}
	}
	std::vector<ThreediCollisionObjectRun> runs(base.collision->object_count);
	if (!threedi_collision_object_runs(base.collision, runs.data())) return;
	for (size_t o = 0; o < base.collision->object_count; ++o)
		for (int32_t f = 0; f < base.collision->objects[o].num_faces; ++f) {
			const size_t face = size_t(runs[o].face_start + f);
			const ThreediCollisionFace &cf = base.collision->faces[face];
			ThreediBuildVec3 corner[3];
			bool inside = true;
			for (int k = 0; k < 3 && inside; ++k) {
				const int32_t at = cf.vert_index[k];
				inside = at >= 0 && at < base.collision->objects[o].num_vertices;
				if (!inside) break;
				const float *q = base.collision->vertices[size_t(runs[o].vertex_start + at)].position;
				corner[k] = ThreediBuildVec3{q[0], q[1], q[2]};
			}
			if (!inside) continue;
			// The face's stored normal (its winding before the 8.8 grid), else its corners'.
			ThreediBuildVec3 normal = cross(sub(corner[1], corner[0]), sub(corner[2], corner[0]));
			const int32_t stored = runs[o].normal_start + cf.normal_index;
			if (cf.normal_index >= 0 && stored >= 0 && size_t(stored) < base.collision->normal_count) {
				const float *n = base.collision->normals[size_t(stored)].normal;
				normal = ThreediBuildVec3{n[0], n[1], n[2]};
			}
			const ThreediBuildVec3 m{(corner[0].x + corner[1].x + corner[2].x) / 3,
			                         (corner[0].y + corner[1].y + corner[2].y) / 3,
			                         (corner[0].z + corner[1].z + corner[2].z) / 3};
			const auto [cx, cy, cz] = key(m);
			std::set<int> own;
			for (int dx = -1; dx <= 1; ++dx)
				for (int dy = -1; dy <= 1; ++dy)
					for (int dz = -1; dz <= 1; ++dz) {
						const auto found = cells.find(std::make_tuple(cx + dx, cy + dy, cz + dz));
						if (found == cells.end()) continue;
						for (const size_t i : found->second) {
							const Tri &tri = tris[i];
							// Its corners truncated to the 8.8 grid are the face's, exactly (WriteCVRT's truncation).
							bool same = true;
							for (int k = 0; k < 3 && same; ++k) {
								bool any = false;
								for (int j = 0; j < 3 && !any; ++j)
									any = threedi_q8f_trunc(tri.corner[j].x) == corner[k].x &&
									      threedi_q8f_trunc(tri.corner[j].y) == corner[k].y &&
									      threedi_q8f_trunc(tri.corner[j].z) == corner[k].z;
								same = any;
							}
							const double along = double(tri.normal.x) * normal.x + double(tri.normal.y) * normal.y +
							                     double(tri.normal.z) * normal.z;
							if (same && along > 0) own.insert(tri.material);
						}
					}
			if (own.empty()) continue;
			++checked;
			if (own.count(made.material[face]) == 0) {
				if (first_wrong.empty())
					first_wrong = "face " + std::to_string(face) + " took material " +
					              std::to_string(made.material[face]) + ", its own " + std::to_string(*own.begin());
				++wrong;
			}
		}
}

int retail_surfaces() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every model's faces and their materials)");
	const ProjectDocument project;
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t models = 0, with_faces = 0, faces = 0, matched = 0, all_matched = 0;
	size_t materials = 0, mixed = 0, mixed_models = 0, differing = 0, flags_mixed = 0, ambiguous = 0, against = 0;
	std::map<std::string, size_t> examples;
	size_t on_own = 0, wrong = 0, on_own_wrong_models = 0;
	std::map<std::string, std::string> wrong_examples;
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
			ambiguous += made->ambiguous;
			against += made->against;
			{
				std::string first;
				const size_t before = wrong;
				faces_on_their_own_triangles(*document.model_row()->base, *made, on_own, wrong, first);
				if (wrong > before && wrong_examples.size() < 8) wrong_examples[file.logical_name] = first;
				on_own_wrong_models += wrong > before ? 1 : 0;
			}
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
	std::printf("retail surfaces: %zu faces had triangles of more than one material about them (their corners and winding chose); %zu "
	            "took a triangle wound against them\n",
	            ambiguous, against);
	std::printf("retail surfaces: %zu faces lie on a triangle of their own wound with them; %zu of them took "
	            "another's material (in %zu models)\n",
	            on_own, wrong, on_own_wrong_models);
	for (const auto &e : wrong_examples) std::printf("retail surfaces: %s: %s\n", e.first.c_str(), e.second.c_str());
	// Nearly every face is a triangle of a render LOD (a first-person gun's own collision mesh aside), and a
	// face on a triangle of its own takes that triangle's material (Dpuma1's faces 369 and 1323, a sheet
	// stored both ways, each its own side's).
	TEST_EXPECT(matched * 10 >= faces * 9);
	TEST_EXPECT(on_own * 10 >= faces * 9 && wrong == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (faces_find_their_materials() != 0) return 1;
	if (two_sided_sheet() != 0) return 1;
	if (set_on_a_material() != 0) return 1;
	if (words() != 0) return 1;
	return retail_surfaces();
}
