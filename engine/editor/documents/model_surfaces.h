#pragma once

// A model's bullet-face surfaces as a modder sets them: on its materials (ADR 0046 S17, Models).
//
// The file keeps a surface and flags on each bullet face (a CFAC record: poly_type and material_flags),
// never on a material; the faces are the triangles of one of the model's render LODs, the collision LOD,
// each made from a material's triangle (threedi_build.cpp, collision_section_points: "the bullet faces
// are that LOD's triangles"; docs/threedi/scene-naming-contract.md, Bullet faces: "Each face's surface
// type and flags come from its material"). So a material's surface is the surface its faces carry: one
// value where they agree, "mixed" where they do not (retail ships such models: the Blender add-on's
// import counts the faces that lose its vote). Setting a material's surface sets it on every face made
// from it, one batch, one undo step; what each face holds stays exact underneath, and a face is still
// set alone in the collision row (the advanced place).
//
// What a surface means is the game's: a round that hits a face plays its ammo's effects_table row
// `poly_type + 4` [orig: Projectile_HandleEntityImpact @ 0x4e9390, `ray[22] + 4` @ 0x4e982b], the rows
// named by the tag table [orig: g_AmmoEffectTagTable @ 0x813420; world::kImpactEffectTagNames], a tag
// past 27 played as row 4 [orig: AmmoDef_ProcessImpactEffect @ 0x40a170, the clamp @ 0x40a1bf].

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <editor/model/value.h>
#include <runtime/assets/asset_store.h>

namespace opennova::editor {

class ModelDocument;

// --- what a surface is -------------------------------------------------------------------------------

// The surfaces the game's tag table names: a face byte of 0 to 23 plays row 4 to 27.
inline constexpr int kModelSurfaceCount = 24;

struct ModelSurfaceWords {
	std::string name;  // a modder's word for it: "Metal", "Heavy metal", "Object"
	std::string tag;   // the effects_table row the game plays: "metal", "hmetal", "obj"
	std::string note;  // what else the game does with it, cited ("" for nothing more)
	bool known = true; // false past 23: the game plays the `obj` row
};
// The words of a face byte (any 0 to 255).
ModelSurfaceWords model_surface_words(int64_t poly_type);
// The surfaces a picker offers, by name, 0 to 23 (name: the tag; label: the words).
const std::vector<FieldChoice> &model_surface_choices();

// The bullet-face flag bits a modder sets per material (docs/threedi/o3d-scene-format.md `cf`): both
// sides (1), bullets pass (0x100), front only (0x800) [orig: Physics_RaycastAgainstBoneCollision @
// 0x4e4cb0: the 0x100 skip, the 0x800 test @ 0x4e5139].
struct ModelFaceFlag {
	uint32_t bit = 0;
	const char *token = "";
	const char *label = "";
	const char *tip = "";
};
const std::vector<ModelFaceFlag> &model_face_flags();

// --- which material made each face -------------------------------------------------------------------

// Each bullet face's material: the base material (its index in the parsed file's MTRL table) whose
// triangle of the collision LOD the face is, found by the triangle's middle (the collision corners sit on
// the 8.8 grid, so a face's middle lies within 1/256 m of its triangle's), and the LOD whose triangles
// meet the most faces. A face no triangle meets (a first-person gun's collision mesh of its own, which
// the file does not keep) has none (-1).
struct ModelFaceMaterials {
	std::vector<int> material; // per collision face, in the file's order (the collision row's faces)
	int lod = -1;              // the collision LOD, -1 when no LOD meets a face
	size_t matched = 0;        // faces with a material
};
// Made once per parsed file (the documents' immutable base) and kept while it lives.
std::shared_ptr<const ModelFaceMaterials> model_face_materials(const assets::Model &base);

// --- a material's surface ----------------------------------------------------------------------------

struct ModelSurfaceCount {
	int64_t surface = 0;
	size_t faces = 0;
};
struct ModelFlagCount {
	uint32_t bit = 0;
	size_t on = 0; // of the material's faces
};
// What a material's faces hold now (the document's collision row): how many, the surfaces they carry
// (the most first, a tie by the byte), each flag's count, and the faces (their collision indices) whose
// surface is not the most common one.
struct ModelMaterialSurface {
	size_t faces = 0;
	std::vector<ModelSurfaceCount> surfaces;
	std::vector<ModelFlagCount> flags; // model_face_flags' order
	std::vector<size_t> differing;
	bool mixed() const { return surfaces.size() > 1; }
	// The surface the most faces carry (0 with no face).
	int64_t common() const { return surfaces.empty() ? 0 : surfaces.front().surface; }
};

// The surface of the material at `material` in the model row's materials, false for a record that is
// no material of the document.
bool model_material_surface(const ModelDocument &document, NodeId material, ModelMaterialSurface &out);
// "Metal", "Mixed: 112 Metal, 3 Glass", "No bullet faces": the material's surface in words.
std::string model_material_surface_words(const ModelMaterialSurface &surface);
// How many of the model's faces no material made (the collision row's faces past the collision LOD's).
size_t model_faces_without_material(const ModelDocument &document);

// The edits that give every face of the material `surface` (a Set of each face's poly_type that holds
// another), one batch; false with `refusal` for a record that is no material, a material that made no
// face, or a value past a byte.
bool model_surface_edits(const ModelDocument &document, NodeId material, int64_t surface, std::vector<Edit> &out,
                         std::string &refusal);
// The edits that set (`on`) or clear one flag bit (model_face_flags) on every face of the material.
bool model_face_flag_edits(const ModelDocument &document, NodeId material, uint32_t bit, bool on, std::vector<Edit> &out,
                           std::string &refusal);
// The faces of a material whose surface is not its common one, as records of the collision row (what a
// Show the faces that differ selects).
std::vector<NodeAddress> model_differing_faces(const ModelDocument &document, NodeId material);
// The material a face was made from, as a record of the model row; {} for none.
NodeAddress model_face_material(const ModelDocument &document, NodeId face);

} // namespace opennova::editor
