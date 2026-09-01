// ObjectData materials and textures: immutable MTRL inspection, runtime shader
// classification, and texture resolution through a resource root or loose
// source directory.
#include "object/object_data_internal.h"

#include <runtime/renderer/material_classify.h>
#include <runtime/renderer/material_descriptor.h>
#include <formats/threedi/threedi_ctrl_catalog.h>

#include "util/texture_path_resolver.h"

using namespace novaobj;

namespace {

// The MTRL texture-slot array capacity (ThreediMaterial::textures).
constexpr uint32_t kMaxMaterialTextures = 24;

const char *shader_blend_name(opennova::renderer::ObjectBlendMode blend) {
	switch (blend) {
		case opennova::renderer::ObjectBlendMode::Opaque: return "opaque";
		case opennova::renderer::ObjectBlendMode::AlphaBlend: return "alpha_blend";
		case opennova::renderer::ObjectBlendMode::Additive: return "additive";
		case opennova::renderer::ObjectBlendMode::Multiplicative: return "multiplicative";
	}
	return "opaque";
}

const char *shader_normal_space_name(opennova::renderer::ObjectNormalSpace normal_space) {
	switch (normal_space) {
		case opennova::renderer::ObjectNormalSpace::None: return "none";
		case opennova::renderer::ObjectNormalSpace::Tangent: return "tangent";
		case opennova::renderer::ObjectNormalSpace::Object: return "object";
	}
	return "none";
}

// Capability word for a shader tag: the descriptor table's shader_flags, or
// the first row (FF_ST_OP, DIFFUSE only) for an unknown tag, the same fallback
// the retail material-info registry gives an unregistered tag.
uint32_t shader_flags_for_tag(const char *shader_name) {
	if (const opennova::renderer::MaterialDescriptorRecord *descriptor =
			opennova::renderer::find_material_descriptor(shader_name != nullptr ? shader_name : "")) {
		return static_cast<uint32_t>(descriptor->shader_flags);
	}
	return static_cast<uint32_t>(opennova::renderer::kMaterialDescriptorTable[0].shader_flags);
}

// The boolean keys are the descriptor's capability word (shader_flags) and
// descriptor_flags, bit for bit: what the TAG says the shader can do, not the
// per-material runtime classification (which folds in MTRL overrides such as
// is_glass and the authored UV generators). Family, blend and normal space
// come from the runtime classification; they have no separate descriptor
// meaning.
void add_shader_classification_fields(Dictionary &item,
		const ThreediMaterial &material) {
	const opennova::renderer::MaterialDescriptorRecord *descriptor =
			opennova::renderer::find_material_descriptor(material.shader_name);
	const uint32_t flags = shader_flags_for_tag(material.shader_name);
	const uint32_t descriptor_flags = descriptor != nullptr ? descriptor->descriptor_flags : 0;
	const opennova::renderer::ObjectMaterialClassification classification =
			opennova::renderer::classify_object_material(material.shader_name,
					material.material_flags, material.emissive_type,
					material.is_glass, material.alpha_test_value_byte);
	item["shader_flags"] = static_cast<int64_t>(flags);
	item["has_diffuse"] = (flags & opennova::renderer::MATERIAL_FLAG_DIFFUSE) != 0;
	item["has_secondary"] = (flags & opennova::renderer::MATERIAL_FLAG_SECONDARY) != 0;
	item["has_normal_a"] = (flags & opennova::renderer::MATERIAL_FLAG_NORMAL_A) != 0;
	item["has_normal_b"] = (flags & opennova::renderer::MATERIAL_FLAG_NORMAL_B) != 0;
	item["is_alpha"] = (flags & opennova::renderer::MATERIAL_FLAG_ALPHA) != 0;
	// Self-lum keys on EMISSIVE; 0x10000000 is the separate glow/bloom-copy
	// capability (REN-4, D-RMAT-4 — the two ride together on FF _LUM rows but
	// FFP_GLASS carries only the capability).
	item["is_luminance"] = (flags & opennova::renderer::MATERIAL_FLAG_EMISSIVE) != 0;
	item["is_glow_capable"] = (flags & opennova::renderer::MATERIAL_FLAG_GLOW) != 0;
	item["is_glass_shader"] = (flags & opennova::renderer::MATERIAL_FLAG_GLASS) != 0;
	item["is_skinned_shader"] = (descriptor_flags & opennova::renderer::MATERIAL_DESCRIPTOR_SKINNED) != 0;
	item["is_blending_shader"] = (flags & opennova::renderer::MATERIAL_FLAG_BLENDING) != 0;
	item["uses_uv_generators"] = (descriptor_flags & opennova::renderer::MATERIAL_DESCRIPTOR_UV_TRANSFORM) != 0;
	item["uses_environment"] = (descriptor_flags & opennova::renderer::MATERIAL_DESCRIPTOR_ENVIRONMENT) != 0;
	item["uses_specular"] = (descriptor_flags & opennova::renderer::MATERIAL_DESCRIPTOR_SPECULAR) != 0;
	item["environment_textured"] =
			(descriptor_flags & opennova::renderer::MATERIAL_DESCRIPTOR_ENVIRONMENT_TEXTURED) != 0;
	item["uses_flag_animation"] = (descriptor_flags & opennova::renderer::MATERIAL_DESCRIPTOR_FLAG_ANIMATION) != 0;
	item["shader_family"] = from_native(
			opennova::renderer::object_shader_family_name(classification.family));
	item["shader_blend"] = from_native(shader_blend_name(classification.blend));
	item["normal_space"] = from_native(
			shader_normal_space_name(classification.normal_space));
}

Dictionary uv_params_to_dict(const ThreediUvParams &params) {
	Dictionary dict;
	dict["style"] = params.style;
	dict["phase"] = params.phase;
	dict["reg"] = params.reg;
	dict["rate"] = params.gen_rate;
	dict["start"] = params.start;
	dict["end"] = params.end;
	return dict;
}

Dictionary alpha_gen_to_dict(const ThreediAlphaGen &gen) {
	Dictionary dict;
	dict["style"] = gen.style;
	dict["phase"] = gen.phase;
	dict["reg"] = gen.reg;
	dict["rate"] = gen.rate;
	dict["start"] = gen.start;
	dict["end"] = gen.end;
	return dict;
}

Dictionary rgb_gen_to_dict(const ThreediRgbGen &gen) {
	Dictionary dict;
	dict["style"] = gen.style;
	dict["phase"] = gen.phase;
	dict["reg"] = gen.reg;
	dict["rate"] = gen.rate;
	dict["start_color"] = Color(gen.start_color[0], gen.start_color[1], gen.start_color[2], gen.start_color[3]);
	dict["end_color"] = Color(gen.end_color[0], gen.end_color[1], gen.end_color[2], gen.end_color[3]);
	return dict;
}

Dictionary texture_animation_to_dict(const ThreediTexAnim &anim) {
	Dictionary dict;
	dict["num_frames"] = anim.num_frames;
	dict["animation_type"] = anim.animation_type;
	dict["cycle_frame_time"] = anim.cycle_frame_time;
	return dict;
}

} // namespace

int ObjectData::get_material_count() const {
	return has_source_model ? static_cast<int>(source_model.material_count) : 0;
}

Array ObjectData::get_materials() const {
	Array result;
	if (!has_source_model) {
		return result;
	}
	for (size_t i = 0; i < source_model.material_count; ++i) {
		const ThreediMaterial &mat = source_model.materials[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["material_index"] = mat.index;
		item["shader"] = from_native(mat.shader_name);
		add_shader_classification_fields(item, mat);
		item["texture_count"] = mat.texture_count;
		item["alpha_threshold"] = static_cast<float>(mat.alpha_test_value_byte) / 255.0f;
		item["flags"] = static_cast<int64_t>(mat.material_flags);
		item["u_params"] = uv_params_to_dict(mat.u_params);
		item["v_params"] = uv_params_to_dict(mat.v_params);
		item["alpha_gen"] = alpha_gen_to_dict(mat.alpha_gen);
		item["rgb_gen"] = rgb_gen_to_dict(mat.rgb_gen);
		item["animation"] = texture_animation_to_dict(mat.animation);
		item["reflect_color"] = Color(mat.reflect_color[0], mat.reflect_color[1], mat.reflect_color[2], mat.reflect_color[3]);
		item["is_glass"] = mat.is_glass != 0;
		item["emissive_type"] = mat.emissive_type;
		Array textures;
		for (uint32_t t = 0; t < mat.texture_count && t < kMaxMaterialTextures; ++t) {
			Dictionary tex;
			tex["name"] = from_native(mat.textures[t].name);
			tex["slot"] = mat.textures[t].slot;
			tex["type"] = mat.textures[t].type;
			tex["flags"] = mat.textures[t].flags;
			tex["frame"] = mat.textures[t].frame;
			// Resolve through the resource root when mounted (so PFF-resident textures
			// report a path) and fall back to the loose source dir otherwise. Reuses the
			// same root-aware logic as the per-texture resolver below.
			tex["resolved_path"] = resolve_material_texture_path(static_cast<int>(i), static_cast<int>(t));
			textures.push_back(tex);
		}
		item["textures"] = textures;
		result.push_back(item);
	}
	return result;
}

Dictionary ObjectData::get_material_info(int p_index) const {
	Dictionary info;
	if (!has_source_model || p_index < 0 || static_cast<size_t>(p_index) >= source_model.material_count) {
		return info;
	}
	const ThreediMaterial &mat = source_model.materials[p_index];
	info["name"] = from_native(mat.shader_name);
	info["shader_tag"] = from_native(mat.shader_name);
	info["alpha_test"] = static_cast<int>(mat.alpha_test_value_byte);
	info["alpha_invert"] = (mat.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) != 0;
	info["two_sided"] = (mat.material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) != 0;
	info["alpha_test_enabled"] = (mat.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0;
	info["is_glass"] = mat.is_glass != 0;
	info["emissive"] = mat.emissive_type == THREEDI_EMISSIVE_FULL;
	info["diffuse_a"] = String();
	info["detail_a"] = String();
	info["normal_a"] = String();
	for (uint32_t i = 0; i < mat.texture_count && i < kMaxMaterialTextures; ++i) {
		const ThreediMaterialTexture &tex = mat.textures[i];
		if ((tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0 && tex.frame != 0) {
			continue;
		}
		if (tex.slot == THREEDI_TEX_SLOT_DIFFUSE && String(info["diffuse_a"]).is_empty()) {
			info["diffuse_a"] = from_native(tex.name);
		} else if (tex.slot == THREEDI_TEX_SLOT_DETAIL && String(info["detail_a"]).is_empty()) {
			info["detail_a"] = from_native(tex.name);
		} else if ((tex.slot == THREEDI_TEX_SLOT_NORMAL || tex.slot == THREEDI_TEX_SLOT_NORMAL_B) &&
				String(info["normal_a"]).is_empty()) {
			info["normal_a"] = from_native(tex.name);
		}
	}
	info["reflect_color"] = Color(mat.reflect_color[0], mat.reflect_color[1], mat.reflect_color[2], mat.reflect_color[3]);
	info["rgb_gen_style"] = static_cast<int>(mat.rgb_gen.style);
	info["rgb_gen_rate"] = mat.rgb_gen.rate;
	info["rgb_gen_phase"] = mat.rgb_gen.phase;
	info["rgb_gen_start_color"] = Color(mat.rgb_gen.start_color[0], mat.rgb_gen.start_color[1], mat.rgb_gen.start_color[2], mat.rgb_gen.start_color[3]);
	info["rgb_gen_end_color"] = Color(mat.rgb_gen.end_color[0], mat.rgb_gen.end_color[1], mat.rgb_gen.end_color[2], mat.rgb_gen.end_color[3]);
	info["rgb_gen_reg"] = mat.rgb_gen.reg;
	info["rgb_gen_reg_name"] = control_register_name_for(source_model, mat.rgb_gen.reg);
	info["alpha_gen_style"] = static_cast<int>(mat.alpha_gen.style);
	info["alpha_gen_rate"] = mat.alpha_gen.rate;
	info["alpha_gen_phase"] = mat.alpha_gen.phase;
	info["alpha_gen_start"] = static_cast<int>(mat.alpha_gen.start);
	info["alpha_gen_end"] = static_cast<int>(mat.alpha_gen.end);
	info["alpha_gen_reg"] = mat.alpha_gen.reg;
	info["alpha_gen_reg_name"] = control_register_name_for(source_model, mat.alpha_gen.reg);
	info["uv_u_style"] = static_cast<int>(mat.u_params.style);
	info["uv_u_rate"] = mat.u_params.gen_rate;
	info["uv_u_phase"] = mat.u_params.phase;
	info["uv_u_start"] = mat.u_params.start;
	info["uv_u_end"] = mat.u_params.end;
	info["uv_u_reg"] = mat.u_params.reg;
	info["uv_u_reg_name"] = control_register_name_for(source_model, mat.u_params.reg);
	info["uv_v_style"] = static_cast<int>(mat.v_params.style);
	info["uv_v_rate"] = mat.v_params.gen_rate;
	info["uv_v_phase"] = mat.v_params.phase;
	info["uv_v_start"] = mat.v_params.start;
	info["uv_v_end"] = mat.v_params.end;
	info["uv_v_reg"] = mat.v_params.reg;
	info["uv_v_reg_name"] = control_register_name_for(source_model, mat.v_params.reg);
	info["anim_frames"] = static_cast<int>(mat.animation.num_frames);
	info["anim_type"] = static_cast<int>(mat.animation.animation_type);
	info["anim_frame_time"] = static_cast<int>(mat.animation.cycle_frame_time);
	return info;
}

PackedStringArray ObjectData::get_material_anim_frames(int p_index, int p_slot) const {
	PackedStringArray out;
	if (!has_source_model || p_index < 0 || static_cast<size_t>(p_index) >= source_model.material_count) {
		return out;
	}
	const ThreediMaterial &mat = source_model.materials[p_index];
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
	// present_applier.cpp is the precedent).
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
	if (!has_source_model) {
		return result;
	}
	for (uint32_t i = 0; i < source_model.ctrl.count; ++i) {
		const char *authored_name = source_model.ctrl.registers[i].name;
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
	if (!has_source_model || p_material_index < 0 || static_cast<size_t>(p_material_index) >= source_model.material_count ||
			p_texture_index < 0 || p_texture_index >= static_cast<int>(kMaxMaterialTextures)) {
		return String();
	}

	const ThreediMaterial &material = source_model.materials[p_material_index];
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
	if (!has_source_model || p_material_index < 0 || static_cast<size_t>(p_material_index) >= source_model.material_count ||
			p_texture_index < 0 || p_texture_index >= static_cast<int>(kMaxMaterialTextures)) {
		return Ref<Texture2D>();
	}

	const ThreediMaterial &material = source_model.materials[p_material_index];
	if (static_cast<uint32_t>(p_texture_index) >= material.texture_count) {
		return Ref<Texture2D>();
	}

	const String texture_name = from_native(material.textures[p_texture_index].name);
	return resource_root.is_valid()
			? resource_root->load_texture(texture_name)
			: opennova::load_texture_from_dir(source_dir, texture_name);
}

Ref<Texture2D> ObjectData::load_texture_name(const String &p_texture_name) const {
	if (!has_source_model || p_texture_name.is_empty()) {
		return Ref<Texture2D>();
	}
	if (resource_root.is_valid()) {
		return resource_root->load_texture(p_texture_name);
	}
	return opennova::load_texture_from_dir(source_dir, p_texture_name);
}
