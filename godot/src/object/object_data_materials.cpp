// ObjectData materials and textures: immutable MTRL inspection, runtime shader
// classification, and texture resolution through a resource root or loose
// source directory.
#include "object/object_data_internal.h"
#include "object/material_info.h"

#include <formats/threedi/threedi_ctrl_catalog.h>

#include "util/texture_path_resolver.h"

using namespace novaobj;
using namespace opennova::threedi;

namespace {

// The MTRL texture-slot array capacity (ThreediMaterial::textures).
constexpr uint32_t kMaxMaterialTextures = 24;

} // namespace

int ObjectData::get_material_count() const {
	return source_model_ ? static_cast<int>(native_model().material_count) : 0;
}

int ObjectData::find_material_array_index(int p_material_index) const {
	if (!source_model_) {
		return -1;
	}
	for (size_t i = 0; i < native_model().material_count; ++i) {
		if (native_model().materials[i].index == p_material_index) {
			return static_cast<int>(i);
		}
	}
	if (p_material_index >= 0 && static_cast<size_t>(p_material_index) < native_model().material_count) {
		return p_material_index;
	}
	return -1;
}

Ref<Texture2D> ObjectData::load_material_slot_texture(int p_array_index, int p_slot) const {
	if (!source_model_ || p_array_index < 0 ||
			static_cast<size_t>(p_array_index) >= native_model().material_count) {
		return Ref<Texture2D>();
	}
	const ThreediMaterial &mat = native_model().materials[p_array_index];
	for (uint32_t i = 0; i < mat.texture_count && i < kMaxMaterialTextures; ++i) {
		if (static_cast<int>(mat.textures[i].slot) != p_slot) {
			continue;
		}
		const Ref<Texture2D> loaded = load_material_texture(p_array_index, static_cast<int>(i));
		if (loaded.is_valid()) {
			return loaded;
		}
	}
	return Ref<Texture2D>();
}

bool ObjectData::get_material_info(int p_index, MaterialInfo &r_info) const {
	if (!source_model_ || p_index < 0 || static_cast<size_t>(p_index) >= native_model().material_count) {
		return false;
	}
	const ThreediMaterial &mat = native_model().materials[p_index];
	MaterialInfo &info = r_info;
	info = MaterialInfo();
	info.name = from_native(mat.shader_name);
	info.shader_tag = from_native(mat.shader_name);
	info.alpha_test = static_cast<int>(mat.alpha_test_value_byte);
	info.alpha_invert = (mat.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0;
	info.two_sided = (mat.material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) != 0;
	info.alpha_test_enabled = (mat.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0;
	info.is_glass = mat.is_glass != 0;
	info.emissive = mat.emissive_type == THREEDI_EMISSIVE_FULL;
	// The first live (non-animated-frame) texture of each slot family.
	for (uint32_t i = 0; i < mat.texture_count && i < kMaxMaterialTextures; ++i) {
		const ThreediMaterialTexture &tex = mat.textures[i];
		if ((tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0 && tex.frame != 0) {
			continue;
		}
		if (tex.slot == THREEDI_TEX_SLOT_DIFFUSE && info.diffuse_a.is_empty()) {
			info.diffuse_a = from_native(tex.name);
		} else if (tex.slot == THREEDI_TEX_SLOT_DETAIL && info.detail_a.is_empty()) {
			info.detail_a = from_native(tex.name);
		} else if ((tex.slot == THREEDI_TEX_SLOT_NORMAL || tex.slot == THREEDI_TEX_SLOT_NORMAL_B) &&
				info.normal_a.is_empty()) {
			info.normal_a = from_native(tex.name);
		}
	}
	info.reflect_color = Color(mat.reflect_color[0], mat.reflect_color[1], mat.reflect_color[2], mat.reflect_color[3]);
	info.rgb_gen_style = static_cast<int>(mat.rgb_gen.style);
	info.rgb_gen_rate = mat.rgb_gen.rate;
	info.rgb_gen_phase = mat.rgb_gen.phase;
	info.rgb_gen_start_color = Color(mat.rgb_gen.start_color[0], mat.rgb_gen.start_color[1], mat.rgb_gen.start_color[2], mat.rgb_gen.start_color[3]);
	info.rgb_gen_end_color = Color(mat.rgb_gen.end_color[0], mat.rgb_gen.end_color[1], mat.rgb_gen.end_color[2], mat.rgb_gen.end_color[3]);
	info.rgb_gen_reg = mat.rgb_gen.reg;
	info.rgb_gen_reg_name = control_register_name_for(native_model(), mat.rgb_gen.reg);
	info.alpha_gen_style = static_cast<int>(mat.alpha_gen.style);
	info.alpha_gen_rate = mat.alpha_gen.rate;
	info.alpha_gen_phase = mat.alpha_gen.phase;
	info.alpha_gen_start = static_cast<int>(mat.alpha_gen.start);
	info.alpha_gen_end = static_cast<int>(mat.alpha_gen.end);
	info.alpha_gen_reg = mat.alpha_gen.reg;
	info.alpha_gen_reg_name = control_register_name_for(native_model(), mat.alpha_gen.reg);
	info.uv_u_style = static_cast<int>(mat.u_params.style);
	info.uv_u_rate = mat.u_params.gen_rate;
	info.uv_u_phase = mat.u_params.phase;
	info.uv_u_start = mat.u_params.start;
	info.uv_u_end = mat.u_params.end;
	info.uv_u_reg = mat.u_params.reg;
	info.uv_u_reg_name = control_register_name_for(native_model(), mat.u_params.reg);
	info.uv_v_style = static_cast<int>(mat.v_params.style);
	info.uv_v_rate = mat.v_params.gen_rate;
	info.uv_v_phase = mat.v_params.phase;
	info.uv_v_start = mat.v_params.start;
	info.uv_v_end = mat.v_params.end;
	info.uv_v_reg = mat.v_params.reg;
	info.uv_v_reg_name = control_register_name_for(native_model(), mat.v_params.reg);
	info.anim_frames = static_cast<int>(mat.animation.num_frames);
	info.anim_type = static_cast<int>(mat.animation.animation_type);
	info.anim_frame_time = static_cast<int>(mat.animation.cycle_frame_time);
	return true;
}

PackedStringArray ObjectData::get_material_anim_frames(int p_index, int p_slot) const {
	PackedStringArray out;
	if (!source_model_ || p_index < 0 || static_cast<size_t>(p_index) >= native_model().material_count) {
		return out;
	}
	const ThreediMaterial &mat = native_model().materials[p_index];
	const int frames = static_cast<int>(mat.animation.num_frames);
	if (frames <= 0) {
		return out;
	}
	out.resize(frames);
	for (int i = 0; i < frames; ++i) {
		out[i] = String();
	}
	for (uint32_t i = 0; i < mat.texture_count && i < kMaxMaterialTextures; ++i) {
		const ThreediMaterialTexture &tex = mat.textures[i];
		if (tex.slot == static_cast<uint8_t>(p_slot) &&
				(tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0 &&
				tex.frame < frames) {
			out[tex.frame] = from_native(tex.name);
		}
	}
	return out;
}

namespace {
// The register-name memo. Bounded: only names that resolve to a CTRL
// register are inserted (the register table is finite; a miss is looked up
// again, never cached, so mission/mod-fed strings cannot grow the map).
// Heap-owned so no godot::String outlives the extension: cleared from the
// module terminator (uninitialize_opennova_module -> clear_static_caches)
// before Godot's memory subsystem goes away. Main-thread callers only.
using RegisterNameCache = HashMap<String, String>;
RegisterNameCache *g_register_name_cache = nullptr;
} // namespace

void ObjectData::clear_static_caches() {
	if (g_register_name_cache != nullptr) {
		memdelete(g_register_name_cache);
		g_register_name_cache = nullptr;
	}
}

String ObjectData::canonical_control_register_name(const String &p_name) {
	// Memoized: the present pass resolves the same few names per row per
	// frame, and the utf8 round trip + canonical String rebuild are the
	// measured cost, not the ordinal lookup (the memoized infantry_keys_ in
	// entity_presenter.cpp is the precedent).
	if (g_register_name_cache == nullptr) {
		g_register_name_cache = memnew(RegisterNameCache);
	}
	if (const String *hit = g_register_name_cache->getptr(p_name)) {
		return *hit;
	}
	const CharString utf8 = p_name.utf8();
	const int ordinal = threedi_ctrl_register_ordinal(utf8.get_data());
	if (ordinal == THREEDI_CTRL_REGISTER_NOT_FOUND) {
		return String();
	}
	const String canonical =
			from_native(threedi_ctrl_register_name(static_cast<size_t>(ordinal)));
	g_register_name_cache->insert(p_name, canonical);
	return canonical;
}

Array ObjectData::get_control_registers() const {
	Array result;
	if (!source_model_) {
		return result;
	}
	for (uint32_t i = 0; i < native_model().ctrl.count; ++i) {
		const char *authored_name = native_model().ctrl.registers[i].name;
		const uint8_t runtime_ordinal =
				threedi_ctrl_register_loader_ordinal(authored_name);
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["name"] = from_native(authored_name);
		item["runtime_ordinal"] = static_cast<int64_t>(runtime_ordinal);
		item["runtime_name"] =
				from_native(threedi_ctrl_register_name(runtime_ordinal));
		// Keep the authored model-local spelling available to editors, but
		// expose the exact global slot selected by retail's loader fixup.
		// Unknown and empty names intentionally resolve to ordinal zero.
		// [orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640; ordinal store @ 0x5B46E6;
		//  CtrlName_ToOrdinal @ 0x57B290]
		result.push_back(item);
	}
	return result;
}

String ObjectData::resolve_material_texture_path(int p_material_index, int p_texture_index) const {
	if (!source_model_ || p_material_index < 0 || static_cast<size_t>(p_material_index) >= native_model().material_count ||
			p_texture_index < 0 || p_texture_index >= static_cast<int>(kMaxMaterialTextures)) {
		return String();
	}

	const ThreediMaterial &material = native_model().materials[p_material_index];
	if (static_cast<uint32_t>(p_texture_index) >= material.texture_count) {
		return String();
	}

	const String texture_name = from_native(material.textures[p_texture_index].name);
	if (resource_root.is_valid()) {
		const String resolved = resource_root->resolve_file(texture_name);
		return resolved.is_empty() && resource_root->load_texture(texture_name).is_valid() ? texture_name : resolved;
	}
	return opennova::resolve_texture_path(source_dir, texture_name);
}

Ref<Texture2D> ObjectData::load_material_texture(int p_material_index, int p_texture_index) const {
	if (!source_model_ || p_material_index < 0 || static_cast<size_t>(p_material_index) >= native_model().material_count ||
			p_texture_index < 0 || p_texture_index >= static_cast<int>(kMaxMaterialTextures)) {
		return Ref<Texture2D>();
	}

	const ThreediMaterial &material = native_model().materials[p_material_index];
	if (static_cast<uint32_t>(p_texture_index) >= material.texture_count) {
		return Ref<Texture2D>();
	}

	const auto &row = material.textures[p_texture_index];
	const String texture_name = from_native(row.name);
	return resource_root.is_valid()
			? resource_root->load_material_texture(texture_name, row.type)
			: opennova::load_material_texture_from_dir(source_dir, texture_name, row.type);
}

Ref<Texture2D> ObjectData::load_texture_name(const String &p_texture_name) const {
	if (!source_model_ || p_texture_name.is_empty()) {
		return Ref<Texture2D>();
	}
	if (resource_root.is_valid()) {
		return resource_root->load_texture(p_texture_name);
	}
	return opennova::load_texture_from_dir(source_dir, p_texture_name);
}
