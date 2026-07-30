// NovaObjectData — materials & textures: IR material introspection and
// editing, the shader catalog, and texture resolution through the resource
// root / loose source directory.
#include "object/nova_object_data_internal.h"

#include <oed/material_descriptor.h>
#include <threedi/threedi_ctrl_catalog.h>

#include "util/texture_path_resolver.h"

using namespace novaobj;

namespace {

float dict_float(const Dictionary &dict, const char *key, float fallback) {
	const String dict_key(key);
	if (!dict.has(dict_key)) {
		return fallback;
	}
	return static_cast<float>(dict[dict_key]);
}

int dict_int(const Dictionary &dict, const char *key, int fallback) {
	const String dict_key(key);
	if (!dict.has(dict_key)) {
		return fallback;
	}
	return static_cast<int>(dict[dict_key]);
}

Color dict_color(const Dictionary &dict, const char *key, const Color &fallback) {
	const String dict_key(key);
	if (!dict.has(dict_key)) {
		return fallback;
	}
	return dict[dict_key];
}

uint32_t shader_flags_for_tag(const char *shader_name) {
	if (const oed::MaterialDescriptorRecord *descriptor =
			oed::find_material_descriptor(shader_name != nullptr ? shader_name : "")) {
		return static_cast<uint32_t>(descriptor->shader_flags);
	}
	return static_cast<uint32_t>(oed::kMaterialInfoTable[0].flags);
}

const char *shader_family_name(oed::MaterialDescriptorFamily family) {
	switch (family) {
		case oed::MaterialDescriptorFamily::Unknown: return "unknown";
		case oed::MaterialDescriptorFamily::FixedFunction: return "fixed_function";
		case oed::MaterialDescriptorFamily::Phong: return "phong";
		case oed::MaterialDescriptorFamily::Flag: return "flag";
		case oed::MaterialDescriptorFamily::Dot3: return "dot3";
		case oed::MaterialDescriptorFamily::Environment: return "environment";
		case oed::MaterialDescriptorFamily::Glass: return "glass";
	}
	return "unknown";
}

const char *shader_blend_name(oed::MaterialDescriptorBlend blend) {
	switch (blend) {
		case oed::MaterialDescriptorBlend::Opaque: return "opaque";
		case oed::MaterialDescriptorBlend::AlphaBlend: return "alpha_blend";
		case oed::MaterialDescriptorBlend::Additive: return "additive";
		case oed::MaterialDescriptorBlend::Multiplicative: return "multiplicative";
	}
	return "opaque";
}

const char *shader_normal_space_name(oed::MaterialDescriptorNormalSpace normal_space) {
	switch (normal_space) {
		case oed::MaterialDescriptorNormalSpace::None: return "none";
		case oed::MaterialDescriptorNormalSpace::Tangent: return "tangent";
		case oed::MaterialDescriptorNormalSpace::Object: return "object";
	}
	return "none";
}

void add_shader_flag_fields(Dictionary &item, const char *shader_name, uint32_t flags) {
	const oed::MaterialDescriptorRecord *descriptor =
		oed::find_material_descriptor(shader_name != nullptr ? shader_name : "");
	const uint32_t descriptor_flags = descriptor != nullptr ? descriptor->descriptor_flags : 0;
	item["shader_flags"] = static_cast<int64_t>(flags);
	item["has_diffuse"] = (flags & oed::MATERIAL_FLAG_DIFFUSE) != 0;
	item["has_secondary"] = (flags & oed::MATERIAL_FLAG_SECONDARY) != 0;
	item["has_normal_a"] = (flags & oed::MATERIAL_FLAG_NORMAL_A) != 0;
	item["has_normal_b"] = (flags & oed::MATERIAL_FLAG_NORMAL_B) != 0;
	item["is_alpha"] = (flags & oed::MATERIAL_FLAG_ALPHA) != 0;
	// Self-lum keys on EMISSIVE; 0x10000000 is the separate glow/bloom-copy
	// capability (REN-4, D-RMAT-4 — the two ride together on FF _LUM rows but
	// FFP_GLASS carries only the capability).
	item["is_luminance"] = (flags & oed::MATERIAL_FLAG_EMISSIVE) != 0;
	item["is_glow_capable"] = (flags & oed::MATERIAL_FLAG_GLOW) != 0;
	item["is_glass_shader"] = (flags & oed::MATERIAL_FLAG_GLASS) != 0;
	item["is_skinned_shader"] = (descriptor_flags & oed::MATERIAL_DESCRIPTOR_SKINNED) != 0;
	item["is_blending_shader"] = (flags & oed::MATERIAL_FLAG_BLENDING) != 0;
	item["uses_uv_generators"] = (descriptor_flags & oed::MATERIAL_DESCRIPTOR_UV_TRANSFORM) != 0;
	item["uses_environment"] = (descriptor_flags & oed::MATERIAL_DESCRIPTOR_ENVIRONMENT) != 0;
	item["uses_specular"] = (descriptor_flags & oed::MATERIAL_DESCRIPTOR_SPECULAR) != 0;
	item["uses_flag_animation"] = (descriptor_flags & oed::MATERIAL_DESCRIPTOR_FLAG_ANIMATION) != 0;
	item["shader_family"] = descriptor != nullptr ? from_native(shader_family_name(descriptor->family)) : String("unknown");
	item["shader_blend"] = descriptor != nullptr ? from_native(shader_blend_name(descriptor->blend)) : String("opaque");
	item["normal_space"] = descriptor != nullptr ? from_native(shader_normal_space_name(descriptor->normal_space)) : String("none");
}

Dictionary uv_params_to_dict(const ThreediIRUvParams &params) {
	Dictionary dict;
	dict["style"] = params.style;
	dict["phase"] = params.phase;
	dict["reg"] = params.reg;
	dict["rate"] = params.gen_rate;
	dict["start"] = params.start;
	dict["end"] = params.end;
	return dict;
}

Dictionary alpha_gen_to_dict(const ThreediIRAlphaGen &gen) {
	Dictionary dict;
	dict["style"] = gen.style;
	dict["phase"] = gen.phase;
	dict["reg"] = gen.reg;
	dict["rate"] = gen.rate;
	dict["start"] = gen.start;
	dict["end"] = gen.end;
	return dict;
}

Dictionary rgb_gen_to_dict(const ThreediIRRgbGen &gen) {
	Dictionary dict;
	dict["style"] = gen.style;
	dict["phase"] = gen.phase;
	dict["reg"] = gen.reg;
	dict["rate"] = gen.rate;
	dict["start_color"] = Color(gen.start_color[0], gen.start_color[1], gen.start_color[2], gen.start_color[3]);
	dict["end_color"] = Color(gen.end_color[0], gen.end_color[1], gen.end_color[2], gen.end_color[3]);
	return dict;
}

Dictionary texture_animation_to_dict(const ThreediIRTexAnim &anim) {
	Dictionary dict;
	dict["num_frames"] = anim.num_frames;
	dict["animation_type"] = anim.animation_type;
	dict["cycle_frame_time"] = anim.cycle_frame_time;
	return dict;
}

void apply_uv_params(ThreediIRUvParams &dst, const Dictionary &params) {
	dst.style = static_cast<uint8_t>(std::clamp(dict_int(params, "style", dst.style), 0, 255));
	dst.phase = dict_float(params, "phase", dst.phase);
	dst.reg = dict_int(params, "reg", dst.reg);
	dst.gen_rate = dict_float(params, "rate", dst.gen_rate);
	dst.start = dict_float(params, "start", dst.start);
	dst.end = dict_float(params, "end", dst.end);
}

void apply_alpha_gen(ThreediIRAlphaGen &dst, const Dictionary &params) {
	dst.style = static_cast<uint8_t>(std::clamp(dict_int(params, "style", dst.style), 0, 255));
	dst.phase = dict_float(params, "phase", dst.phase);
	dst.reg = dict_int(params, "reg", dst.reg);
	dst.rate = dict_float(params, "rate", dst.rate);
	dst.start = clamp_to_i16(dict_int(params, "start", dst.start));
	dst.end = clamp_to_i16(dict_int(params, "end", dst.end));
}

void apply_rgb_gen(ThreediIRRgbGen &dst, const Dictionary &params) {
	dst.style = static_cast<uint8_t>(std::clamp(dict_int(params, "style", dst.style), 0, 255));
	dst.phase = dict_float(params, "phase", dst.phase);
	dst.reg = dict_int(params, "reg", dst.reg);
	dst.rate = dict_float(params, "rate", dst.rate);
	const Color start = dict_color(params, "start_color",
			Color(dst.start_color[0], dst.start_color[1], dst.start_color[2], dst.start_color[3]));
	const Color end = dict_color(params, "end_color",
			Color(dst.end_color[0], dst.end_color[1], dst.end_color[2], dst.end_color[3]));
	dst.start_color[0] = start.r;
	dst.start_color[1] = start.g;
	dst.start_color[2] = start.b;
	dst.start_color[3] = start.a;
	dst.end_color[0] = end.r;
	dst.end_color[1] = end.g;
	dst.end_color[2] = end.b;
	dst.end_color[3] = end.a;
}

void set_ir_texture_slot(ThreediIRMaterial &mat, int slot, int frame, const String &value, uint8_t flags) {
	const String filename = String(value).get_file();
	for (uint32_t i = 0; i < mat.texture_count && i < 8; ++i) {
		ThreediIRMaterialTexture &tex = mat.textures[i];
		if (tex.slot == static_cast<uint8_t>(slot) && tex.frame == static_cast<uint8_t>(frame) &&
				((flags & THREEDI_TEX_FLAG_ANIMATED) == 0 || (tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0)) {
			if (filename.is_empty()) {
				if (i + 1 < mat.texture_count) {
					mat.textures[i] = mat.textures[mat.texture_count - 1];
				}
				std::memset(&mat.textures[mat.texture_count - 1], 0, sizeof(ThreediIRMaterialTexture));
				--mat.texture_count;
			} else {
				copy_cstr(tex.name, sizeof(tex.name), to_std(filename).c_str());
				tex.slot = static_cast<uint8_t>(slot);
				tex.frame = static_cast<uint8_t>(frame);
				tex.flags |= flags;
			}
			return;
		}
	}

	if (filename.is_empty() || mat.texture_count >= 8) {
		return;
	}
	ThreediIRMaterialTexture &tex = mat.textures[mat.texture_count++];
	std::memset(&tex, 0, sizeof(tex));
	copy_cstr(tex.name, sizeof(tex.name), to_std(filename).c_str());
	tex.slot = static_cast<uint8_t>(slot);
	tex.type = (slot == THREEDI_IR_TEX_SLOT_NORMAL || slot == THREEDI_IR_TEX_SLOT_NORMAL_B) ? 4 : 0;
	tex.flags = flags;
	tex.frame = static_cast<uint8_t>(frame);
}

} // namespace

int NovaObjectData::get_material_count() const {
	return has_ir ? static_cast<int>(ir.material_count) : 0;
}

Array NovaObjectData::get_materials() const {
	Array result;
	if (!has_ir) {
		return result;
	}
	for (size_t i = 0; i < ir.material_count; ++i) {
		const ThreediIRMaterial &mat = ir.materials[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["material_index"] = mat.index;
		item["shader"] = from_native(mat.shader_name);
		add_shader_flag_fields(item, mat.shader_name, shader_flags_for_tag(mat.shader_name));
		item["texture_count"] = mat.texture_count;
		item["alpha_threshold"] = mat.alpha_threshold;
		item["flags"] = static_cast<int64_t>(mat.flags);
		item["blend_mode"] = static_cast<int64_t>(mat.blend_mode);
		item["u_params"] = uv_params_to_dict(mat.u_params);
		item["v_params"] = uv_params_to_dict(mat.v_params);
		item["alpha_gen"] = alpha_gen_to_dict(mat.alpha_gen);
		item["rgb_gen"] = rgb_gen_to_dict(mat.rgb_gen);
		item["animation"] = texture_animation_to_dict(mat.animation);
		item["reflect_color"] = Color(mat.reflect_color[0], mat.reflect_color[1], mat.reflect_color[2], mat.reflect_color[3]);
		item["is_glass"] = mat.is_glass != 0;
		item["emissive_type"] = mat.emissive_type;
		item["emissive_color"] = static_cast<int64_t>(mat.emissive_color);
		item["specular_intensity"] = static_cast<int64_t>(mat.specular_intensity);
		item["luminosity"] = static_cast<int64_t>(mat.luminosity);
		item["u_tiling"] = mat.u_tiling;
		item["v_tiling"] = mat.v_tiling;
		item["surface_type"] = mat.surface_type;
		item["pattrib"] = static_cast<int64_t>(mat.pattrib);
		Array textures;
		for (uint32_t t = 0; t < mat.texture_count && t < 8; ++t) {
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

Dictionary NovaObjectData::get_material_info(int p_index) const {
	Dictionary info;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return info;
	}
	const ThreediIRMaterial &mat = ir.materials[p_index];
	info["name"] = from_native(mat.shader_name);
	info["shader_tag"] = from_native(mat.shader_name);
	info["alpha_test"] = to_u8_color(mat.alpha_threshold);
	info["alpha_invert"] = (mat.flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT) != 0;
	info["two_sided"] = (mat.flags & THREEDI_IR_MATERIAL_FLAG_TWO_SIDED) != 0;
	info["alpha_test_enabled"] = (mat.flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST) != 0;
	info["is_glass"] = mat.is_glass != 0;
	info["emissive"] = mat.emissive_type == THREEDI_EMISSIVE_FULL || mat.emissive_type == 2;
	info["diffuse_a"] = String();
	info["detail_a"] = String();
	info["normal_a"] = String();
	for (uint32_t i = 0; i < mat.texture_count && i < 8; ++i) {
		const ThreediIRMaterialTexture &tex = mat.textures[i];
		if ((tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0 && tex.frame != 0) {
			continue;
		}
		if (tex.slot == THREEDI_IR_TEX_SLOT_DIFFUSE && String(info["diffuse_a"]).is_empty()) {
			info["diffuse_a"] = from_native(tex.name);
		} else if (tex.slot == THREEDI_IR_TEX_SLOT_DETAIL && String(info["detail_a"]).is_empty()) {
			info["detail_a"] = from_native(tex.name);
		} else if ((tex.slot == THREEDI_IR_TEX_SLOT_NORMAL || tex.slot == THREEDI_IR_TEX_SLOT_NORMAL_B) &&
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
	info["rgb_gen_reg_name"] = control_register_name_for(ir, mat.rgb_gen.reg);
	info["alpha_gen_style"] = static_cast<int>(mat.alpha_gen.style);
	info["alpha_gen_rate"] = mat.alpha_gen.rate;
	info["alpha_gen_phase"] = mat.alpha_gen.phase;
	info["alpha_gen_start"] = static_cast<int>(mat.alpha_gen.start);
	info["alpha_gen_end"] = static_cast<int>(mat.alpha_gen.end);
	info["alpha_gen_reg"] = mat.alpha_gen.reg;
	info["alpha_gen_reg_name"] = control_register_name_for(ir, mat.alpha_gen.reg);
	info["uv_u_style"] = static_cast<int>(mat.u_params.style);
	info["uv_u_rate"] = mat.u_params.gen_rate;
	info["uv_u_phase"] = mat.u_params.phase;
	info["uv_u_start"] = mat.u_params.start;
	info["uv_u_end"] = mat.u_params.end;
	info["uv_u_reg"] = mat.u_params.reg;
	info["uv_u_reg_name"] = control_register_name_for(ir, mat.u_params.reg);
	info["uv_v_style"] = static_cast<int>(mat.v_params.style);
	info["uv_v_rate"] = mat.v_params.gen_rate;
	info["uv_v_phase"] = mat.v_params.phase;
	info["uv_v_start"] = mat.v_params.start;
	info["uv_v_end"] = mat.v_params.end;
	info["uv_v_reg"] = mat.v_params.reg;
	info["uv_v_reg_name"] = control_register_name_for(ir, mat.v_params.reg);
	info["anim_frames"] = static_cast<int>(mat.animation.num_frames);
	info["anim_type"] = static_cast<int>(mat.animation.animation_type);
	info["anim_frame_time"] = static_cast<int>(mat.animation.cycle_frame_time);
	return info;
}

bool NovaObjectData::set_material_field(int p_index, const String &p_key, const Variant &p_value) {
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return false;
	}
	ThreediIRMaterial &mat = ir.materials[p_index];
	const String key = p_key;

	auto set_flag = [&](uint32_t flag) {
		if (static_cast<bool>(p_value)) {
			mat.flags |= flag;
		} else {
			mat.flags &= ~flag;
		}
	};
	auto set_color = [&](float out[4]) {
		const Color c = p_value;
		out[0] = c.r;
		out[1] = c.g;
		out[2] = c.b;
		out[3] = c.a;
	};
	auto resolve_reg = [&](const String &name, int32_t &reg) -> bool {
		return resolve_control_register_index(ir, name, reg);
	};

	if (key == "shader_tag" || key == "name") {
		copy_cstr(mat.shader_name, sizeof(mat.shader_name), to_std(String(p_value)).c_str());
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "alpha_test") {
		mat.alpha_threshold = std::clamp(static_cast<float>(static_cast<int>(p_value)) / 255.0f, 0.0f, 1.0f);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "alpha_test_enabled") {
		set_flag(THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "alpha_invert") {
		set_flag(THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "two_sided") {
		set_flag(THREEDI_IR_MATERIAL_FLAG_TWO_SIDED);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "is_glass") {
		mat.is_glass = static_cast<bool>(p_value) ? 1 : 0;
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "emissive") {
		mat.emissive_type = static_cast<bool>(p_value) ? 2 : 0;
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "diffuse_a") {
		set_ir_texture_slot(mat, THREEDI_IR_TEX_SLOT_DIFFUSE, 0, p_value, 0);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "detail_a") {
		set_ir_texture_slot(mat, THREEDI_IR_TEX_SLOT_DETAIL, 0, p_value, 0);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "normal_a") {
		set_ir_texture_slot(mat, THREEDI_IR_TEX_SLOT_NORMAL, 0, p_value, 0);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "reflect_color") {
		set_color(mat.reflect_color);
		_notify_object_changed(UPDATE_MTRL);
		return true;
	}
	if (key == "rgb_gen_style") { mat.rgb_gen.style = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "rgb_gen_rate") { mat.rgb_gen.rate = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "rgb_gen_phase") { mat.rgb_gen.phase = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "rgb_gen_start_color") { set_color(mat.rgb_gen.start_color); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "rgb_gen_end_color") { set_color(mat.rgb_gen.end_color); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "rgb_gen_reg") { mat.rgb_gen.reg = static_cast<int32_t>(static_cast<int>(p_value)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "rgb_gen_reg_name") { if (!resolve_reg(String(p_value), mat.rgb_gen.reg)) return false; _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_style") { mat.alpha_gen.style = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_rate") { mat.alpha_gen.rate = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_phase") { mat.alpha_gen.phase = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_start") { mat.alpha_gen.start = clamp_to_i16(static_cast<int>(p_value)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_end") { mat.alpha_gen.end = clamp_to_i16(static_cast<int>(p_value)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_reg") { mat.alpha_gen.reg = static_cast<int32_t>(static_cast<int>(p_value)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_reg_name") { if (!resolve_reg(String(p_value), mat.alpha_gen.reg)) return false; _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_u_style") { mat.u_params.style = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_u_rate") { mat.u_params.gen_rate = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_u_phase") { mat.u_params.phase = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_u_start") { mat.u_params.start = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_u_end") { mat.u_params.end = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_u_reg") { mat.u_params.reg = static_cast<int32_t>(static_cast<int>(p_value)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_u_reg_name") { if (!resolve_reg(String(p_value), mat.u_params.reg)) return false; _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_v_style") { mat.v_params.style = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_v_rate") { mat.v_params.gen_rate = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_v_phase") { mat.v_params.phase = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_v_start") { mat.v_params.start = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_v_end") { mat.v_params.end = static_cast<float>(p_value); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_v_reg") { mat.v_params.reg = static_cast<int32_t>(static_cast<int>(p_value)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "uv_v_reg_name") { if (!resolve_reg(String(p_value), mat.v_params.reg)) return false; _notify_object_changed(UPDATE_MTRL); return true; }
	return false;
}

int NovaObjectData::get_material_shader_flags(int p_index) const {
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return 0;
	}
	return static_cast<int>(shader_flags_for_tag(ir.materials[p_index].shader_name));
}

PackedStringArray NovaObjectData::get_material_anim_frames(int p_index, int p_slot) const {
	PackedStringArray out;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return out;
	}
	const ThreediIRMaterial &mat = ir.materials[p_index];
	const int frames = static_cast<int>(mat.animation.num_frames);
	if (frames <= 0) {
		return out;
	}
	out.resize(frames);
	for (int i = 0; i < frames; ++i) {
		out[i] = String();
	}
	for (uint32_t i = 0; i < mat.texture_count && i < 8; ++i) {
		const ThreediIRMaterialTexture &tex = mat.textures[i];
		if (tex.slot == static_cast<uint8_t>(p_slot) &&
				(tex.flags & THREEDI_TEX_FLAG_ANIMATED) != 0 &&
				tex.frame < frames) {
			out[tex.frame] = from_native(tex.name);
		}
	}
	return out;
}

bool NovaObjectData::set_material_anim_frame(int p_index, int p_slot, int p_frame_idx, const String &p_path) {
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return false;
	}
	ThreediIRMaterial &mat = ir.materials[p_index];
	if (p_frame_idx < 0 || p_frame_idx >= static_cast<int>(mat.animation.num_frames)) {
		return false;
	}
	set_ir_texture_slot(mat, p_slot, p_frame_idx, p_path, THREEDI_TEX_FLAG_ANIMATED);
	_notify_object_changed(UPDATE_MTRL);
	return true;
}

Array NovaObjectData::get_shader_catalog() const {
	Array result;
	for (size_t i = 0; i < oed::kMaterialDescriptorTableCount; ++i) {
		const oed::MaterialDescriptorRecord &record = oed::kMaterialDescriptorTable[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["name"] = from_native(record.name);
		add_shader_flag_fields(item, record.name, static_cast<uint32_t>(record.shader_flags));
		result.push_back(item);
	}
	return result;
}

Array NovaObjectData::get_global_control_register_catalog() {
	Array result;
	for (size_t ordinal = 0; ordinal < THREEDI_CTRL_REGISTER_COUNT; ++ordinal) {
		Dictionary item;
		item["ordinal"] = static_cast<int64_t>(ordinal);
		item["name"] = from_native(threedi_ctrl_register_name(ordinal));
		result.push_back(item);
	}
	return result;
}

String NovaObjectData::canonical_control_register_name(const String &p_name) {
	const CharString utf8 = p_name.utf8();
	const int ordinal = threedi_ctrl_register_ordinal(utf8.get_data());
	return ordinal == THREEDI_CTRL_REGISTER_NOT_FOUND
			? String()
			: from_native(threedi_ctrl_register_name(
					  static_cast<size_t>(ordinal)));
}

Array NovaObjectData::get_control_registers() const {
	Array result;
	if (!has_ir) {
		return result;
	}
	for (size_t i = 0; i < ir.control_register_count; ++i) {
		const char *authored_name = ir.control_registers[i].name;
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
		// [orig: sub_5B4640 @ 0x5B4640; ordinal store @ 0x5B46E6;
		//  CtrlName_ToOrdinal @ 0x57B290]
		result.push_back(item);
	}
	return result;
}

bool NovaObjectData::set_control_register_name(
		int p_index, const String &p_name) {
	if (!has_ir || ir.control_registers == nullptr || p_index < 0 ||
			static_cast<size_t>(p_index) >= ir.control_register_count) {
		return false;
	}
	const std::string name = to_std(p_name);
	if (name.size() > 24) {
		return false;
	}
	copy_cstr(ir.control_registers[p_index].name,
			sizeof(ir.control_registers[p_index].name), name.c_str());
	// A local CTRL rename can retarget material, texture, PANM, and light
	// references after the retail loader-name fixup, so invalidate every
	// dependent view rather than guessing which blocks cite this slot.
	_notify_object_changed(UPDATE_ALL);
	return true;
}

String NovaObjectData::resolve_material_texture_path(int p_material_index, int p_texture_index) const {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count ||
			p_texture_index < 0 || p_texture_index >= 8) {
		return String();
	}

	const ThreediIRMaterial &material = ir.materials[p_material_index];
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

Ref<Texture2D> NovaObjectData::load_material_texture(int p_material_index, int p_texture_index) const {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count ||
			p_texture_index < 0 || p_texture_index >= 8) {
		return Ref<Texture2D>();
	}

	const ThreediIRMaterial &material = ir.materials[p_material_index];
	if (static_cast<uint32_t>(p_texture_index) >= material.texture_count) {
		return Ref<Texture2D>();
	}

	const String texture_name = from_native(material.textures[p_texture_index].name);
	return resource_root.is_valid()
			? resource_root->load_texture(texture_name)
			: opennova::load_texture_from_dir(source_dir, texture_name);
}

String NovaObjectData::resolve_texture_name(const String &p_texture_name) const {
	if (!has_ir || p_texture_name.is_empty()) {
		return String();
	}
	if (resource_root.is_valid()) {
		const String resolved = resource_root->resolve_file(p_texture_name);
		return resolved.is_empty() && resource_root->load_texture(p_texture_name).is_valid() ? p_texture_name : resolved;
	}
	return opennova::resolve_texture_path(source_dir, p_texture_name);
}

Ref<Texture2D> NovaObjectData::load_texture_name(const String &p_texture_name) const {
	if (!has_ir || p_texture_name.is_empty()) {
		return Ref<Texture2D>();
	}
	if (resource_root.is_valid()) {
		return resource_root->load_texture(p_texture_name);
	}
	return opennova::load_texture_from_dir(source_dir, p_texture_name);
}

Error NovaObjectData::set_material_shader(int p_material_index, const String &p_shader_name) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	copy_cstr(ir.materials[p_material_index].shader_name, sizeof(ir.materials[p_material_index].shader_name),
			to_std(p_shader_name).c_str());
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_material_texture(int p_material_index, int p_texture_index, const String &p_texture_name) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	ThreediIRMaterial &material = ir.materials[p_material_index];
	if (p_texture_index < 0 || p_texture_index >= 8) {
		return ERR_INVALID_PARAMETER;
	}
	if (static_cast<uint32_t>(p_texture_index) >= material.texture_count) {
		material.texture_count = static_cast<uint32_t>(p_texture_index + 1);
	}
	copy_cstr(material.textures[p_texture_index].name, sizeof(material.textures[p_texture_index].name),
			to_std(p_texture_name).c_str());
	if (material.textures[p_texture_index].slot == 0) {
		material.textures[p_texture_index].slot = THREEDI_IR_TEX_SLOT_DIFFUSE;
	}
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_material_texture_slot(int p_material_index, int p_slot, const String &p_texture_name) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_slot < THREEDI_IR_TEX_SLOT_DIFFUSE || p_slot > THREEDI_IR_TEX_SLOT_NORMAL_B) {
		return ERR_INVALID_PARAMETER;
	}

	ThreediIRMaterial &material = ir.materials[p_material_index];
	int texture_index = -1;
	for (uint32_t i = 0; i < material.texture_count && i < 8; ++i) {
		if (material.textures[i].slot == static_cast<uint8_t>(p_slot)) {
			texture_index = static_cast<int>(i);
			break;
		}
	}

	if (texture_index < 0) {
		if (p_texture_name.is_empty()) {
			return OK;
		}
		if (material.texture_count >= 8) {
			return ERR_OUT_OF_MEMORY;
		}
		texture_index = static_cast<int>(material.texture_count);
		++material.texture_count;
		ThreediIRMaterialTexture &texture = material.textures[texture_index];
		std::memset(&texture, 0, sizeof(texture));
		texture.slot = static_cast<uint8_t>(p_slot);
		texture.type = (p_slot == THREEDI_IR_TEX_SLOT_NORMAL || p_slot == THREEDI_IR_TEX_SLOT_NORMAL_B) ? 4 : 0;
	}

	ThreediIRMaterialTexture &texture = material.textures[texture_index];
	copy_cstr(texture.name, sizeof(texture.name), to_std(p_texture_name.get_file()).c_str());
	texture.slot = static_cast<uint8_t>(p_slot);
	if (p_slot == THREEDI_IR_TEX_SLOT_NORMAL || p_slot == THREEDI_IR_TEX_SLOT_NORMAL_B) {
		const String texture_name = p_texture_name.to_lower();
		texture.type = texture_name.contains(".tga") ? 5 : 4;
	} else {
		texture.type = 0;
	}
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_material_texture_slot_options(int p_material_index, int p_slot, int p_flags, int p_frame, int p_type) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_slot < THREEDI_IR_TEX_SLOT_DIFFUSE || p_slot > THREEDI_IR_TEX_SLOT_NORMAL_B) {
		return ERR_INVALID_PARAMETER;
	}

	ThreediIRMaterial &material = ir.materials[p_material_index];
	for (uint32_t i = 0; i < material.texture_count && i < 8; ++i) {
		ThreediIRMaterialTexture &texture = material.textures[i];
		if (texture.slot != static_cast<uint8_t>(p_slot)) {
			continue;
		}
		texture.flags = static_cast<uint8_t>(std::clamp(p_flags, 0, 255));
		texture.frame = static_cast<uint8_t>(std::clamp(p_frame, 0, 255));
		texture.type = static_cast<uint8_t>(std::clamp(p_type, 0, 255));
		_notify_object_changed(UPDATE_MTRL);
		return OK;
	}

	return ERR_DOES_NOT_EXIST;
}

Error NovaObjectData::set_material_alpha_threshold(int p_material_index, float p_alpha_threshold) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	ir.materials[p_material_index].alpha_threshold = std::clamp(p_alpha_threshold, 0.0f, 1.0f);
	ir.materials[p_material_index].flags |= THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST;
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_material_uv_generator(int p_material_index, const String &p_axis, const Dictionary &p_params) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	const String axis = p_axis.to_lower();
	if (axis == "u") {
		apply_uv_params(ir.materials[p_material_index].u_params, p_params);
	} else if (axis == "v") {
		apply_uv_params(ir.materials[p_material_index].v_params, p_params);
	} else {
		return ERR_INVALID_PARAMETER;
	}
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_material_rgb_generator(int p_material_index, const Dictionary &p_params) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	apply_rgb_gen(ir.materials[p_material_index].rgb_gen, p_params);
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_material_alpha_generator(int p_material_index, const Dictionary &p_params) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	apply_alpha_gen(ir.materials[p_material_index].alpha_gen, p_params);
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_material_texture_animation(int p_material_index, const Dictionary &p_params) {
	if (!has_ir || p_material_index < 0 || static_cast<size_t>(p_material_index) >= ir.material_count) {
		return ERR_INVALID_PARAMETER;
	}
	ThreediIRTexAnim &animation = ir.materials[p_material_index].animation;
	animation.num_frames = static_cast<uint8_t>(std::clamp(dict_int(p_params, "num_frames", animation.num_frames), 0, 255));
	animation.animation_type = static_cast<uint8_t>(std::clamp(dict_int(p_params, "animation_type", animation.animation_type), 0, 255));
	animation.cycle_frame_time = clamp_to_i16(dict_int(p_params, "cycle_frame_time", animation.cycle_frame_time));
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}
