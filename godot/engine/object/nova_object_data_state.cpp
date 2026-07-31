// NovaObjectData — edit-state snapshots (the editor's undo blob), the
// summary/accessor surface, and the project/LOD scalar fields.
#include "object/nova_object_data_internal.h"

#include <cstdlib>
#include <vector>

using namespace novaobj;

// --- Edit-state snapshots (B3) ----------------------------------------------
// In-process undo payload only — raw little-endian POD bytes behind a magic +
// version tag, never persisted. The blob carries exactly the OED-editable
// state; geometry stays outside, so apply() validates the geometry-fixed
// counts (materials/lights/LODs) and rejects a blob from a different model.

namespace {

constexpr uint32_t kEditStateMagic = 0x4E4F4453u; // 'NODS'
constexpr uint16_t kEditStateVersion = 1;

void edit_state_append(PackedByteArray &r_out, const void *p_data, size_t p_size) {
	const int64_t at = r_out.size();
	r_out.resize(at + static_cast<int64_t>(p_size));
	std::memcpy(r_out.ptrw() + at, p_data, p_size);
}

template <class T>
void edit_state_append_pod(PackedByteArray &r_out, const T &p_value) {
	edit_state_append(r_out, &p_value, sizeof(T));
}

bool edit_state_read(const PackedByteArray &p_in, int64_t &r_cursor, void *p_data, size_t p_size) {
	if (r_cursor < 0 || r_cursor + static_cast<int64_t>(p_size) > p_in.size()) {
		return false;
	}
	std::memcpy(p_data, p_in.ptr() + r_cursor, p_size);
	r_cursor += static_cast<int64_t>(p_size);
	return true;
}

template <class T>
bool edit_state_read_pod(const PackedByteArray &p_in, int64_t &r_cursor, T &r_value) {
	return edit_state_read(p_in, r_cursor, &r_value, sizeof(T));
}

// The TdpLod scalars the object editor edits (scene_file drives geometry and
// deliberately stays out — a snapshot never restores across a geometry swap).
struct EditStateLodScalars {
	int32_t attributes;
	char render_function[32];
	float threshold;
	int32_t part_anim_enabled;
};

} // namespace

PackedByteArray NovaObjectData::snapshot_edit_state() const {
	PackedByteArray out;
	if (!has_ir) {
		return out; // Empty blob = no document; the GDScript session stays inert.
	}
	edit_state_append_pod(out, kEditStateMagic);
	edit_state_append_pod(out, kEditStateVersion);
	edit_state_append_pod(out, static_cast<uint8_t>(source_kind));
	edit_state_append_pod(out, oed_dirty_mask);

	const CharString name_utf8 = object_name.utf8();
	const uint32_t name_len = static_cast<uint32_t>(name_utf8.length());
	edit_state_append_pod(out, name_len);
	edit_state_append(out, name_utf8.get_data(), name_len);

	const uint32_t material_count = static_cast<uint32_t>(ir.material_count);
	edit_state_append_pod(out, material_count);
	if (material_count > 0) {
		edit_state_append(out, ir.materials, material_count * sizeof(ThreediIRMaterial));
	}

	const uint32_t light_count = static_cast<uint32_t>(ir.light_count);
	edit_state_append_pod(out, light_count);
	if (light_count > 0) {
		edit_state_append(out, ir.lights, light_count * sizeof(ThreediIRLight));
	}

	const uint32_t lod_count = static_cast<uint32_t>(ir.lod_count);
	edit_state_append_pod(out, lod_count);
	for (uint32_t i = 0; i < lod_count; ++i) {
		const ThreediIRLod &lod = ir.lods[i];
		const uint32_t anim_count = static_cast<uint32_t>(lod.part_animation_count);
		edit_state_append_pod(out, anim_count);
		if (anim_count > 0) {
			edit_state_append(out, lod.part_animations, anim_count * sizeof(ThreediIRPartAnimation));
		}
	}

	const uint8_t has_project = has_source_project ? 1 : 0;
	edit_state_append_pod(out, has_project);
	if (has_project) {
		edit_state_append_pod(out, source_project.poly_collision_lod);
		for (int i = 0; i < TDP_MAX_LODS; ++i) {
			const TdpLod &lod = source_project.lods[i];
			EditStateLodScalars scalars = {};
			scalars.attributes = lod.attributes;
			std::memcpy(scalars.render_function, lod.render_function, sizeof(scalars.render_function));
			scalars.threshold = lod.threshold;
			scalars.part_anim_enabled = lod.part_anim_enabled;
			edit_state_append_pod(out, scalars);
		}
	}
	return out;
}

Error NovaObjectData::apply_edit_state(const PackedByteArray &p_bytes) {
	if (!has_ir) {
		return ERR_UNCONFIGURED;
	}
	int64_t cursor = 0;

	uint32_t magic = 0;
	uint16_t version = 0;
	uint8_t kind = 0;
	uint8_t snap_dirty_mask = 0;
	if (!edit_state_read_pod(p_bytes, cursor, magic) || magic != kEditStateMagic) {
		return ERR_INVALID_DATA;
	}
	if (!edit_state_read_pod(p_bytes, cursor, version) || version != kEditStateVersion) {
		return ERR_INVALID_DATA;
	}
	if (!edit_state_read_pod(p_bytes, cursor, kind) || kind != static_cast<uint8_t>(source_kind)) {
		return ERR_INVALID_DATA;
	}
	if (!edit_state_read_pod(p_bytes, cursor, snap_dirty_mask)) {
		return ERR_INVALID_DATA;
	}

	uint32_t name_len = 0;
	if (!edit_state_read_pod(p_bytes, cursor, name_len) || name_len > 4096) {
		return ERR_INVALID_DATA;
	}
	std::vector<char> name_bytes(static_cast<size_t>(name_len) + 1, '\0');
	if (name_len > 0 && !edit_state_read(p_bytes, cursor, name_bytes.data(), name_len)) {
		return ERR_INVALID_DATA;
	}

	// Two-phase: parse everything into temporaries and validate the
	// geometry-fixed counts BEFORE touching the document.
	uint32_t material_count = 0;
	if (!edit_state_read_pod(p_bytes, cursor, material_count) ||
			material_count != static_cast<uint32_t>(ir.material_count)) {
		return ERR_INVALID_DATA;
	}
	std::vector<ThreediIRMaterial> materials(material_count);
	if (material_count > 0 &&
			!edit_state_read(p_bytes, cursor, materials.data(), material_count * sizeof(ThreediIRMaterial))) {
		return ERR_INVALID_DATA;
	}

	uint32_t light_count = 0;
	if (!edit_state_read_pod(p_bytes, cursor, light_count) ||
			light_count != static_cast<uint32_t>(ir.light_count)) {
		return ERR_INVALID_DATA;
	}
	std::vector<ThreediIRLight> lights(light_count);
	if (light_count > 0 &&
			!edit_state_read(p_bytes, cursor, lights.data(), light_count * sizeof(ThreediIRLight))) {
		return ERR_INVALID_DATA;
	}

	uint32_t lod_count = 0;
	if (!edit_state_read_pod(p_bytes, cursor, lod_count) ||
			lod_count != static_cast<uint32_t>(ir.lod_count)) {
		return ERR_INVALID_DATA;
	}
	std::vector<std::vector<ThreediIRPartAnimation>> lod_anims(lod_count);
	for (uint32_t i = 0; i < lod_count; ++i) {
		uint32_t anim_count = 0;
		if (!edit_state_read_pod(p_bytes, cursor, anim_count) || anim_count > 4096) {
			return ERR_INVALID_DATA;
		}
		lod_anims[i].resize(anim_count);
		if (anim_count > 0 &&
				!edit_state_read(p_bytes, cursor, lod_anims[i].data(), anim_count * sizeof(ThreediIRPartAnimation))) {
			return ERR_INVALID_DATA;
		}
	}

	uint8_t has_project = 0;
	if (!edit_state_read_pod(p_bytes, cursor, has_project) ||
			(has_project != 0) != has_source_project) {
		return ERR_INVALID_DATA;
	}
	int32_t poly_collision = 0;
	std::vector<EditStateLodScalars> project_scalars;
	if (has_project) {
		if (!edit_state_read_pod(p_bytes, cursor, poly_collision)) {
			return ERR_INVALID_DATA;
		}
		project_scalars.resize(TDP_MAX_LODS);
		if (!edit_state_read(p_bytes, cursor, project_scalars.data(),
					project_scalars.size() * sizeof(EditStateLodScalars))) {
			return ERR_INVALID_DATA;
		}
	}
	if (cursor != p_bytes.size()) {
		return ERR_INVALID_DATA;
	}

	// Commit.
	object_name = String::utf8(name_bytes.data());
	if (material_count > 0) {
		std::memcpy(ir.materials, materials.data(), material_count * sizeof(ThreediIRMaterial));
	}
	if (light_count > 0) {
		std::memcpy(ir.lights, lights.data(), light_count * sizeof(ThreediIRLight));
	}
	for (uint32_t i = 0; i < lod_count; ++i) {
		ThreediIRLod &lod = ir.lods[i];
		const size_t anim_count = lod_anims[i].size();
		if (anim_count != lod.part_animation_count) {
			// Add/delete changed the count: realloc (same idiom as add_part_anim).
			ThreediIRPartAnimation *next = nullptr;
			if (anim_count > 0) {
				next = static_cast<ThreediIRPartAnimation *>(
						std::calloc(anim_count, sizeof(ThreediIRPartAnimation)));
				if (next == nullptr) {
					return ERR_OUT_OF_MEMORY;
				}
				std::memcpy(next, lod_anims[i].data(), anim_count * sizeof(ThreediIRPartAnimation));
			}
			std::free(lod.part_animations);
			lod.part_animations = next;
			lod.part_animation_count = anim_count;
		} else if (anim_count > 0) {
			std::memcpy(lod.part_animations, lod_anims[i].data(),
					anim_count * sizeof(ThreediIRPartAnimation));
		}
	}
	if (has_project) {
		source_project.poly_collision_lod = poly_collision;
		for (int i = 0; i < TDP_MAX_LODS; ++i) {
			TdpLod &lod = source_project.lods[i];
			const EditStateLodScalars &scalars = project_scalars[i];
			lod.attributes = scalars.attributes;
			std::memcpy(lod.render_function, scalars.render_function, sizeof(lod.render_function));
			lod.threshold = scalars.threshold;
			lod.part_anim_enabled = scalars.part_anim_enabled;
		}
	}

	_notify_object_changed(UPDATE_ALL);
	// Exact dirty restore AFTER the notify (which ORs UPDATE_ALL in): an undo
	// back to a just-saved state must show that state's export hints, while
	// last_oed_update_mask stays ALL so every consumer rebuilds.
	oed_dirty_mask = snap_dirty_mask;
	return OK;
}

bool NovaObjectData::has_document() const {
	return has_ir;
}

bool NovaObjectData::can_save_project() const {
	return has_ir;
}

bool NovaObjectData::can_export_3di() const {
	return has_ir && (has_source_model || oed_session != nullptr);
}

String NovaObjectData::get_source_path() const {
	return source_path;
}

String NovaObjectData::get_source_dir() const {
	return source_dir;
}

String NovaObjectData::get_object_name() const {
	return object_name;
}

String NovaObjectData::get_source_kind() const {
	return _source_kind_name();
}

String NovaObjectData::get_last_error() const {
	return last_error;
}

int NovaObjectData::get_oed_dirty_mask() const {
	return static_cast<int>(oed_dirty_mask & UPDATE_ALL);
}

int NovaObjectData::get_last_oed_update_mask() const {
	return static_cast<int>(last_oed_update_mask & UPDATE_ALL);
}

Dictionary NovaObjectData::get_summary() const {
	Dictionary result;
	result["name"] = object_name;
	result["source_kind"] = _source_kind_name();
	result["lod_count"] = static_cast<int64_t>(has_ir ? ir.lod_count : 0);
	result["material_count"] = static_cast<int64_t>(has_ir ? ir.material_count : 0);
	result["light_count"] = static_cast<int64_t>(has_ir ? ir.light_count : 0);
	result["userpoint_count"] = static_cast<int64_t>(has_ir ? ir.userpoint_count : 0);
	result["project_lod_count"] = static_cast<int64_t>(has_source_project ? project_lod_count(source_project) : 0);
	result["poly_collision_lod"] = static_cast<int64_t>(has_source_project ? source_project.poly_collision_lod : 0);
	result["oed_dirty_mask"] = static_cast<int64_t>(get_oed_dirty_mask());
	result["can_save_project"] = can_save_project();
	result["can_export_3di"] = can_export_3di();
	return result;
}

Array NovaObjectData::get_project_lods() const {
	Array result;
	if (!has_source_project) {
		return result;
	}
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		const TdpLod &lod = source_project.lods[i];
		if (lod.scene_file[0] == '\0') {
			break;
		}
		Dictionary item;
		item["index"] = i;
		item["scene_file"] = from_native(lod.scene_file);
		const std::string resolved_scene_path = resolve_relative_file(source_dir, lod.scene_file);
		item["scene_path"] = source_dir.is_empty() ? String() : from_native(resolved_scene_path.c_str());
		item["attributes"] = lod.attributes;
		item["render_function"] = from_native(lod.render_function);
		item["threshold"] = lod.threshold;
		item["part_anim_enabled"] = lod.part_anim_enabled != 0;
		item["part_anim_count"] = static_cast<int64_t>(lod.part_anim_count);
		item["light_count"] = static_cast<int64_t>(lod.light_count);
		result.push_back(item);
	}
	return result;
}

bool NovaObjectData::set_lod_field(int p_lod_index, const String &p_key, const Variant &p_value) {
	if (!has_source_project || p_lod_index < 0 || p_lod_index >= project_lod_count(source_project)) {
		return false;
	}
	TdpLod &lod = source_project.lods[p_lod_index];
	const String key = p_key.to_lower();
	if (key == "attributes") {
		lod.attributes = static_cast<int32_t>(p_value);
	} else if (key == "render_function") {
		copy_cstr(lod.render_function, sizeof(lod.render_function), to_std(String(p_value)).c_str());
	} else if (key == "threshold") {
		lod.threshold = static_cast<float>(p_value);
	} else {
		return false;
	}

	if (oed_session != nullptr) {
		return _build_ir_from_project_session(nullptr, UPDATE_ALL) == OK;
	}
	_notify_object_changed(UPDATE_ALL);
	return true;
}

bool NovaObjectData::set_project_field(const String &p_key, const Variant &p_value) {
	const String key = p_key.to_lower();
	if (key != "poly_collision_lod") {
		return false;
	}
	if (!has_source_project) {
		tdp_init(&source_project);
		has_source_project = true;
	}
	source_project.poly_collision_lod = std::clamp(static_cast<int32_t>(p_value), 0, TDP_MAX_LODS - 1);
	_notify_object_changed(UPDATE_ALL);
	return true;
}
