// The model's display names (model_labels.h).

#include <editor/documents/model_labels.h>

#include <cstdio>
#include <set>

#include <base/io/strutil.h>
#include <editor/documents/model_collision_words.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_surfaces.h>
#include <formats/threedi/threedi_panm.h>

namespace opennova::editor {

using namespace threedi;

namespace {

constexpr NodeKind k(ModelKind kind) { return node_kind(kind); }

const ModelDocument *model_of(const Document &document) { return dynamic_cast<const ModelDocument *>(&document); }

size_t lod0_parts(const ModelRow &row) { return row.lods.empty() ? 0 : row.lods[0].lod.render_object_count; }

std::string two_digits(int64_t n) {
	char buf[24];
	std::snprintf(buf, sizeof(buf), "%02lld", static_cast<long long>(n));
	return buf;
}

// A part's name as the add-on lays it out: PN## on a rigid model, BN## on a skinned one's rig.
std::string part_word(const ModelRow &row, int64_t part) {
	return (row.header.mesh_type == THREEDI_MESH_SKINNED ? "BN" : "PN") + two_digits(part + 1);
}

// The index of `address` in its owner's list, and the owner; false for a record the document lacks.
bool place_of(const Document &document, const NodeAddress &address, Document::Placement &at) {
	return address.child && document.placement(address, at);
}

// The register a track or a generator byte names on the model, by its CTRL entry's name ("" for none).
std::string register_name(const ModelRow &row, int64_t index) {
	if (index < 0 || static_cast<size_t>(index) >= row.registers.size()) return "";
	return strutil::fixed_string(row.registers[static_cast<size_t>(index)].name, sizeof(row.registers[0].name));
}

// What a part animation row does, in words: spins, turns, faces the camera, slides, scales, and the
// registers its loaded tracks read; "still" for a row that moves nothing.
std::string panm_words(const ModelRow &row, const ThreediPartAnimation &pa) {
	std::vector<std::string> verbs;
	switch (threedi_panm_rotation_type(pa.flags)) {
	case 1: verbs.push_back("spins"); break;
	case 2: verbs.push_back("turns"); break;
	case 3: verbs.push_back("faces the camera"); break;
	case 4: verbs.push_back("faces the camera upright"); break;
	default: break;
	}
	if (threedi_panm_translate_type(pa.flags) != 0) verbs.push_back("slides");
	if (threedi_panm_scale_type(pa.flags) != 0) verbs.push_back("scales");
	std::set<std::string> registers;
	for (int t = 0; t < THREEDI_PANM_TRACK_COUNT; ++t) {
		const ThreediTransform &track = *threedi_panm_tracks(pa)[t];
		if (!threedi_panm_track_loaded(pa, t) || !threedi_generator_names_register(track.control)) continue;
		const std::string name = register_name(row, track.control_param);
		if (!name.empty()) registers.insert(name);
	}
	std::string out;
	for (size_t i = 0; i < verbs.size(); ++i) out += (i ? ", " : "") + verbs[i];
	if (out.empty()) out = "still";
	for (const std::string &r : registers) out += ", " + r;
	return out;
}

// A material by what a modder knows it by: its first texture row's file (a diffuse one first), then its
// shader tag.
std::string material_words(const ThreediMaterial &m) {
	const size_t rows = std::min<size_t>(m.texture_count, 24);
	const ThreediMaterialTexture *pick = nullptr;
	for (size_t i = 0; i < rows && !pick; ++i)
		if (m.textures[i].slot == THREEDI_TEX_SLOT_DIFFUSE && m.textures[i].name[0]) pick = &m.textures[i];
	for (size_t i = 0; i < rows && !pick; ++i)
		if (m.textures[i].name[0]) pick = &m.textures[i];
	const std::string shader = strutil::fixed_string(m.shader_name, sizeof(m.shader_name));
	if (!pick) return shader + " (no texture)";
	return strutil::fixed_string(pick->name, sizeof(pick->name)) + ", " + shader;
}

const char *slot_words(uint8_t slot) {
	switch (slot) {
	case THREEDI_TEX_SLOT_DIFFUSE: return "diffuse";
	case THREEDI_TEX_SLOT_DETAIL: return "detail";
	case THREEDI_TEX_SLOT_NORMAL: return "normal";
	case THREEDI_TEX_SLOT_NORMAL_B: return "second normal";
	default: return "slot";
	}
}

} // namespace

bool model_part_exists(const ModelRow &row, int64_t part) {
	return part >= 0 && static_cast<size_t>(part) < lod0_parts(row);
}

std::string model_part_name(const ModelRow &row, int64_t part) {
	if (part == -1 || part == 255) return "None";
	if (model_part_exists(row, part)) return part_word(row, part);
	return "No part " + std::to_string(part) + " (LOD 0 has " + std::to_string(lod0_parts(row)) + " part" +
	       (lod0_parts(row) == 1 ? "" : "s") + ")";
}

bool model_lod_drawn(const std::vector<int32_t> &thresholds, size_t lod) {
	// The walk advances while the radius is at or below a level's threshold: a level of 0 stops it, so no
	// level after one is reached [orig: Model_SelectRlodLevel @ 0x5c3b20, the walk @ 0x5c3b3b..0x5c3b5a].
	for (size_t i = 0; i < lod && i < thresholds.size(); ++i)
		if (thresholds[i] <= 0) return false;
	return lod < thresholds.size();
}

std::string model_lod_range(const std::vector<int32_t> &thresholds, size_t lod) {
	if (lod >= thresholds.size()) return "";
	if (!model_lod_drawn(thresholds, lod)) {
		size_t stop = 0;
		while (stop < lod && thresholds[stop] > 0) ++stop;
		return "never (LOD " + std::to_string(stop) + " draws down to 0 px)";
	}
	const int32_t own = thresholds[lod];
	if (lod == 0) return own > 0 ? "above " + std::to_string(own) + " px" : "at any size";
	const int32_t before = thresholds[lod - 1];
	if (own <= 0) return "below " + std::to_string(before) + " px";
	return std::to_string(own) + " to " + std::to_string(before) + " px";
}

std::vector<int32_t> model_lod_thresholds(const ModelRow &row) {
	std::vector<int32_t> out;
	for (const ModelLod &lod : row.lods) out.push_back(lod.lod.lod_threshold);
	return out;
}

bool model_lod_drawn(const ModelRow &row, size_t lod) { return model_lod_drawn(model_lod_thresholds(row), lod); }

std::string model_lod_range(const ModelRow &row, size_t lod) { return model_lod_range(model_lod_thresholds(row), lod); }
std::string model_user_point_role(const std::string &raw) {
	const std::string name = strutil::trim(raw);
	if (threedi_user_point_is_sitex(name)) return "passenger seat";
	if (strutil::starts_with_icase(name, "ctrlx")) return "control seat";
	if (strutil::starts_with_icase(name, "drvrx")) return "driver's seat";
	if (strutil::iequals(name, "UseGun")) return "gunner's seat";
	if (strutil::starts_with_icase(name, "flare")) return "flare launch point";
	if (strutil::starts_with_icase(name, "bullet01")) return "weapon muzzle (primary)";
	if (strutil::starts_with_icase(name, "bullet02")) return "weapon muzzle (primary and secondary)";
	if (strutil::starts_with_icase(name, "prim")) return "weapon muzzle (primary)";
	if (strutil::starts_with_icase(name, "sec")) return "weapon muzzle (secondary)";
	if (strutil::starts_with_icase(name, "agun")) return "gunner attachment";
	if (strutil::iequals(name, "TARGET")) return "weapons' aim origin";
	if (strutil::iequals(name, "LOOK")) return "line-of-sight origin";
	if (strutil::iequals(name, "CAMERA")) return "mounted gun's camera";
	if (strutil::iequals(name, "ground")) return "ground anchor";
	return "";
}

std::string model_record_label(const Document &document, const NodeAddress &address, const NameSource *) {
	const ModelDocument *model = model_of(document);
	const ModelRow *row = model ? model->model_row() : nullptr;
	if (!row) return "";
	if (!address.child) {
		if (address.kind == k(ModelKind::Collision)) return "Collision";
		return strutil::fixed_string(row->header.name, sizeof(row->header.name));
	}
	Document::Placement at;
	if (!place_of(document, address, at)) return "";
	const size_t i = at.index;
	switch (static_cast<ModelKind>(address.kind)) {
	case ModelKind::Lod: {
		if (i >= row->lods.size()) return "";
		const ModelLod &lod = row->lods[i];
		const size_t parts = lod.lod.render_object_count;
		return "LOD " + std::to_string(i) + ": " + model_lod_range(*row, i) + ", " +
		       (parts ? std::to_string(parts) + (parts == 1 ? " part" : " parts") : std::string("no parts"));
	}
	case ModelKind::PartAnimation: {
		Document::Placement lod_at;
		if (!place_of(document, at.owner, lod_at) || lod_at.index >= row->lods.size()) return "";
		const ModelLod &lod = row->lods[lod_at.index];
		if (i >= lod.panm.size()) return "";
		const ThreediPartAnimation &pa = lod.panm[i];
		return part_word(*row, pa.subobject_index) + ": " + panm_words(*row, pa);
	}
	case ModelKind::Material: {
		if (i >= row->materials.size()) return "";
		// Two materials of one texture and shader told apart by their place (1-based, as the lists number them).
		const std::string words = material_words(row->materials[i].material);
		for (size_t j = 0; j < row->materials.size(); ++j)
			if (j != i && material_words(row->materials[j].material) == words) return words + " #" + std::to_string(i + 1);
		return words;
	}
	case ModelKind::Texture: {
		Document::Placement material_at;
		if (!place_of(document, at.owner, material_at) || material_at.index >= row->materials.size()) return "";
		const ThreediMaterial &m = row->materials[material_at.index].material;
		if (i >= m.texture_count || i >= 24) return "";
		const ThreediMaterialTexture &t = m.textures[i];
		return (t.name[0] ? strutil::fixed_string(t.name, sizeof(t.name)) : std::string("(no file)")) + ", " +
		       slot_words(t.slot);
	}
	case ModelKind::Light: {
		if (i >= row->lights.size()) return "";
		const ThreediLight &l = row->lights[i];
		const bool spot = (l.flags & THREEDI_LIGHT_FLAG_TYPE_TARGET) != 0;
		char reach[32];
		std::snprintf(reach, sizeof(reach), "%.3g m", static_cast<double>(l.atten_end));
		// Part 0 is the unattached sentinel (ObjectModel::get_model_light_world_position's rule).
		return "Light " + std::to_string(i + 1) + ": " + (spot ? "spot" : "omni") + ", " + reach +
		       (l.subobj_index ? ", on " + model_part_name(*row, l.subobj_index) : std::string());
	}
	case ModelKind::UserPoint: {
		if (i >= row->user_points.size()) return "";
		const std::string name = strutil::fixed_string(row->user_points[i].name, sizeof(row->user_points[i].name));
		const std::string role = model_user_point_role(name);
		return role.empty() ? name : name + ": " + role;
	}
	case ModelKind::Register: {
		const std::string name = register_name(*row, int64_t(i));
		return std::to_string(i) + ": " + (name.empty() ? std::string("(no name)") : name);
	}
	case ModelKind::Frame: {
		// The parts whose rotation turns through this row (threedi_panm_frame_row, the pose's rule).
		// A frame byte of 0 names no row, so row 0 is never read [orig: Model_TransformBoneMatrices @ 0x58E3FE].
		std::string out = "Rotation frame " + std::to_string(i);
		if (i == 0) return out + " (never read: a frame byte of 0 names no row)";
		std::set<int> parts;
		for (const ModelLod &lod : row->lods)
			for (const ThreediPartAnimation &pa : lod.panm)
				if (threedi_panm_frame_row(pa) == static_cast<int>(i)) parts.insert(pa.subobject_index);
		if (parts.empty()) return out + " (no part turns through it)";
		out += ":";
		size_t n = 0;
		for (const int p : parts) out += (n++ ? ", " : " ") + part_word(*row, p);
		return out;
	}
	case ModelKind::Section: {
		// Section i is part i of the collision LOD (one section per part, WriteCOBJ;
		// docs/threedi/3di-gp-format-re.md, Retail JO corpus layout); a person's bone section is its hit
		// sphere (model_collision_words.h).
		const CollisionRow *collision = model->collision_row();
		if (!collision || i >= collision->sections.size()) return "";
		const ThreediCollisionObject &s = collision->sections[i];
		if (row->header.mesh_type == THREEDI_MESH_SKINNED && s.num_faces == 0 && s.num_bounding_volumes == 0)
			return "Section of " + part_word(*row, int64_t(i)) + ": hit sphere" + (i == 14 ? " (the head)" : "");
		return "Section of " + part_word(*row, int64_t(i)) + ": " + std::to_string(s.num_bounding_volumes) + " volumes, " +
		       std::to_string(s.num_faces) + " faces";
	}
	case ModelKind::Volume: {
		// A volume by what its type does in the game (model_collision_words.h), its code after.
		const CollisionRow *collision = model->collision_row();
		if (!collision || i >= collision->volumes.size()) return "";
		const ModelVolumeType &type = model_volume_type(collision->volumes[i].collidable_type);
		return "Volume " + std::to_string(i + 1) + ": " + type.words +
		       (type.code[0] ? std::string(" (") + type.code + ")"
		                     : " (type " + std::to_string(collision->volumes[i].collidable_type) + ")");
	}
	case ModelKind::Face: {
		const CollisionRow *collision = model->collision_row();
		if (!collision || i >= collision->faces.size()) return "";
		return "Face " + std::to_string(i + 1) + ": " + model_surface_words(collision->faces[i].poly_type).name;
	}
	case ModelKind::Occlusion: {
		const CollisionRow *collision = model->collision_row();
		if (!collision || i >= collision->occlusion.size()) return "";
		const uint8_t type = collision->occlusion[i].type;
		return "Occlusion " + std::to_string(i + 1) + ": " +
		       (type < 5 ? std::string(model_occlusion_type_words(type)) + (type == 4 ? " (no witnessed meaning)" : "")
		                 : "type " + std::to_string(type));
	}
	default: return "";
	}
}

bool model_value_label(const Document &document, const NodeAddress &address, const FieldUse &field, const Value &value,
                       const NameSource *, DisplayName &out) {
	const ModelDocument *model = model_of(document);
	const ModelRow *row = model ? model->model_row() : nullptr;
	const int64_t *n = std::get_if<int64_t>(&value);
	if (!row || !n || !field.schema) return false;
	const std::string &id = field.schema->id;
	const ModelKind kind = static_cast<ModelKind>(address.kind);
	// The fields that name a part of LOD 0 by its index (model_table.cpp's Ref::Part).
	const bool part = (kind == ModelKind::PartAnimation && id == "parent") || (kind == ModelKind::Light && id == "part") ||
	                  (kind == ModelKind::UserPoint && id == "part");
	if (part) {
		const int64_t parts = int64_t(lod0_parts(*row));
		// A light's part 0 is the unattached sentinel.
		if (kind == ModelKind::Light && *n == 0) out.text = "None (part 0: unattached)";
		// A user point's -1, and an index past LOD 0's parts, ride the root part; one at the part count or
		// another negative reads past the parts [orig: Userpoint_ComputeWorldTransform @ 0x56c420: the bound
		// @ 0x56c474..0x56c489, `jle` signed, so only -1 and an index above the count map to part 0].
		else if (kind == ModelKind::UserPoint && *n == -1) out.text = "None: it rides " + part_word(*row, 0) + ", the root";
		else if (kind == ModelKind::UserPoint && *n > parts)
			out.text = model_part_name(*row, *n) + ": the game puts it on " + part_word(*row, 0) + ", the root";
		else if (kind == ModelKind::UserPoint && !model_part_exists(*row, *n))
			out.text = model_part_name(*row, *n) + ": the game reads past its parts";
		else out.text = model_part_name(*row, *n);
		out.raw = std::to_string(*n);
		out.dangling = !(*n == -1 || *n == 255 || model_part_exists(*row, *n));
		return true;
	}
	if (kind == ModelKind::Face && id == "poly_type") {
		const ModelSurfaceWords words = model_surface_words(*n);
		out.text = words.name;
		out.raw = std::to_string(*n);
		out.source = "the ammo effects row " + words.tag;
		return true;
	}
	return false;
}

} // namespace opennova::editor
