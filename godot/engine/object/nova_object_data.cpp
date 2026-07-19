#include "object/nova_object_data.h"

#include "util/nova_string_convert.h"

#include <ase/ase_parser.h>
#include <oed/material_descriptor.h>
#include <oed/types.h>
#include <renderer/light_runtime.h>
#include <renderer/material_eval.h>
#include <threedi/threedi_panm_runtime.h>

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "util/texture_path_resolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <string>
#include <vector>

using namespace godot;

namespace {

using opennova::to_std;

std::string to_native_path(const String &path) {
	String global = path;
	if (ProjectSettings::get_singleton() != nullptr) {
		global = ProjectSettings::get_singleton()->globalize_path(path);
	}
	return to_std(global);
}

String from_native(const char *value) {
	return String(value == nullptr ? "" : value);
}

String oed_error_detail(OedSession *session, const char *fallback) {
	const char *detail = session != nullptr ? oed_session_last_error(session) : nullptr;
	if (detail != nullptr && detail[0] != '\0') {
		return String(fallback) + ": " + from_native(detail);
	}
	return String(fallback);
}

void copy_cstr(char *dst, size_t dst_size, const char *src) {
	if (dst == nullptr || dst_size == 0) {
		return;
	}
	if (src == nullptr) {
		dst[0] = '\0';
		return;
	}
	std::strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

uint8_t to_u8_color(float value) {
	const int rounded = static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
	return static_cast<uint8_t>(std::clamp(rounded, 0, 255));
}

uint8_t to_u8_255(float value) {
	const int rounded = static_cast<int>(std::lround(std::clamp(value, 0.0f, 255.0f)));
	return static_cast<uint8_t>(std::clamp(rounded, 0, 255));
}

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

String filename_stem(const String &path) {
	const String stem = path.get_file().get_basename();
	return stem.is_empty() ? String("untitled") : stem;
}

String sanitized_basename(const String &value) {
	String name = value.strip_edges();
	if (name.is_empty()) {
		name = "untitled";
	}
	const String ext = name.get_extension().to_lower();
	if (ext == "3di" || ext == "3dp" || ext == "ase") {
		name = name.get_basename();
	}
	name = name.replace(" ", "_").replace("/", "_").replace("\\", "_").replace(":", "_");
	return name;
}

std::string resolve_relative_file(const String &dir, const char *filename) {
	std::filesystem::path base(to_native_path(dir));
	std::filesystem::path candidate = base / (filename ? filename : "");
	if (std::filesystem::exists(candidate)) {
		return candidate.string();
	}

	const std::string wanted = filename ? filename : "";
	std::string wanted_lower = wanted;
	std::transform(wanted_lower.begin(), wanted_lower.end(), wanted_lower.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	std::error_code ec;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(base, ec)) {
		if (ec) {
			break;
		}
		std::string current = entry.path().filename().string();
		std::transform(current.begin(), current.end(), current.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (current == wanted_lower) {
			return entry.path().string();
		}
	}
	return candidate.string();
}

bool copy_scene_file_to_project_dir(const std::filesystem::path &source_path,
		const std::filesystem::path &dest_path, std::string &error) {
	std::error_code ec;
	if (!std::filesystem::exists(source_path, ec)) {
		error = "missing scene source " + source_path.string();
		return false;
	}

	ec.clear();
	if (std::filesystem::exists(dest_path, ec)) {
		ec.clear();
		if (std::filesystem::equivalent(source_path, dest_path, ec)) {
			return true;
		}
	}

	const std::filesystem::path parent = dest_path.parent_path();
	if (!parent.empty()) {
		ec.clear();
		std::filesystem::create_directories(parent, ec);
		if (ec) {
			error = "could not create scene directory " + parent.string() + ": " + ec.message();
			return false;
		}
	}

	ec.clear();
	std::filesystem::copy_file(source_path, dest_path, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		error = "could not copy " + source_path.string() + " to " + dest_path.string() + ": " + ec.message();
		return false;
	}
	return true;
}

bool copy_project_scene_sources_to_dir(const TdpProject &project, const String &source_dir,
		const String &dest_dir, std::string &error) {
	if (source_dir.is_empty() || dest_dir.is_empty()) {
		return true;
	}

	const std::filesystem::path dest_root(to_native_path(dest_dir));
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		const char *scene_file = project.lods[i].scene_file;
		if (scene_file[0] == '\0') {
			break;
		}

		std::filesystem::path scene_rel(scene_file);
		const std::filesystem::path source_path = scene_rel.is_absolute() ?
				scene_rel :
				std::filesystem::path(resolve_relative_file(source_dir, scene_file));
		const std::filesystem::path dest_path = scene_rel.is_absolute() ?
				dest_root / scene_rel.filename() :
				dest_root / scene_rel;
		if (!copy_scene_file_to_project_dir(source_path, dest_path, error)) {
			return false;
		}
	}
	return true;
}

int project_lod_count(const TdpProject &project) {
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		if (project.lods[i].scene_file[0] == '\0') {
			return i;
		}
	}
	return TDP_MAX_LODS;
}

bool same_directory(const String &a, const String &b) {
	if (a == b) {
		return true;
	}
	std::error_code ec;
	const std::filesystem::path pa(to_native_path(a));
	const std::filesystem::path pb(to_native_path(b));
	if (std::filesystem::exists(pa, ec) && std::filesystem::exists(pb, ec)) {
		ec.clear();
		return std::filesystem::equivalent(pa, pb, ec);
	}
	return false;
}

void set_default_project_lod_fields(TdpLod &lod) {
	if (lod.attributes == 0) {
		lod.attributes = 5;
	}
	if (lod.render_function[0] == '\0') {
		copy_cstr(lod.render_function, sizeof(lod.render_function), "gnrc");
	}
}

void copy_transform(const ThreediIRTransform &src, ThreediTransform &dst) {
	dst.control = src.control;
	dst.control_param = src.control_param;
	dst.rate = src.rate;
	dst.start = src.start;
	dst.end = src.end;
}

void copy_ir_part_animation(const ThreediIRPartAnimation &src, ThreediPartAnimation &dst) {
	dst.flags = src.flags;
	dst.parent_subobject = src.parent_part;
	dst.subobject_index = src.part_index;
	dst.matrix_index = src.matrix_index;
	dst.matrix_offset = src.matrix_offset;
	dst.bind_matrix_index = src.bind_matrix_index;
	copy_transform(src.rotation_x, dst.rotation_x);
	copy_transform(src.rotation_y, dst.rotation_y);
	copy_transform(src.rotation_z, dst.rotation_z);
	copy_transform(src.scale_x, dst.scale_x);
	copy_transform(src.scale_y, dst.scale_y);
	copy_transform(src.scale_z, dst.scale_z);
	copy_transform(src.translation, dst.translation);
}

void copy_ir_material(const ThreediIRMaterial &src, ThreediMaterial &dst) {
	dst.index = src.index;
	copy_cstr(dst.shader_name, sizeof(dst.shader_name), src.shader_name);
	dst.texture_count = std::min<uint32_t>(src.texture_count, 8u);
	std::memset(dst.textures, 0, sizeof(dst.textures));
	for (uint32_t i = 0; i < dst.texture_count; ++i) {
		copy_cstr(dst.textures[i].name, sizeof(dst.textures[i].name), src.textures[i].name);
		dst.textures[i].slot = src.textures[i].slot;
		dst.textures[i].type = src.textures[i].type;
		dst.textures[i].flags = src.textures[i].flags;
		dst.textures[i].frame = src.textures[i].frame;
	}

	dst.material_flags = 0;
	if ((src.flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST) != 0) {
		dst.material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_TEST;
	}
	if ((src.flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT) != 0) {
		dst.material_flags |= THREEDI_MATERIAL_FLAG_ALPHA_INVERT;
	}
	if ((src.flags & THREEDI_IR_MATERIAL_FLAG_TWO_SIDED) != 0) {
		dst.material_flags |= THREEDI_MATERIAL_FLAG_TWO_SIDED;
	}
	dst.alpha_test_value_byte = to_u8_color(src.alpha_threshold);
	dst.u_params.style = src.u_params.style;
	dst.u_params.phase = src.u_params.phase;
	dst.u_params.reg = src.u_params.reg;
	dst.u_params.gen_rate = src.u_params.gen_rate;
	dst.u_params.start = src.u_params.start;
	dst.u_params.end = src.u_params.end;
	dst.v_params.style = src.v_params.style;
	dst.v_params.phase = src.v_params.phase;
	dst.v_params.reg = src.v_params.reg;
	dst.v_params.gen_rate = src.v_params.gen_rate;
	dst.v_params.start = src.v_params.start;
	dst.v_params.end = src.v_params.end;
	dst.alpha_gen.style = src.alpha_gen.style;
	dst.alpha_gen.phase = src.alpha_gen.phase;
	dst.alpha_gen.reg = src.alpha_gen.reg;
	dst.alpha_gen.rate = src.alpha_gen.rate;
	dst.alpha_gen.start = src.alpha_gen.start;
	dst.alpha_gen.end = src.alpha_gen.end;
	dst.rgb_gen.style = src.rgb_gen.style;
	dst.rgb_gen.phase = src.rgb_gen.phase;
	dst.rgb_gen.reg = src.rgb_gen.reg;
	dst.rgb_gen.rate = src.rgb_gen.rate;
	std::memcpy(dst.rgb_gen.start_color, src.rgb_gen.start_color, sizeof(dst.rgb_gen.start_color));
	std::memcpy(dst.rgb_gen.end_color, src.rgb_gen.end_color, sizeof(dst.rgb_gen.end_color));
	std::memcpy(dst.reflect_color, src.reflect_color, sizeof(dst.reflect_color));
	dst.emissive_type = src.emissive_type;
	dst.is_glass = static_cast<uint8_t>(src.is_glass != 0);
	dst.animation.num_frames = src.animation.num_frames;
	dst.animation.animation_type = src.animation.animation_type;
	dst.animation.cycle_frame_time = src.animation.cycle_frame_time;
}

void copy_ir_light(const ThreediIRLight &src, ThreediLight &dst) {
	std::memcpy(dst.offset, src.offset, sizeof(dst.offset));
	dst.atten_start = src.attenuation_start;
	dst.atten_end = src.attenuation_end;
	dst.style = src.style;
	dst.phase = src.phase;
	dst.rate = src.rate;
	dst.color_start[0] = to_u8_color(src.color_start[2]);
	dst.color_start[1] = to_u8_color(src.color_start[1]);
	dst.color_start[2] = to_u8_color(src.color_start[0]);
	dst.color_end[0] = to_u8_color(src.color_end[2]);
	dst.color_end[1] = to_u8_color(src.color_end[1]);
	dst.color_end[2] = to_u8_color(src.color_end[0]);
	dst.subobj_index = static_cast<uint8_t>(std::clamp(src.part_index, 0, 255));
	dst.flags = src.flags;
	dst.falloff_byte = to_u8_255(src.falloff);
	dst.rotation[0] = src.rotation[0];
	dst.rotation[1] = src.rotation[1];
	dst.rotation[2] = src.rotation[2];
	dst.rotation[3] = std::cos(src.falloff * 0.017453292519943295f);
}

void copy_lod_binding(TdpLod &dst, const TdpLod &src) {
	if (src.scene_file[0] != '\0') {
		copy_cstr(dst.scene_file, sizeof(dst.scene_file), src.scene_file);
	}
	if (src.attributes != 0) {
		dst.attributes = src.attributes;
	}
	if (src.render_function[0] != '\0') {
		copy_cstr(dst.render_function, sizeof(dst.render_function), src.render_function);
	}
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
	dst.start = static_cast<int16_t>(std::clamp(dict_int(params, "start", dst.start), -32768, 32767));
	dst.end = static_cast<int16_t>(std::clamp(dict_int(params, "end", dst.end), -32768, 32767));
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

int material_array_index_for_id(const ThreediModelIR &ir, int32_t material_index) {
	for (size_t i = 0; i < ir.material_count; ++i) {
		if (ir.materials[i].index == material_index) {
			return static_cast<int>(i);
		}
	}
	if (material_index >= 0 && static_cast<size_t>(material_index) < ir.material_count) {
		return material_index;
	}
	return -1;
}

String control_register_name_for(const ThreediModelIR &ir, int32_t reg) {
	if (reg < 0 || static_cast<size_t>(reg) >= ir.control_register_count) {
		return String();
	}
	return from_native(ir.control_registers[reg].name);
}

constexpr double kPanmRotationUnit = 360.0 / 16384.0;
constexpr double kPanmValueUnit = 1.0 / 256.0;

String part_anim_part_label(int part_index) {
	return vformat("Part %02d", part_index);
}

Dictionary part_anim_part_ref(const ThreediIRLod &lod, int part_index) {
	Dictionary result;
	result["index"] = part_index;
	result["label"] = part_anim_part_label(part_index);
	result["valid"] = part_index >= 0 && static_cast<size_t>(part_index) < lod.part_count;
	return result;
}

String panm_mode_key_for_control(uint8_t control) {
	switch (control) {
		case 0: return "none";
		case 16: return "slide";
		case 17: return "slide_inverse";
		case 24: return "set";
		case 32: return "rotate_cw";
		case 33: return "rotate_ccw";
		case 50: return "sine_wave";
		case 52: return "saw_wave";
		case 53: return "inverse_saw_wave";
		case 113: return "control_register";
		case 114: return "control_register_add";
		default:
			break;
	}
	const ThreediControlFuncInfo *info = threedi_control_func_info(control);
	if (info == nullptr || info->name == nullptr) {
		return "unsupported";
	}
	String key = String(info->name).to_lower();
	return key.replace("set_wave_", "").replace("add_wave_", "add_");
}

String panm_mode_label_for_control(uint8_t control) {
	switch (control) {
		case 0: return "None";
		case 16: return "Slide";
		case 17: return "Slide inverse";
		case 24: return "Set";
		case 32: return "Rotate clockwise";
		case 33: return "Rotate counter-clockwise";
		case 50: return "Sine wave";
		case 52: return "Saw wave";
		case 53: return "Inverse saw wave";
		case 113: return "Control register";
		case 114: return "Add control register";
		default:
			break;
	}
	const ThreediControlFuncInfo *info = threedi_control_func_info(control);
	return (info != nullptr && info->name != nullptr) ? String(info->name).capitalize() : String("Unsupported");
}

bool panm_control_uses_register(uint8_t control) {
	const ThreediControlFuncInfo *info = threedi_control_func_info(control);
	return info != nullptr && info->is_register_func != 0;
}

bool panm_control_supported(uint8_t control) {
	return threedi_control_func_info(control) != nullptr;
}

bool panm_control_for_mode(const String &p_mode, uint8_t &out_control) {
	const String mode = p_mode.to_lower();
	if (mode == "none") {
		out_control = 0;
		return true;
	}
	if (mode == "slide") {
		out_control = 16;
		return true;
	}
	if (mode == "slide_inverse") {
		out_control = 17;
		return true;
	}
	if (mode == "set") {
		out_control = 24;
		return true;
	}
	if (mode == "rotate_cw") {
		out_control = 32;
		return true;
	}
	if (mode == "rotate_ccw") {
		out_control = 33;
		return true;
	}
	if (mode == "sine_wave") {
		out_control = 50;
		return true;
	}
	if (mode == "saw_wave") {
		out_control = 52;
		return true;
	}
	if (mode == "inverse_saw_wave") {
		out_control = 53;
		return true;
	}
	if (mode == "control_register") {
		out_control = 113;
		return true;
	}
	if (mode == "control_register_add") {
		out_control = 114;
		return true;
	}
	return false;
}

Dictionary panm_axis_to_editor_dict(const ThreediIRTransform &track, const ThreediModelIR &ir, bool is_rotation) {
	Dictionary result;
	const bool supported = panm_control_supported(track.control);
	result["supported"] = supported;
	result["mode"] = panm_mode_key_for_control(track.control);
	result["mode_label"] = panm_mode_label_for_control(track.control);
	result["uses_control_register"] = panm_control_uses_register(track.control);
	result["control_register"] = static_cast<int>(track.control_param);
	result["control_register_label"] = control_register_name_for(ir, track.control_param);
	result["speed"] = static_cast<double>(track.rate) * kPanmValueUnit;
	const double unit = is_rotation ? kPanmRotationUnit : kPanmValueUnit;
	result["from_value"] = static_cast<double>(track.start) * unit;
	result["to_value"] = static_cast<double>(track.end) * unit;
	return result;
}

int16_t panm_clamp_i16_from_double(double value) {
	if (!std::isfinite(value)) {
		return 0;
	}
	return static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(value)), -32768, 32767));
}

uint32_t panm_pack_flags_from_parts(uint8_t scale_type, uint8_t rotation_type, uint8_t rotation_reversed, uint8_t translate_type) {
	return static_cast<uint32_t>(scale_type) |
			(static_cast<uint32_t>(rotation_type) << 8) |
			(static_cast<uint32_t>(rotation_reversed) << 16) |
			(static_cast<uint32_t>(translate_type) << 24);
}

String panm_axis_key_for_translate_type(uint8_t translate_type) {
	switch (translate_type) {
		case THREEDI_TRANS_X: return "x";
		case THREEDI_TRANS_Y: return "y";
		case THREEDI_TRANS_Z: return "z";
		default: return "none";
	}
}

uint8_t panm_translate_type_for_axis(const String &p_axis) {
	const String axis = p_axis.to_lower();
	if (axis == "y") {
		return THREEDI_TRANS_Y;
	}
	if (axis == "z") {
		return THREEDI_TRANS_Z;
	}
	if (axis == "none") {
		return THREEDI_TRANS_NONE;
	}
	return THREEDI_TRANS_X;
}

String panm_scale_style_for_type(uint8_t scale_type) {
	if (scale_type == 1) {
		return "uniform";
	}
	if (scale_type == 2) {
		return "per_axis";
	}
	return "none";
}

ThreediIRTransform *semantic_part_anim_track(ThreediIRPartAnimation &anim, const String &p_channel, const String &p_axis) {
	const String channel = p_channel.to_lower();
	const String axis = p_axis.to_lower();
	if (channel == "rotation") {
		if (axis == "y") {
			return &anim.rotation_y;
		}
		if (axis == "z") {
			return &anim.rotation_z;
		}
		return &anim.rotation_x;
	}
	if (channel == "scale") {
		if (axis == "y") {
			return &anim.scale_y;
		}
		if (axis == "z") {
			return &anim.scale_z;
		}
		return &anim.scale_x;
	}
	if (channel == "translation") {
		return &anim.translation;
	}
	return nullptr;
}

void ensure_part_anim_channel_flag(ThreediIRPartAnimation &anim, const String &p_channel, const String &p_axis) {
	uint8_t scale_type = threedi_panm_scale_type(anim.flags);
	uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
	uint8_t rotation_reversed = threedi_panm_rotation_reversed(anim.flags) ? 1 : 0;
	uint8_t translate_type = threedi_panm_translate_type(anim.flags);
	const String channel = p_channel.to_lower();
	if (channel == "rotation") {
		rotation_type = 2;
	} else if (channel == "scale") {
		const String axis = p_axis.to_lower();
		scale_type = (axis == "y" || axis == "z" || axis == "per_axis") ? 2 : 1;
	} else if (channel == "translation") {
		translate_type = panm_translate_type_for_axis(p_axis);
		if (translate_type == THREEDI_TRANS_NONE) {
			translate_type = THREEDI_TRANS_X;
		}
	}
	anim.flags = panm_pack_flags_from_parts(scale_type, rotation_type, rotation_reversed, translate_type);
}

Dictionary transform_to_dict(const ThreediIRTransform &track, const ThreediModelIR &ir) {
	Dictionary result;
	result["control"] = static_cast<int>(track.control);
	result["function"] = static_cast<int>(track.control);
	result["control_param"] = static_cast<int>(track.control_param);
	result["reg"] = static_cast<int>(track.control_param);
	result["reg_name"] = control_register_name_for(ir, track.control_param);
	result["rate"] = static_cast<int>(track.rate);
	result["start"] = static_cast<int>(track.start);
	result["end"] = static_cast<int>(track.end);
	return result;
}

template <typename Animation>
bool panm_animation_is_live(const Animation &anim) {
	const auto track_is_live = [](const auto &track) {
		return (track.control & 0xF0u) != 0;
	};
	const uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
	// Spinner and the two view-derived modes are evaluated without sampling a
	// conventional track. In particular, spinner coefficients reinterpret raw
	// PANM bytes as floats and may have a zero control high nibble.
	if (rotation_type == 1 || rotation_type == 3 || rotation_type == 4)
		return true;
	if (rotation_type == 2 &&
			(track_is_live(anim.rotation_x) ||
			 track_is_live(anim.rotation_y) ||
			 track_is_live(anim.rotation_z)))
		return true;

	const uint8_t scale_type = threedi_panm_scale_type(anim.flags);
	if (scale_type == 1 && track_is_live(anim.scale_x))
		return true;
	if (scale_type == 2 &&
			(track_is_live(anim.scale_x) ||
			 track_is_live(anim.scale_y) ||
			 track_is_live(anim.scale_z)))
		return true;

	return threedi_panm_translate_type(anim.flags) != THREEDI_TRANS_NONE &&
			track_is_live(anim.translation);
}

uint32_t retail_runtime_time_ms(int64_t time_ms) {
	return time_ms < 0 ? 0u : static_cast<uint32_t>(time_ms);
}

const ThreediIRTransform *part_anim_track_for_name(const ThreediIRPartAnimation &anim, const String &p_track) {
	const String track = p_track.to_lower();
	if (track == "rotation_x" || track == "yaw") {
		return &anim.rotation_x;
	}
	if (track == "rotation_y" || track == "pitch") {
		return &anim.rotation_y;
	}
	if (track == "rotation_z" || track == "roll") {
		return &anim.rotation_z;
	}
	if (track == "scale_x" || track == "scale") {
		return &anim.scale_x;
	}
	if (track == "scale_y") {
		return &anim.scale_y;
	}
	if (track == "scale_z") {
		return &anim.scale_z;
	}
	if (track == "translation" || track == "trans_x" || track == "trans_y" || track == "trans_z") {
		return &anim.translation;
	}
	return nullptr;
}

ThreediIRTransform *part_anim_track_for_name(ThreediIRPartAnimation &anim, const String &p_track) {
	const String track = p_track.to_lower();
	if (track == "rotation_x" || track == "yaw") {
		return &anim.rotation_x;
	}
	if (track == "rotation_y" || track == "pitch") {
		return &anim.rotation_y;
	}
	if (track == "rotation_z" || track == "roll") {
		return &anim.rotation_z;
	}
	if (track == "scale_x" || track == "scale") {
		return &anim.scale_x;
	}
	if (track == "scale_y") {
		return &anim.scale_y;
	}
	if (track == "scale_z") {
		return &anim.scale_z;
	}
	if (track == "translation" || track == "trans_x" || track == "trans_y" || track == "trans_z") {
		return &anim.translation;
	}
	return nullptr;
}

bool resolve_control_register_index(const ThreediModelIR &ir, const String &name, int32_t &out_reg) {
	if (name.is_empty()) {
		out_reg = -1;
		return true;
	}
	const std::string needle = to_std(name);
	for (size_t i = 0; i < ir.control_register_count; ++i) {
		if (needle == ir.control_registers[i].name) {
			out_reg = static_cast<int32_t>(i);
			return true;
		}
	}
	return false;
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

std::vector<std::string> control_register_names(const ThreediModelIR &ir) {
	std::vector<std::string> names;
	names.reserve(ir.control_register_count);
	for (size_t i = 0; i < ir.control_register_count; ++i) {
		names.emplace_back(ir.control_registers[i].name);
	}
	return names;
}

std::unordered_map<std::string, uint16_t> control_values_from_dict(const Dictionary &dict) {
	std::unordered_map<std::string, uint16_t> values;
	const Array keys = dict.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		values[to_std(key)] = static_cast<uint16_t>(std::clamp(static_cast<int>(dict[keys[i]]), 0, 65535));
	}
	return values;
}

Transform3D panm_matrix_to_transform(const ThreediMatrix4x4 &m) {
	const float *r = m.m;
	Transform3D t;
	t.basis[0] = Vector3(r[0], -r[4], -r[8]);
	t.basis[1] = Vector3(-r[1], r[5], r[9]);
	t.basis[2] = Vector3(-r[2], r[6], r[10]);
	t.origin = Vector3(-r[12], r[13], r[14]);
	return t;
}

Vector3 godot_position(const ThreediIRVertex &v) {
	return Vector3(-v.position[0], v.position[1], v.position[2]);
}

Vector3 godot_normal(const ThreediIRVertex &v) {
	return Vector3(-v.normal[0], v.normal[1], v.normal[2]);
}

Vector3 godot_vec3(const float v[3]) {
	return Vector3(-v[0], v[1], v[2]);
}

bool vertex_has_tangents(const ThreediIRVertex &v) {
	if ((v.flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0) {
		return true;
	}
	const float tangent_len =
			v.tangent[0] * v.tangent[0] + v.tangent[1] * v.tangent[1] + v.tangent[2] * v.tangent[2];
	const float bitangent_len =
			v.bitangent[0] * v.bitangent[0] + v.bitangent[1] * v.bitangent[1] + v.bitangent[2] * v.bitangent[2];
	return tangent_len > 0.000001f && bitangent_len > 0.000001f;
}

bool decode_primitive_indices(const ThreediIRLod &lod, const ThreediIRPrimitive &prim, std::vector<uint16_t> &out) {
	out.clear();
	if (lod.indices == nullptr || lod.vertices == nullptr || prim.index_count == 0 || prim.vertex_count == 0) {
		return false;
	}
	if (prim.index_offset + prim.index_count > lod.index_count ||
			prim.vertex_offset + prim.vertex_count > lod.vertex_count) {
		return false;
	}

	const uint16_t *raw = lod.indices + prim.index_offset;
	uint16_t min_idx = 0xffffu;
	uint16_t max_idx = 0;
	for (uint32_t i = 0; i < prim.index_count; ++i) {
		const uint16_t idx = raw[i];
		min_idx = std::min(min_idx, idx);
		max_idx = std::max(max_idx, idx);
	}
	const bool relative_valid = max_idx < prim.vertex_count;
	const bool absolute_valid = min_idx >= prim.vertex_offset &&
			static_cast<uint32_t>(max_idx) - prim.vertex_offset < prim.vertex_count;
	const bool use_absolute = absolute_valid && !relative_valid;

	auto to_local = [&](uint16_t idx, bool &ok) -> uint16_t {
		if (!use_absolute) {
			if (idx >= prim.vertex_count) {
				ok = false;
				return 0;
			}
			return idx;
		}
		if (idx < prim.vertex_offset) {
			ok = false;
			return 0;
		}
		const uint32_t local = static_cast<uint32_t>(idx) - prim.vertex_offset;
		if (local >= prim.vertex_count) {
			ok = false;
			return 0;
		}
		return static_cast<uint16_t>(local);
	};

	bool ok = true;
	if (prim.topology == THREEDI_IR_TOPOLOGY_TRIANGLES) {
		out.reserve(prim.index_count);
		for (uint32_t i = 0; i + 2 < prim.index_count; i += 3) {
			const uint16_t a = to_local(raw[i], ok);
			const uint16_t b = to_local(raw[i + 1], ok);
			const uint16_t c = to_local(raw[i + 2], ok);
			if (!ok) {
				return false;
			}
			if (a == b || b == c || a == c) {
				continue;
			}
			out.push_back(a);
			out.push_back(b);
			out.push_back(c);
		}
	} else {
		out.reserve(static_cast<size_t>(prim.index_count) * 3);
		for (uint32_t i = 0; i + 2 < prim.index_count; ++i) {
			const bool odd = (i & 1u) != 0u;
			const uint16_t a = to_local(raw[i], ok);
			const uint16_t b = to_local(raw[i + (odd ? 2 : 1)], ok);
			const uint16_t c = to_local(raw[i + (odd ? 1 : 2)], ok);
			if (!ok) {
				return false;
			}
			if (a == b || b == c || a == c) {
				continue;
			}
			out.push_back(a);
			out.push_back(b);
			out.push_back(c);
		}
	}
	return true;
}

bool primitive_is_alpha(const ThreediIRLod &lod, size_t prim_index) {
	const ThreediIRPrimitive &prim = lod.primitives[prim_index];
	if (prim.part_index < 0 || static_cast<size_t>(prim.part_index) >= lod.part_count) {
		return false;
	}
	const ThreediIRPart &part = lod.parts[prim.part_index];
	const int alpha_start = part.primitive_start + part.opaque_count;
	const int alpha_end = alpha_start + part.alpha_count;
	return static_cast<int>(prim_index) >= alpha_start && static_cast<int>(prim_index) < alpha_end;
}

} // namespace

NovaObjectData::NovaObjectData() {
	threedi_ir_init(&ir);
	tdp_init(&source_project);
}

NovaObjectData::~NovaObjectData() {
	_clear();
}

// --- Edit-state snapshots (B3) ----------------------------------------------
// In-process undo payload only — raw little-host POD bytes behind a magic +
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

void NovaObjectData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("snapshot_edit_state"), &NovaObjectData::snapshot_edit_state);
	ClassDB::bind_method(D_METHOD("apply_edit_state", "bytes"), &NovaObjectData::apply_edit_state);
	ClassDB::bind_method(D_METHOD("open_file", "path"), &NovaObjectData::open_file);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name"), &NovaObjectData::open_from_resource_root);
	ClassDB::bind_method(D_METHOD("save_project_to_dir", "dir_path"), &NovaObjectData::save_project_to_dir);
	ClassDB::bind_method(D_METHOD("export_3di_to_dir", "dir_path", "update_mask"), &NovaObjectData::export_3di_to_dir, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("reset_empty", "name"), &NovaObjectData::reset_empty, DEFVAL("untitled"));
	ClassDB::bind_method(D_METHOD("set_lod_scene", "lod_index", "path"), &NovaObjectData::set_lod_scene);
	ClassDB::bind_method(D_METHOD("has_document"), &NovaObjectData::has_document);
	ClassDB::bind_method(D_METHOD("can_save_project"), &NovaObjectData::can_save_project);
	ClassDB::bind_method(D_METHOD("can_export_3di"), &NovaObjectData::can_export_3di);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaObjectData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_source_dir"), &NovaObjectData::get_source_dir);
	ClassDB::bind_method(D_METHOD("get_object_name"), &NovaObjectData::get_object_name);
	ClassDB::bind_method(D_METHOD("get_source_kind"), &NovaObjectData::get_source_kind);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaObjectData::get_last_error);
	ClassDB::bind_method(D_METHOD("get_oed_dirty_mask"), &NovaObjectData::get_oed_dirty_mask);
	ClassDB::bind_method(D_METHOD("get_last_oed_update_mask"), &NovaObjectData::get_last_oed_update_mask);
	ClassDB::bind_method(D_METHOD("get_summary"), &NovaObjectData::get_summary);
	ClassDB::bind_method(D_METHOD("get_project_lods"), &NovaObjectData::get_project_lods);
	ClassDB::bind_method(D_METHOD("set_lod_field", "lod_index", "key", "value"), &NovaObjectData::set_lod_field);
	ClassDB::bind_method(D_METHOD("set_project_field", "key", "value"), &NovaObjectData::set_project_field);
	ClassDB::bind_method(D_METHOD("get_material_count"), &NovaObjectData::get_material_count);
	ClassDB::bind_method(D_METHOD("get_lod_surfaces", "lod_index"), &NovaObjectData::get_lod_surfaces);
	ClassDB::bind_method(D_METHOD("get_materials"), &NovaObjectData::get_materials);
	ClassDB::bind_method(D_METHOD("get_material_info", "index"), &NovaObjectData::get_material_info);
	ClassDB::bind_method(D_METHOD("set_material_field", "index", "key", "value"), &NovaObjectData::set_material_field);
	ClassDB::bind_method(D_METHOD("get_material_shader_flags", "index"), &NovaObjectData::get_material_shader_flags);
	ClassDB::bind_method(D_METHOD("get_material_anim_frames", "index", "slot"), &NovaObjectData::get_material_anim_frames);
	ClassDB::bind_method(D_METHOD("set_material_anim_frame", "index", "slot", "frame_idx", "path"), &NovaObjectData::set_material_anim_frame);
	ClassDB::bind_method(D_METHOD("get_shader_catalog"), &NovaObjectData::get_shader_catalog);
	ClassDB::bind_method(D_METHOD("get_control_registers"), &NovaObjectData::get_control_registers);
	ClassDB::bind_method(D_METHOD("resolve_material_texture_path", "material_index", "texture_index"), &NovaObjectData::resolve_material_texture_path);
	ClassDB::bind_method(D_METHOD("load_material_texture", "material_index", "texture_index"), &NovaObjectData::load_material_texture);
	ClassDB::bind_method(D_METHOD("resolve_texture_name", "texture_name"), &NovaObjectData::resolve_texture_name);
	ClassDB::bind_method(D_METHOD("load_texture_name", "texture_name"), &NovaObjectData::load_texture_name);
	ClassDB::bind_method(D_METHOD("get_light_count"), &NovaObjectData::get_light_count);
	ClassDB::bind_method(D_METHOD("get_lights"), &NovaObjectData::get_lights);
	ClassDB::bind_method(D_METHOD("get_light_info", "index"), &NovaObjectData::get_light_info);
	ClassDB::bind_method(D_METHOD("set_light_field", "index", "key", "value"), &NovaObjectData::set_light_field);
	ClassDB::bind_method(D_METHOD("get_user_point_count"), &NovaObjectData::get_user_point_count);
	ClassDB::bind_method(D_METHOD("get_user_point_info", "index"), &NovaObjectData::get_user_point_info);
	ClassDB::bind_method(D_METHOD("get_ground_anchor", "lod_index"), &NovaObjectData::get_ground_anchor, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("has_collision"), &NovaObjectData::has_collision);
	ClassDB::bind_method(D_METHOD("has_occlusion"), &NovaObjectData::has_occlusion);
	ClassDB::bind_method(D_METHOD("get_collision_volumes"), &NovaObjectData::get_collision_volumes);
	ClassDB::bind_method(D_METHOD("has_live_panm"), &NovaObjectData::has_live_panm);
	ClassDB::bind_method(D_METHOD("has_live_panm_for_lod", "lod_index"),
			&NovaObjectData::has_live_panm_for_lod);
	ClassDB::bind_method(D_METHOD("get_live_panm_lod"), &NovaObjectData::get_live_panm_lod);
	ClassDB::bind_method(D_METHOD("get_effective_panm_targets", "lod_index"), &NovaObjectData::get_effective_panm_targets);
	ClassDB::bind_method(D_METHOD("get_part_anim_count", "lod_index"), &NovaObjectData::get_part_anim_count);
	ClassDB::bind_method(D_METHOD("get_part_animations", "lod_index"), &NovaObjectData::get_part_animations);
	ClassDB::bind_method(D_METHOD("get_part_anim_editor_entries", "lod_index"), &NovaObjectData::get_part_anim_editor_entries);
	ClassDB::bind_method(D_METHOD("add_part_anim", "lod_index", "part_index"), &NovaObjectData::add_part_anim);
	ClassDB::bind_method(D_METHOD("duplicate_part_anim", "lod_index", "anim_index"), &NovaObjectData::duplicate_part_anim);
	ClassDB::bind_method(D_METHOD("delete_part_anim", "lod_index", "anim_index"), &NovaObjectData::delete_part_anim);
	ClassDB::bind_method(D_METHOD("set_part_anim_target", "lod_index", "anim_index", "part_index", "parent_part"), &NovaObjectData::set_part_anim_target);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_enabled", "lod_index", "anim_index", "channel", "enabled"), &NovaObjectData::set_part_anim_channel_enabled);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_mode", "lod_index", "anim_index", "channel", "axis", "mode", "control_register"), &NovaObjectData::set_part_anim_channel_mode);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_values", "lod_index", "anim_index", "channel", "axis", "from_value", "to_value", "speed"), &NovaObjectData::set_part_anim_channel_values);
	ClassDB::bind_method(D_METHOD("set_part_anim_rotation_reversed", "lod_index", "anim_index", "reversed"), &NovaObjectData::set_part_anim_rotation_reversed);
	ClassDB::bind_method(D_METHOD("get_part_anim_info", "lod_index", "anim_index"), &NovaObjectData::get_part_anim_info);
	ClassDB::bind_method(D_METHOD("set_part_anim_field", "lod_index", "anim_index", "key", "value"), &NovaObjectData::set_part_anim_field);
	ClassDB::bind_method(D_METHOD("set_part_anim_track_field", "lod_index", "anim_index", "track", "key", "value"), &NovaObjectData::set_part_anim_track_field);
	ClassDB::bind_method(D_METHOD("get_render_lod_info", "lod_index"), &NovaObjectData::get_render_lod_info);
	ClassDB::bind_method(D_METHOD("get_bone_origins", "lod_index"), &NovaObjectData::get_bone_origins, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_bone_parents", "lod_index"), &NovaObjectData::get_bone_parents, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_lod_submeshes", "lod_index", "skeletal", "bone_count", "native_frame"), &NovaObjectData::build_lod_submeshes, DEFVAL(false), DEFVAL(0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_skinned", "lod_index"), &NovaObjectData::is_skinned);
	ClassDB::bind_method(D_METHOD("eval_material_runtime", "index", "time_ms", "ctrl_values"), &NovaObjectData::eval_material_runtime);
	ClassDB::bind_method(D_METHOD("compute_anim_frame", "index", "time_ms", "ctrl_values"), &NovaObjectData::compute_anim_frame);
	ClassDB::bind_method(D_METHOD("evaluate_panm", "lod_index", "time_ms", "ctrl_values"), &NovaObjectData::evaluate_panm);
	ClassDB::bind_method(D_METHOD("evaluate_lights", "time_ms", "ctrl_values"), &NovaObjectData::evaluate_lights);
	ClassDB::bind_method(D_METHOD("set_material_shader", "material_index", "shader_name"), &NovaObjectData::set_material_shader);
	ClassDB::bind_method(D_METHOD("set_material_texture", "material_index", "texture_index", "texture_name"), &NovaObjectData::set_material_texture);
	ClassDB::bind_method(D_METHOD("set_material_texture_slot", "material_index", "slot", "texture_name"), &NovaObjectData::set_material_texture_slot);
	ClassDB::bind_method(D_METHOD("set_material_texture_slot_options", "material_index", "slot", "flags", "frame", "type"), &NovaObjectData::set_material_texture_slot_options);
	ClassDB::bind_method(D_METHOD("set_material_alpha_threshold", "material_index", "alpha_threshold"), &NovaObjectData::set_material_alpha_threshold);
	ClassDB::bind_method(D_METHOD("set_material_uv_generator", "material_index", "axis", "params"), &NovaObjectData::set_material_uv_generator);
	ClassDB::bind_method(D_METHOD("set_material_rgb_generator", "material_index", "params"), &NovaObjectData::set_material_rgb_generator);
	ClassDB::bind_method(D_METHOD("set_material_alpha_generator", "material_index", "params"), &NovaObjectData::set_material_alpha_generator);
	ClassDB::bind_method(D_METHOD("set_material_texture_animation", "material_index", "params"), &NovaObjectData::set_material_texture_animation);
	ClassDB::bind_method(D_METHOD("set_light_colors", "light_index", "start", "end"), &NovaObjectData::set_light_colors);
	ClassDB::bind_method(D_METHOD("set_part_animation_flags", "lod_index", "anim_index", "flags"), &NovaObjectData::set_part_animation_flags);

	BIND_CONSTANT(UPDATE_NONE);
	BIND_CONSTANT(UPDATE_MTRL);
	BIND_CONSTANT(UPDATE_LGHT);
	BIND_CONSTANT(UPDATE_PANM);
	BIND_CONSTANT(UPDATE_ALL);

	ADD_SIGNAL(MethodInfo("object_changed"));
}

void NovaObjectData::_clear_oed_session() {
	if (oed_session != nullptr) {
		oed_session_destroy(oed_session);
		oed_session = nullptr;
	}
}

void NovaObjectData::_clear_source_model() {
	if (has_source_model) {
		threedi_3di3_free(&source_model);
		std::memset(&source_model, 0, sizeof(source_model));
		has_source_model = false;
	}
}

void NovaObjectData::_clear_source_project() {
	if (has_source_project) {
		tdp_free(&source_project);
		tdp_init(&source_project);
		has_source_project = false;
	}
}

void NovaObjectData::_clear() {
	_clear_oed_session();
	_clear_source_model();
	_clear_source_project();
	submesh_cache.clear();
	threedi_ir_free(&ir);
	threedi_ir_init(&ir);
	has_ir = false;
	source_kind = SourceKind::Empty;
	source_path = String();
	source_dir = String();
	resource_root.unref();
	object_name = "untitled";
	last_error = String();
	oed_dirty_mask = UPDATE_NONE;
	last_oed_update_mask = UPDATE_NONE;
}

void NovaObjectData::_mark_oed_dirty(uint8_t p_update_mask) {
	oed_dirty_mask |= (p_update_mask & UPDATE_ALL);
}

void NovaObjectData::_clear_oed_dirty(uint8_t p_update_mask) {
	const uint8_t update_mask = p_update_mask & UPDATE_ALL;
	if (update_mask == UPDATE_NONE || update_mask == UPDATE_ALL) {
		oed_dirty_mask = UPDATE_NONE;
		return;
	}
	oed_dirty_mask &= static_cast<uint8_t>(~update_mask) & UPDATE_ALL;
}

uint8_t NovaObjectData::_normalize_oed_update_mask(int p_update_mask) const {
	uint8_t update_mask = static_cast<uint8_t>(p_update_mask) & UPDATE_ALL;
	if (update_mask == UPDATE_NONE) {
		update_mask = oed_dirty_mask & UPDATE_ALL;
	}
	if (update_mask == UPDATE_NONE) {
		update_mask = UPDATE_ALL;
	}
	return update_mask;
}

void NovaObjectData::_notify_object_changed(uint8_t p_update_mask) {
	// Every document mutation (all OED setters, opens, LOD/scene swaps) funnels
	// through here or _clear() — the memoized submesh builds die with the data
	// they were built from.
	submesh_cache.clear();
	last_oed_update_mask = p_update_mask & UPDATE_ALL;
	_mark_oed_dirty(p_update_mask);
	emit_signal("object_changed");
	emit_changed();
}

void NovaObjectData::reset_empty(const String &p_name) {
	_clear();
	object_name = sanitized_basename(p_name);
	copy_cstr(ir.name, sizeof(ir.name), to_std(object_name).c_str());
	has_ir = true;
	_notify_object_changed();
}

Error NovaObjectData::set_lod_scene(int p_lod_index, const String &p_path) {
	if (p_path.is_empty() || p_lod_index < -1 || p_lod_index >= TDP_MAX_LODS) {
		return ERR_INVALID_PARAMETER;
	}
	if (source_kind == SourceKind::Threedi) {
		last_error = "3DI documents cannot bind project LOD scenes";
		return ERR_UNAVAILABLE;
	}

	const std::filesystem::path native_path(to_native_path(p_path));
	std::error_code ec;
	if (!std::filesystem::exists(native_path, ec)) {
		last_error = "LOD scene does not exist: " + p_path;
		return ERR_FILE_NOT_FOUND;
	}

	if (!has_source_project) {
		tdp_init(&source_project);
		has_source_project = true;
	}

	const int current_count = project_lod_count(source_project);
	const int lod_index = p_lod_index < 0 ? current_count : p_lod_index;
	if (lod_index < 0 || lod_index >= TDP_MAX_LODS || lod_index > current_count) {
		last_error = "LOD scenes must be added without gaps";
		return ERR_INVALID_PARAMETER;
	}

	const String scene_dir = p_path.get_base_dir();
	if (source_dir.is_empty() || current_count == 0) {
		source_dir = scene_dir;
	} else if (!same_directory(source_dir, scene_dir)) {
		last_error = "LOD scenes must be in the same source directory";
		return ERR_INVALID_PARAMETER;
	}

	TdpLod &lod = source_project.lods[lod_index];
	copy_cstr(lod.scene_file, sizeof(lod.scene_file), to_std(p_path.get_file()).c_str());
	set_default_project_lod_fields(lod);
	if (lod_index == 0 && (object_name.is_empty() || object_name == "untitled")) {
		object_name = filename_stem(p_path);
	}
	if (source_path.is_empty()) {
		source_path = p_path;
	}

	return _rebuild_oed_session_from_project(UPDATE_ALL);
}

Error NovaObjectData::open_file(const String &p_path) {
	const String ext = p_path.get_extension().to_lower();
	if (ext == "3di") {
		return _open_3di(p_path);
	}
	if (ext == "3dp") {
		return _open_3dp(p_path);
	}
	if (ext == "ase") {
		return _open_ase(p_path);
	}
	last_error = "Unsupported object source: " + ext;
	return ERR_FILE_UNRECOGNIZED;
}

Error NovaObjectData::open_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name) {
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.get_extension().to_lower() != "3di") {
		last_error = "Only mounted .3di object files are supported";
		return ERR_FILE_UNRECOGNIZED;
	}
	const PackedByteArray bytes = p_resource_root->read_file(file);
	if (bytes.is_empty()) {
		last_error = "Object file not found in resource root: " + file;
		return ERR_FILE_NOT_FOUND;
	}

	const Error err = _open_3di_bytes(file, bytes);
	if (err == OK) {
		resource_root = p_resource_root;
		source_dir = p_resource_root->get_root_dir();
		_notify_object_changed();
	}
	return err;
}

Error NovaObjectData::_open_3di(const String &p_path) {
	_clear();
	const std::string native_path = to_native_path(p_path);
	if (threedi_3di3_read(native_path.c_str(), &source_model) != 0) {
		last_error = "Failed to read 3DI";
		return ERR_FILE_CANT_READ;
	}
	has_source_model = true;
	if (threedi_ir_from_3di3(&source_model, &ir) != 0) {
		last_error = "Failed to convert 3DI to IR";
		_clear();
		return ERR_FILE_CORRUPT;
	}
	has_ir = true;
	source_kind = SourceKind::Threedi;
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = ir.name[0] != '\0' ? from_native(ir.name) : filename_stem(p_path);
	_notify_object_changed();
	return OK;
}

Error NovaObjectData::_open_3di_bytes(const String &p_name, const PackedByteArray &p_bytes) {
	_clear();
	if (p_bytes.is_empty()) {
		last_error = "Mounted 3DI entry is empty";
		return ERR_FILE_CANT_READ;
	}
	if (threedi_3di3_read_memory(p_bytes.ptr(), static_cast<size_t>(p_bytes.size()), &source_model) != 0) {
		last_error = "Failed to read mounted 3DI";
		return ERR_FILE_CANT_READ;
	}
	has_source_model = true;
	if (threedi_ir_from_3di3(&source_model, &ir) != 0) {
		last_error = "Failed to convert mounted 3DI to IR";
		_clear();
		return ERR_FILE_CORRUPT;
	}
	has_ir = true;
	source_kind = SourceKind::Threedi;
	source_path = p_name.get_file();
	source_dir = String();
	object_name = ir.name[0] != '\0' ? from_native(ir.name) : filename_stem(p_name);
	return OK;
}

Error NovaObjectData::_open_3dp(const String &p_path) {
	_clear();
	const std::string native_path = to_native_path(p_path);
	if (tdp_parse(native_path.c_str(), &source_project) != 0) {
		last_error = "Failed to parse 3DP";
		return ERR_FILE_CANT_READ;
	}
	has_source_project = true;
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = filename_stem(p_path);

	std::vector<std::string> ase_paths;
	std::vector<const char *> ase_ptrs;
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		if (source_project.lods[i].scene_file[0] == '\0') {
			break;
		}
		ase_paths.push_back(resolve_relative_file(source_dir, source_project.lods[i].scene_file));
		ase_ptrs.push_back(ase_paths.back().c_str());
	}
	if (ase_ptrs.empty()) {
		last_error = "3DP has no render LOD ASE files";
		_clear();
		return ERR_FILE_CORRUPT;
	}

	const OedStatus create_rc = oed_session_create(
			ase_ptrs.data(), static_cast<int>(ase_ptrs.size()), &source_project, &oed_session);
	if (create_rc != OED_STATUS_OK) {
		last_error = "Failed to create OED session";
		_clear();
		return ERR_FILE_CANT_READ;
	}

	source_kind = SourceKind::Project;
	return _build_ir_from_project_session(nullptr);
}

Error NovaObjectData::_open_ase(const String &p_path) {
	_clear();
	tdp_init(&source_project);
	copy_cstr(source_project.lods[0].scene_file, sizeof(source_project.lods[0].scene_file),
			to_std(p_path.get_file()).c_str());
	source_project.lods[0].attributes = 5;
	copy_cstr(source_project.lods[0].render_function, sizeof(source_project.lods[0].render_function), "gnrc");
	source_project.lods[0].threshold = 0.0f;
	source_project.poly_collision_lod = 0;
	has_source_project = true;
	source_path = p_path;
	source_dir = p_path.get_base_dir();
	object_name = filename_stem(p_path);

	const std::string native_path = to_native_path(p_path);
	const char *ase_ptr = native_path.c_str();
	const OedStatus create_rc = oed_session_create(&ase_ptr, 1, &source_project, &oed_session);
	if (create_rc != OED_STATUS_OK) {
		last_error = "Failed to create OED session from ASE";
		_clear();
		return ERR_FILE_CANT_READ;
	}

	source_kind = SourceKind::Ase;
	return _build_ir_from_project_session(nullptr);
}

Error NovaObjectData::_build_ir_from_project_session(const char *p_model_name, uint8_t p_dirty_mask) {
	Threedi3di3 built_model = {};
	const OedStatus build_rc = oed_session_build_model(
			oed_session, &source_project, static_cast<uint8_t>(OED_UPDATE_ALL), p_model_name, &built_model);
	if (build_rc != OED_STATUS_OK) {
		last_error = oed_error_detail(oed_session, "Failed to build OED model");
		return ERR_FILE_CANT_READ;
	}

	if (has_ir) {
		threedi_ir_free(&ir);
		threedi_ir_init(&ir);
		has_ir = false;
	}
	if (threedi_ir_from_3di3(&built_model, &ir) != 0) {
		threedi_3di3_free(&built_model);
		last_error = "Failed to convert OED model to IR";
		return ERR_FILE_CORRUPT;
	}
	threedi_3di3_free(&built_model);
	has_ir = true;
	_notify_object_changed(p_dirty_mask);
	return OK;
}

Error NovaObjectData::_rebuild_oed_session_from_project(uint8_t p_dirty_mask) {
	if (!has_source_project) {
		last_error = "Object has no source project";
		return ERR_UNCONFIGURED;
	}

	std::vector<std::string> ase_paths;
	std::vector<const char *> ase_ptrs;
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		if (source_project.lods[i].scene_file[0] == '\0') {
			break;
		}
		ase_paths.push_back(resolve_relative_file(source_dir, source_project.lods[i].scene_file));
		ase_ptrs.push_back(ase_paths.back().c_str());
	}
	if (ase_ptrs.empty()) {
		_clear_oed_session();
		last_error = "Object project has no render LOD ASE files";
		return ERR_UNCONFIGURED;
	}

	OedSession *next_session = nullptr;
	const OedStatus create_rc = oed_session_create(
			ase_ptrs.data(), static_cast<int>(ase_ptrs.size()), &source_project, &next_session);
	if (create_rc != OED_STATUS_OK) {
		last_error = "Failed to create OED session";
		return ERR_FILE_CANT_READ;
	}

	_clear_oed_session();
	oed_session = next_session;
	source_kind = SourceKind::Project;
	return _build_ir_from_project_session(nullptr, p_dirty_mask);
}

TdpProject NovaObjectData::_build_project_from_ir() const {
	TdpProject out = {};
	tdp_from_ir(&ir, &out);
	if (has_source_project) {
		for (int i = 0; i < TDP_MAX_LODS; ++i) {
			copy_lod_binding(out.lods[i], source_project.lods[i]);
		}
		out.poly_collision_lod = source_project.poly_collision_lod;
	}
	return out;
}

Error NovaObjectData::save_project_to_dir(const String &p_dir_path) {
	if (!has_ir || p_dir_path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}

	TdpProject project = _build_project_from_ir();
	const String output_path = p_dir_path.path_join(_export_basename() + ".3dp");
	std::string scene_copy_error;
	if (has_source_project && !copy_project_scene_sources_to_dir(project, source_dir, p_dir_path, scene_copy_error)) {
		tdp_free(&project);
		last_error = "Failed to copy 3DP scene source: " + from_native(scene_copy_error.c_str());
		return ERR_FILE_CANT_WRITE;
	}

	const std::string native_path = to_native_path(output_path);
	const int rc = tdp_write(native_path.c_str(), &project);
	tdp_free(&project);
	if (rc != 0) {
		last_error = "Failed to write 3DP";
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

Error NovaObjectData::export_3di_to_dir(const String &p_dir_path, int p_update_mask) {
	if (!has_ir || p_dir_path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	const String output_path = p_dir_path.path_join(_export_basename() + ".3di");
	if (source_kind == SourceKind::Threedi && has_source_model) {
		const Error err = _export_patched_3di(output_path);
		if (err == OK) {
			_clear_oed_dirty(UPDATE_ALL);
		}
		return err;
	}
	const uint8_t update_mask = _normalize_oed_update_mask(p_update_mask);
	return _export_project_backed_3di(output_path, update_mask);
}

Error NovaObjectData::_export_project_backed_3di(const String &p_path, uint8_t p_update_mask) {
	if (oed_session == nullptr) {
		last_error = "Object has no OED geometry session";
		return ERR_UNCONFIGURED;
	}

	TdpProject export_project = _build_project_from_ir();
	const std::string native_path = to_native_path(p_path);
	OedExportRequest request = {};
	request.project = &export_project;
	request.output_path = native_path.c_str();
	request.update_mask = p_update_mask;
	const OedStatus rc = oed_session_export(oed_session, &request);
	tdp_free(&export_project);
	if (rc != OED_STATUS_OK) {
		last_error = oed_error_detail(oed_session, "Failed to export 3DI");
		return ERR_FILE_CANT_WRITE;
	}
	_clear_oed_dirty(p_update_mask);
	return OK;
}

Error NovaObjectData::_apply_ir_to_source_model() {
	if (!has_ir || !has_source_model) {
		return ERR_UNCONFIGURED;
	}

	if (source_model.material_count != ir.material_count || source_model.materials == nullptr) {
		std::free(source_model.materials);
		source_model.materials = nullptr;
		source_model.material_count = static_cast<uint32_t>(ir.material_count);
		if (ir.material_count > 0) {
			source_model.materials = static_cast<ThreediMaterial *>(
					std::calloc(ir.material_count, sizeof(ThreediMaterial)));
			if (source_model.materials == nullptr) {
				return ERR_OUT_OF_MEMORY;
			}
		}
	}
	for (size_t i = 0; i < ir.material_count; ++i) {
		copy_ir_material(ir.materials[i], source_model.materials[i]);
	}

	if (source_model.light_count != ir.light_count || source_model.lights == nullptr) {
		std::free(source_model.lights);
		source_model.lights = nullptr;
		source_model.light_count = ir.light_count;
		if (ir.light_count > 0) {
			source_model.lights = static_cast<ThreediLight *>(std::calloc(ir.light_count, sizeof(ThreediLight)));
			if (source_model.lights == nullptr) {
				return ERR_OUT_OF_MEMORY;
			}
		}
	}
	for (size_t i = 0; i < ir.light_count; ++i) {
		copy_ir_light(ir.lights[i], source_model.lights[i]);
	}

	const size_t lod_count = std::min(source_model.lod_count, ir.lod_count);
	for (size_t lod_index = 0; lod_index < lod_count; ++lod_index) {
		ThreediLod &dst_lod = source_model.lods[lod_index];
		const ThreediIRLod &src_lod = ir.lods[lod_index];
		if (dst_lod.part_animation_count != src_lod.part_animation_count || dst_lod.part_animations == nullptr) {
			std::free(dst_lod.part_animations);
			dst_lod.part_animations = nullptr;
			dst_lod.part_animation_count = src_lod.part_animation_count;
			if (src_lod.part_animation_count > 0) {
				dst_lod.part_animations = static_cast<ThreediPartAnimation *>(
						std::calloc(src_lod.part_animation_count, sizeof(ThreediPartAnimation)));
				if (dst_lod.part_animations == nullptr) {
					return ERR_OUT_OF_MEMORY;
				}
			}
		}
		for (size_t i = 0; i < src_lod.part_animation_count; ++i) {
			copy_ir_part_animation(src_lod.part_animations[i], dst_lod.part_animations[i]);
		}
	}

	return OK;
}

Error NovaObjectData::_export_patched_3di(const String &p_path) {
	const Error apply_err = _apply_ir_to_source_model();
	if (apply_err != OK) {
		return apply_err;
	}
	const std::string native_path = to_native_path(p_path);
	if (threedi_3di3_write(native_path.c_str(), &source_model) != 0) {
		last_error = "Failed to write patched 3DI";
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

String NovaObjectData::_export_basename() const {
	return sanitized_basename(object_name);
}

String NovaObjectData::_source_kind_name() const {
	switch (source_kind) {
		case SourceKind::Threedi:
			return "3di";
		case SourceKind::Project:
			return "3dp";
		case SourceKind::Ase:
			return "ase";
		case SourceKind::Empty:
		default:
			return "empty";
	}
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
	if (key == "alpha_gen_start") { mat.alpha_gen.start = static_cast<int16_t>(std::clamp(static_cast<int>(p_value), -32768, 32767)); _notify_object_changed(UPDATE_MTRL); return true; }
	if (key == "alpha_gen_end") { mat.alpha_gen.end = static_cast<int16_t>(std::clamp(static_cast<int>(p_value), -32768, 32767)); _notify_object_changed(UPDATE_MTRL); return true; }
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

Array NovaObjectData::get_control_registers() const {
	Array result;
	if (!has_ir) {
		return result;
	}
	for (size_t i = 0; i < ir.control_register_count; ++i) {
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["name"] = from_native(ir.control_registers[i].name);
		result.push_back(item);
	}
	return result;
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

int NovaObjectData::get_light_count() const {
	return has_ir ? static_cast<int>(ir.light_count) : 0;
}

Array NovaObjectData::get_lights() const {
	Array result;
	if (!has_ir) {
		return result;
	}
	for (size_t i = 0; i < ir.light_count; ++i) {
		const ThreediIRLight &light = ir.lights[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["part_index"] = light.part_index;
		item["offset"] = godot_vec3(light.offset);
		item["attenuation_start"] = light.attenuation_start;
		item["attenuation_end"] = light.attenuation_end;
		item["color_start"] = Color(light.color_start[0], light.color_start[1], light.color_start[2]);
		item["color_end"] = Color(light.color_end[0], light.color_end[1], light.color_end[2]);
		item["style"] = light.style;
		item["phase"] = light.phase;
		item["rate"] = light.rate;
		item["flags"] = light.flags;
		item["falloff"] = light.falloff;
		item["type"] = light.light_type;
		result.push_back(item);
	}
	return result;
}

Dictionary NovaObjectData::get_light_info(int p_index) const {
	Dictionary info;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.light_count) {
		return info;
	}
	const ThreediIRLight &light = ir.lights[p_index];
	info["name"] = vformat("Light %d", p_index);
	info["position"] = godot_vec3(light.offset);
	info["atten_start"] = light.attenuation_start;
	info["atten_end"] = light.attenuation_end;
	info["color_start"] = Color(light.color_start[0], light.color_start[1], light.color_start[2], 1.0f);
	info["color_end"] = Color(light.color_end[0], light.color_end[1], light.color_end[2], 1.0f);
	info["falloff_deg"] = static_cast<int>(light.falloff);
	info["subobject"] = light.part_index;
	info["disable_corona"] = (light.flags & 0x01) != 0;
	info["disable_lightterrain"] = (light.flags & 0x02) != 0;
	info["disable_lightobjects"] = (light.flags & 0x04) != 0;
	info["colorgen_style"] = static_cast<int>(light.style);
	info["colorgen_phase"] = static_cast<int>(light.phase);
	info["colorgen_rate"] = static_cast<int>(light.rate);
	info["light_type"] = static_cast<int>(light.light_type);
	return info;
}

bool NovaObjectData::set_light_field(int p_index, const String &p_key, const Variant &p_value) {
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.light_count) {
		return false;
	}
	ThreediIRLight &light = ir.lights[p_index];
	const String key = p_key;
	auto set_flag = [&](uint8_t bit) {
		if (static_cast<bool>(p_value)) {
			light.flags |= bit;
		} else {
			light.flags &= ~bit;
		}
	};
	if (key == "position") {
		const Vector3 v = p_value;
		light.offset[0] = -v.x;
		light.offset[1] = v.y;
		light.offset[2] = v.z;
		_notify_object_changed(UPDATE_LGHT);
		return true;
	}
	if (key == "atten_start") { light.attenuation_start = static_cast<float>(p_value); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "atten_end") { light.attenuation_end = static_cast<float>(p_value); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "color_start") {
		const Color c = p_value;
		light.color_start[0] = c.r;
		light.color_start[1] = c.g;
		light.color_start[2] = c.b;
		_notify_object_changed(UPDATE_LGHT);
		return true;
	}
	if (key == "color_end") {
		const Color c = p_value;
		light.color_end[0] = c.r;
		light.color_end[1] = c.g;
		light.color_end[2] = c.b;
		_notify_object_changed(UPDATE_LGHT);
		return true;
	}
	if (key == "falloff_deg") { light.falloff = static_cast<float>(p_value); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "subobject") { light.part_index = static_cast<int32_t>(static_cast<int>(p_value)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "disable_corona") { set_flag(0x01); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "disable_lightterrain") { set_flag(0x02); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "disable_lightobjects") { set_flag(0x04); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "colorgen_style") { light.style = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "colorgen_phase") { light.phase = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "colorgen_rate") { light.rate = static_cast<uint16_t>(std::clamp(static_cast<int>(p_value), 0, 65535)); _notify_object_changed(UPDATE_LGHT); return true; }
	if (key == "light_type") { light.light_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255)); _notify_object_changed(UPDATE_LGHT); return true; }
	return false;
}

int NovaObjectData::get_user_point_count() const {
	return has_ir ? static_cast<int>(ir.userpoint_count) : 0;
}

Dictionary NovaObjectData::get_user_point_info(int p_index) const {
	Dictionary info;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.userpoint_count) {
		return info;
	}
	const ThreediIRUserPoint &point = ir.userpoints[p_index];
	info["name"] = from_native(point.name);
	info["position"] = godot_vec3(point.position);
	info["rotation"] = godot_vec3(point.direction);
	info["subobject"] = point.part_index;
	info["point_type"] = point.type_code;
	return info;
}

Vector3 NovaObjectData::get_ground_anchor(int p_lod_index) const {
	// The model-space point that should sit at a placed object's stored position:
	// the "ground" userpoint if present, else part 0's bounding center (see
	// threedi_ir_ground_anchor). The helper returns IR axis order; godot_vec3
	// applies the single negate-x that maps it into render/model space, exactly as
	// get_user_point_info / build_lod_submeshes do for userpoints and part origins.
	if (!has_ir) {
		return Vector3();
	}
	float anchor[3];
	if (!threedi_ir_ground_anchor(&ir, p_lod_index, anchor)) {
		return Vector3();
	}
	return godot_vec3(anchor);
}

bool NovaObjectData::_effective_panm_for_lod(int p_lod_index,
		std::vector<ThreediPartAnimation> &r_nodes) const {
	r_nodes.clear();
	if (!has_ir || ir.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= ir.lod_count)
		return false;
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.part_animation_count > 0 && lod.part_animations != nullptr) {
		r_nodes.resize(lod.part_animation_count);
		for (size_t i = 0; i < lod.part_animation_count; ++i)
			copy_ir_part_animation(lod.part_animations[i], r_nodes[i]);
	} else if (has_source_model &&
			source_model.part_animation_count > 0 &&
			source_model.part_animations != nullptr) {
		r_nodes.assign(source_model.part_animations,
				source_model.part_animations + source_model.part_animation_count);
	}
	return !r_nodes.empty();
}

bool NovaObjectData::has_live_panm_for_lod(int p_lod_index) const {
	if (!has_ir || ir.lods == nullptr || p_lod_index < 0 ||
			static_cast<size_t>(p_lod_index) >= ir.lod_count)
		return false;
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.part_count == 0 || lod.parts == nullptr) return false;
	std::vector<ThreediPartAnimation> nodes;
	_effective_panm_for_lod(p_lod_index, nodes);
	for (const ThreediPartAnimation &node : nodes)
		if (panm_animation_is_live(node)) return true;
	return false;
}

int NovaObjectData::get_live_panm_lod() const {
	if (!has_ir || ir.lods == nullptr) return -1;
	for (size_t lod_index = 0; lod_index < ir.lod_count; ++lod_index)
		if (has_live_panm_for_lod(static_cast<int>(lod_index)))
			return static_cast<int>(lod_index);
	return -1;
}

bool NovaObjectData::has_live_panm() const {
	return get_live_panm_lod() >= 0;
}

PackedInt32Array NovaObjectData::get_effective_panm_targets(int p_lod_index) const {
	PackedInt32Array out;
	std::vector<ThreediPartAnimation> nodes;
	_effective_panm_for_lod(p_lod_index, nodes);
	for (const ThreediPartAnimation &node : nodes)
		out.push_back(static_cast<int32_t>(node.subobject_index));
	return out;
}

int NovaObjectData::get_part_anim_count(int p_lod_index) const {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return 0;
	}
	return static_cast<int>(ir.lods[p_lod_index].part_animation_count);
}

bool NovaObjectData::has_collision() const {
	return has_ir && ir.collision != nullptr && ir.collision->volume_count > 0;
}

bool NovaObjectData::has_occlusion() const {
	return has_ir && ir.occlusion != nullptr && ir.occlusion->object_count > 0;
}

Array NovaObjectData::get_collision_volumes() const {
	// Expose the parsed collision bounding volumes (the engine's CB/CC collidable
	// primitives) in Godot model-local space. Each volume carries its AABB plus the
	// bounding planes that carve the convex region; callers build ConvexPolygonShape3D
	// hulls from them (editor picking now, runtime collision later).
	//
	// Coordinate frame: unlike render geometry (RDTA, which the importer stores already
	// converted to engine space, so the Godot boundary only needs godot_position's
	// negate-x), collision geometry (CVRT) is stored in *workspace* space with no
	// conversion -- confirmed in the OED exporter and validated here against the visual
	// mesh AABB. RDTA reaches engine space via (-y, z, x); composing that with
	// godot_position's negate-x gives the net workspace->Godot map (x, y, z) -> (y, z, x),
	// a pure cyclic axis rotation. Applying it makes a hull placed at the same transform
	// as the visual model coincide with it (empirically the best of the candidates: see
	// the Object Editor "Collision" overlay).
	Array out;
	if (!has_ir || ir.collision == nullptr) {
		return out;
	}
	const ThreediIRCollision *col = ir.collision;
	for (size_t i = 0; i < col->volume_count; ++i) {
		const ThreediIRCollisionVolume &v = col->volumes[i];
		// (x, y, z) -> (y, z, x); the cyclic rotation has no sign flips, so min stays min.
		const Vector3 gmin(v.min[1], v.min[2], v.min[0]);
		const Vector3 gmax(v.max[1], v.max[2], v.max[0]);
		Array planes;
		// plane_start / plane_count come straight from the on-disk model with no clamp
		// (threedi_ir_from_3di3), so a malformed file can make plane_count huge or plane_start out of
		// range. Planes are contiguous, so clamp the window to [0, col->plane_count) and iterate that
		// instead of spinning over billions of out-of-range indices; the arithmetic is 64-bit so
		// plane_start + plane_count cannot signed-overflow.
		const int64_t start = v.plane_start;
		const int64_t plane_total = static_cast<int64_t>(col->plane_count);
		const int64_t begin = start > 0 ? start : 0;
		int64_t end = start + static_cast<int64_t>(v.plane_count);
		if (end > plane_total) {
			end = plane_total;
		}
		for (int64_t idx = begin; idx < end; ++idx) {
			const ThreediIRCollisionPlane &pl = col->planes[static_cast<size_t>(idx)];
			// The map is orthonormal, so the normal rotates the same way and the plane's
			// perpendicular offset is preserved in magnitude. The stored convention is
			// `normal.dot(p) + distance == 0` (offset is the *negated* signed distance,
			// verified against the volume AABBs), whereas Godot's Plane(normal, d) means
			// `normal.dot(p) == d`; hence the negation.
			const Vector3 n(pl.normal[1], pl.normal[2], pl.normal[0]);
			planes.push_back(Plane(n, -pl.distance));
		}
		Dictionary d;
		d["type"] = v.type;
		d["flags"] = v.flags;
		d["min"] = gmin;
		d["max"] = gmax;
		d["planes"] = planes;
		d["part_index"] = v.part_index;
		d["object_index"] = v.object_index;
		out.push_back(d);
	}
	return out;
}

Array NovaObjectData::get_part_anim_editor_entries(int p_lod_index) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	for (size_t i = 0; i < lod.part_animation_count; ++i) {
		const ThreediIRPartAnimation &anim = lod.part_animations[i];
		const uint8_t scale_type = threedi_panm_scale_type(anim.flags);
		const uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
		const uint8_t translate_type = threedi_panm_translate_type(anim.flags);
		const bool rotation_enabled = rotation_type == 2;
		const bool scale_enabled = scale_type == 1 || scale_type == 2;
		const bool translation_enabled = translate_type >= THREEDI_TRANS_X && translate_type <= THREEDI_TRANS_Z;

		Dictionary rotation;
		rotation["enabled"] = rotation_enabled;
		rotation["reversed"] = threedi_panm_rotation_reversed(anim.flags) != 0;
		rotation["x"] = panm_axis_to_editor_dict(anim.rotation_x, ir, true);
		rotation["y"] = panm_axis_to_editor_dict(anim.rotation_y, ir, true);
		rotation["z"] = panm_axis_to_editor_dict(anim.rotation_z, ir, true);
		rotation["supported"] = rotation_type == 0 || rotation_type == 2;

		Dictionary scale;
		scale["enabled"] = scale_enabled;
		scale["style"] = panm_scale_style_for_type(scale_type);
		scale["x"] = panm_axis_to_editor_dict(anim.scale_x, ir, false);
		scale["y"] = panm_axis_to_editor_dict(anim.scale_y, ir, false);
		scale["z"] = panm_axis_to_editor_dict(anim.scale_z, ir, false);
		scale["supported"] = scale_type == 0 || scale_type == 1 || scale_type == 2;

		Dictionary translation;
		translation["enabled"] = translation_enabled;
		translation["axis"] = panm_axis_key_for_translate_type(translate_type);
		translation["track"] = panm_axis_to_editor_dict(anim.translation, ir, false);
		translation["supported"] = translate_type <= THREEDI_TRANS_Z;

		const bool rotation_supported = (rotation_type == 0 || rotation_type == 2) &&
				(!rotation_enabled ||
						(panm_control_supported(anim.rotation_x.control) &&
								panm_control_supported(anim.rotation_y.control) &&
								panm_control_supported(anim.rotation_z.control)));
		const bool scale_supported = (scale_type == 0 || scale_type == 1 || scale_type == 2) &&
				(!scale_enabled ||
						(panm_control_supported(anim.scale_x.control) &&
								panm_control_supported(anim.scale_y.control) &&
								panm_control_supported(anim.scale_z.control)));
		const bool translation_supported = translate_type <= THREEDI_TRANS_Z &&
				(!translation_enabled || panm_control_supported(anim.translation.control));
		const bool supported = rotation_supported && scale_supported && translation_supported;

		Array active_channels;
		if (rotation_enabled) {
			active_channels.push_back("Rotation");
		}
		if (scale_enabled) {
			active_channels.push_back("Scale");
		}
		if (translation_enabled) {
			active_channels.push_back(String("Translate ") + String(translation["axis"]).to_upper());
		}
		String channel_summary = "No channels";
		if (!active_channels.is_empty()) {
			PackedStringArray parts;
			for (int j = 0; j < active_channels.size(); ++j) {
				parts.push_back(String(active_channels[j]));
			}
			channel_summary = String(" + ").join(parts);
		}

		Dictionary entry;
		entry["index"] = static_cast<int64_t>(i);
		entry["target_part"] = static_cast<int>(anim.part_index);
		entry["target_part_ref"] = part_anim_part_ref(lod, anim.part_index);
		entry["target_part_label"] = part_anim_part_label(anim.part_index);
		entry["parent_part"] = static_cast<int>(anim.parent_part);
		entry["parent_part_ref"] = part_anim_part_ref(lod, anim.parent_part);
		entry["parent_part_label"] = part_anim_part_label(anim.parent_part);
		entry["rotation"] = rotation;
		entry["scale"] = scale;
		entry["translation"] = translation;
		entry["supported"] = supported;
		entry["unsupported_reason"] = supported ? String() : String("Unsupported PANM mode");
		entry["summary"] = supported ?
				vformat("%s -> %s | %s", entry["target_part_label"], entry["parent_part_label"], channel_summary) :
				String("Unsupported animation | Not editable");
		result.push_back(entry);
	}
	return result;
}

int NovaObjectData::add_part_anim(int p_lod_index, int p_part_index) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return -1;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count + 1;
	ThreediIRPartAnimation *next = static_cast<ThreediIRPartAnimation *>(
			std::calloc(new_count, sizeof(ThreediIRPartAnimation)));
	if (next == nullptr) {
		return -1;
	}
	if (lod.part_animations != nullptr && old_count > 0) {
		std::memcpy(next, lod.part_animations, old_count * sizeof(ThreediIRPartAnimation));
	}
	const int max_part = lod.part_count > 0 ? static_cast<int>(lod.part_count - 1) : 255;
	const int part_index = std::clamp(p_part_index, 0, std::min(max_part, 255));
	ThreediIRPartAnimation &anim = next[old_count];
	anim.part_index = static_cast<uint8_t>(part_index);
	anim.parent_part = 0;
	anim.matrix_index = static_cast<uint8_t>(part_index);
	anim.matrix_offset = 0;
	anim.bind_matrix_index = part_index;
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return static_cast<int>(old_count);
}

int NovaObjectData::duplicate_part_anim(int p_lod_index, int p_anim_index) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return -1;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count || lod.part_animations == nullptr) {
		return -1;
	}
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count + 1;
	ThreediIRPartAnimation *next = static_cast<ThreediIRPartAnimation *>(
			std::calloc(new_count, sizeof(ThreediIRPartAnimation)));
	if (next == nullptr) {
		return -1;
	}
	std::memcpy(next, lod.part_animations, old_count * sizeof(ThreediIRPartAnimation));
	next[old_count] = lod.part_animations[p_anim_index];
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return static_cast<int>(old_count);
}

bool NovaObjectData::delete_part_anim(int p_lod_index, int p_anim_index) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count || lod.part_animations == nullptr) {
		return false;
	}
	const size_t old_count = lod.part_animation_count;
	const size_t new_count = old_count - 1;
	ThreediIRPartAnimation *next = nullptr;
	if (new_count > 0) {
		next = static_cast<ThreediIRPartAnimation *>(
				std::calloc(new_count, sizeof(ThreediIRPartAnimation)));
		if (next == nullptr) {
			return false;
		}
		const size_t remove_index = static_cast<size_t>(p_anim_index);
		if (remove_index > 0) {
			std::memcpy(next, lod.part_animations, remove_index * sizeof(ThreediIRPartAnimation));
		}
		if (remove_index + 1 < old_count) {
			std::memcpy(next + remove_index,
					lod.part_animations + remove_index + 1,
					(old_count - remove_index - 1) * sizeof(ThreediIRPartAnimation));
		}
	}
	std::free(lod.part_animations);
	lod.part_animations = next;
	lod.part_animation_count = new_count;
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool NovaObjectData::set_part_anim_target(int p_lod_index, int p_anim_index, int p_part_index, int p_parent_part) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	const int max_part = lod.part_count > 0 ? static_cast<int>(lod.part_count - 1) : 255;
	const int part_index = std::clamp(p_part_index, 0, std::min(max_part, 255));
	const int parent_part = std::clamp(p_parent_part, 0, std::min(max_part, 255));
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	anim.part_index = static_cast<uint8_t>(part_index);
	anim.parent_part = static_cast<uint8_t>(parent_part);
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool NovaObjectData::set_part_anim_channel_enabled(int p_lod_index, int p_anim_index, const String &p_channel, bool p_enabled) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	uint8_t scale_type = threedi_panm_scale_type(anim.flags);
	uint8_t rotation_type = threedi_panm_rotation_type(anim.flags);
	uint8_t rotation_reversed = threedi_panm_rotation_reversed(anim.flags) ? 1 : 0;
	uint8_t translate_type = threedi_panm_translate_type(anim.flags);
	const String channel = p_channel.to_lower();
	if (channel == "rotation") {
		rotation_type = p_enabled ? 2 : 0;
	} else if (channel == "scale") {
		scale_type = p_enabled ? (scale_type == 2 ? 2 : 1) : 0;
	} else if (channel == "translation") {
		translate_type = p_enabled ? (translate_type == THREEDI_TRANS_NONE || translate_type > THREEDI_TRANS_Z ? THREEDI_TRANS_X : translate_type) : THREEDI_TRANS_NONE;
	} else {
		return false;
	}
	anim.flags = panm_pack_flags_from_parts(scale_type, rotation_type, rotation_reversed, translate_type);
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool NovaObjectData::set_part_anim_channel_mode(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, const String &p_mode, int p_control_register) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	uint8_t control = 0;
	if (!panm_control_for_mode(p_mode, control)) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediIRTransform *track = semantic_part_anim_track(anim, p_channel, p_axis);
	if (track == nullptr) {
		return false;
	}
	ensure_part_anim_channel_flag(anim, p_channel, p_axis);
	track->control = control;
	if (panm_control_uses_register(control)) {
		track->control_param = static_cast<uint8_t>(std::clamp(p_control_register, 0, 255));
	} else {
		track->control_param = 0;
	}
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool NovaObjectData::set_part_anim_channel_values(int p_lod_index, int p_anim_index, const String &p_channel, const String &p_axis, double p_from_value, double p_to_value, double p_speed) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediIRTransform *track = semantic_part_anim_track(anim, p_channel, p_axis);
	if (track == nullptr) {
		return false;
	}
	ensure_part_anim_channel_flag(anim, p_channel, p_axis);
	const bool is_rotation = p_channel.to_lower() == "rotation";
	const double unit = is_rotation ? kPanmRotationUnit : kPanmValueUnit;
	track->start = panm_clamp_i16_from_double(p_from_value / unit);
	track->end = panm_clamp_i16_from_double(p_to_value / unit);
	track->rate = panm_clamp_i16_from_double(p_speed / kPanmValueUnit);
	_notify_object_changed(UPDATE_PANM);
	return true;
}

bool NovaObjectData::set_part_anim_rotation_reversed(int p_lod_index, int p_anim_index, bool p_reversed) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	anim.flags = panm_pack_flags_from_parts(
			threedi_panm_scale_type(anim.flags),
			threedi_panm_rotation_type(anim.flags),
			p_reversed ? 1 : 0,
			threedi_panm_translate_type(anim.flags));
	_notify_object_changed(UPDATE_PANM);
	return true;
}

Array NovaObjectData::get_part_animations(int p_lod_index) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	for (size_t i = 0; i < lod.part_animation_count; ++i) {
		const ThreediIRPartAnimation &anim = lod.part_animations[i];
		Dictionary item;
		item["index"] = static_cast<int64_t>(i);
		item["flags"] = static_cast<int64_t>(anim.flags);
		item["parent_part"] = anim.parent_part;
		item["part_index"] = anim.part_index;
		item["matrix_index"] = anim.matrix_index;
		item["bind_matrix_index"] = anim.bind_matrix_index;
		result.push_back(item);
	}
	return result;
}

Dictionary NovaObjectData::get_part_anim_info(int p_lod_index, int p_anim_index) const {
	Dictionary info;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return info;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return info;
	}
	const ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	info["index"] = p_anim_index;
	info["transform_as"] = static_cast<int>(anim.part_index);
	info["parent_subobject"] = static_cast<int>(anim.parent_part);
	info["flags"] = static_cast<int64_t>(anim.flags);
	info["scale_type"] = static_cast<int>(anim.flags & 0xFFu);
	info["rotation_type"] = static_cast<int>((anim.flags >> 8) & 0xFFu);
	info["rotation_reversed"] = ((anim.flags >> 16) & 0xFFu) != 0;
	info["translate_type"] = static_cast<int>((anim.flags >> 24) & 0xFFu);
	info["rotation_x"] = transform_to_dict(anim.rotation_x, ir);
	info["rotation_y"] = transform_to_dict(anim.rotation_y, ir);
	info["rotation_z"] = transform_to_dict(anim.rotation_z, ir);
	info["scale_x"] = transform_to_dict(anim.scale_x, ir);
	info["scale_y"] = transform_to_dict(anim.scale_y, ir);
	info["scale_z"] = transform_to_dict(anim.scale_z, ir);
	info["translation"] = transform_to_dict(anim.translation, ir);
	return info;
}

bool NovaObjectData::set_part_anim_field(int p_lod_index, int p_anim_index, const String &p_key, const Variant &p_value) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	const String key = p_key;
	if (key == "transform_as") {
		anim.part_index = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "parent_subobject") {
		anim.parent_part = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "scale_type" || key == "rotation_type" || key == "translate_type" || key == "rotation_reversed") {
		uint8_t scale_type = static_cast<uint8_t>(anim.flags & 0xFFu);
		uint8_t rotation_type = static_cast<uint8_t>((anim.flags >> 8) & 0xFFu);
		uint8_t rotation_reversed = static_cast<uint8_t>((anim.flags >> 16) & 0xFFu);
		uint8_t translate_type = static_cast<uint8_t>((anim.flags >> 24) & 0xFFu);
		if (key == "scale_type") {
			scale_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		} else if (key == "rotation_type") {
			rotation_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		} else if (key == "translate_type") {
			translate_type = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		} else if (key == "rotation_reversed") {
			rotation_reversed = static_cast<bool>(p_value) ? 1 : 0;
		}
		anim.flags = static_cast<uint32_t>(scale_type) |
				(static_cast<uint32_t>(rotation_type) << 8) |
				(static_cast<uint32_t>(rotation_reversed) << 16) |
				(static_cast<uint32_t>(translate_type) << 24);
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	return false;
}

bool NovaObjectData::set_part_anim_track_field(int p_lod_index, int p_anim_index, const String &p_track, const String &p_key, const Variant &p_value) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return false;
	}
	ThreediIRPartAnimation &anim = lod.part_animations[p_anim_index];
	ThreediIRTransform *track = part_anim_track_for_name(anim, p_track);
	if (track == nullptr) {
		return false;
	}
	const String key = p_key.to_lower();
	if (key == "control" || key == "function") {
		track->control = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "control_param" || key == "reg") {
		track->control_param = static_cast<uint8_t>(std::clamp(static_cast<int>(p_value), 0, 255));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "reg_name") {
		int32_t reg = -1;
		if (!resolve_control_register_index(ir, String(p_value), reg) || reg < 0 || reg > 255) {
			return false;
		}
		track->control_param = static_cast<uint8_t>(reg);
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "rate") {
		track->rate = static_cast<int16_t>(std::clamp(static_cast<int>(p_value), -32768, 32767));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "start") {
		track->start = static_cast<int16_t>(std::clamp(static_cast<int>(p_value), -32768, 32767));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	if (key == "end") {
		track->end = static_cast<int16_t>(std::clamp(static_cast<int>(p_value), -32768, 32767));
		_notify_object_changed(UPDATE_PANM);
		return true;
	}
	return false;
}

Dictionary NovaObjectData::get_render_lod_info(int p_lod_index) const {
	Dictionary info;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return info;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	info["render_function"] = from_native(ir.render_function);
	info["threshold"] = lod.threshold;
	info["part_count"] = static_cast<int>(lod.part_count);
	info["render_object_count"] = static_cast<int>(lod.part_count);
	info["strip_count"] = static_cast<int>(lod.primitive_count);
	info["vertex_count"] = static_cast<int>(lod.vertex_count);
	info["index_count"] = static_cast<int>(lod.index_count);
	return info;
}

PackedVector3Array NovaObjectData::get_bone_origins(int p_lod_index) const {
	PackedVector3Array out;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return out;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	out.resize(static_cast<int64_t>(lod.part_count));
	for (size_t i = 0; i < lod.part_count; ++i) {
		const ThreediIRPart &part = lod.parts[i];
		// Raw native rel_position (parent-relative -- the parent-local FK offset the sampler wants),
		// NOT godot_vec3-flipped: bones stay engine-native (ADR 0007 conv #1 -- the mesh carries the
		// (-x,y,z) flip, the bones do not), matching how BadBone.position is consumed as-is by
		// sample_clip. Verified: this reproduces retail's modelDef+56 pivot (the rigid gun renders
		// correctly). [orig: BoneAnim_BuildWorldMatrices @0x40c400 reads the model pivot raw.]
		out[static_cast<int64_t>(i)] = Vector3(part.rel_position[0], part.rel_position[1], part.rel_position[2]);
	}
	return out;
}

PackedInt32Array NovaObjectData::get_bone_parents(int p_lod_index) const {
	PackedInt32Array out;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return out;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	out.resize(static_cast<int64_t>(lod.part_count));
	for (size_t i = 0; i < lod.part_count; ++i) {
		// Raw parent index (the root references itself in the file; the sampler normalizes).
		out[static_cast<int64_t>(i)] = lod.parts[i].parent_index;
	}
	return out;
}

bool NovaObjectData::is_skinned(int p_lod_index) const {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return false;
	}
	if (ir.mesh_type == THREEDI_IR_MESH_SKINNED) {
		return true;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.primitives == nullptr) {
		return false;
	}
	for (size_t i = 0; i < lod.primitive_count; ++i) {
		if (lod.primitives[i].bone_table_length > 0) {
			return true;
		}
	}
	return false;
}

Array NovaObjectData::get_lod_surfaces(int p_lod_index) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.vertices == nullptr || lod.indices == nullptr || lod.primitives == nullptr) {
		return result;
	}

	for (size_t prim_index = 0; prim_index < lod.primitive_count; ++prim_index) {
		const ThreediIRPrimitive &prim = lod.primitives[prim_index];
		std::vector<uint16_t> decoded_indices;
		if (!decode_primitive_indices(lod, prim, decoded_indices)) {
			continue;
		}

		bool has_tangents = true;
		for (uint32_t i = 0; i < prim.vertex_count; ++i) {
			if (!vertex_has_tangents(lod.vertices[prim.vertex_offset + i])) {
				has_tangents = false;
				break;
			}
		}

		PackedVector3Array vertices;
		PackedVector3Array normals;
		PackedVector2Array uvs;
		PackedVector2Array uvs2;
		PackedFloat32Array tangents;
		PackedInt32Array indices;
		// Per-vertex skinning, emitted only for skinned primitives. ARRAY_BONES carries 4
		// *skeleton* bone indices (the per-vertex bone_indices are local indices into this
		// primitive's bone_table, which maps local -> skeleton; we remap here so the host
		// can bind one whole-skeleton Skin). ARRAY_WEIGHTS carries the 4 matching weights.
		// [orig: the runtime skins via the .bad skeleton; bone_table is the per-strip remap.]
		const bool skinned = prim.bone_table_length > 0;
		PackedInt32Array bones;
		PackedFloat32Array weights;
		auto get_vertex = [&](uint16_t local_index) -> const ThreediIRVertex * {
			const uint32_t src_index = prim.vertex_offset + static_cast<uint32_t>(local_index);
			if (src_index >= lod.vertex_count) {
				return nullptr;
			}
			return &lod.vertices[src_index];
		};
		auto push_vertex = [&](const ThreediIRVertex &v) {
			const Vector3 normal = godot_normal(v);
			const Vector3 tangent(-v.tangent[0], v.tangent[1], v.tangent[2]);
			const Vector3 bitangent(-v.bitangent[0], v.bitangent[1], v.bitangent[2]);
			vertices.push_back(godot_position(v));
			normals.push_back(normal);
			uvs.push_back(Vector2(v.uv0[0], v.uv0[1]));
			uvs2.push_back(Vector2(v.uv1[0], v.uv1[1]));
			if (has_tangents) {
				const float w = normal.cross(tangent).dot(bitangent) < 0.0f ? -1.0f : 1.0f;
				tangents.push_back(tangent.x);
				tangents.push_back(tangent.y);
				tangents.push_back(tangent.z);
				tangents.push_back(w);
			}
			if (skinned) {
				for (int k = 0; k < 4; ++k) {
					const int local = static_cast<int>(v.bone_indices[k]);
					const int bone = (local >= 0 && local < prim.bone_table_length)
							? static_cast<int>(prim.bone_table[local])
							: 0;
					bones.push_back(bone);
				}
				float w0 = v.bone_weights[0];
				float w1 = v.bone_weights[1];
				float w2 = v.bone_weights[2];
				float w3 = v.bone_weights[3];
				float sum = w0 + w1 + w2 + w3;
				if (sum <= 1e-6f) {  // degenerate: pin fully to the first influence
					w0 = 1.0f;
					w1 = w2 = w3 = 0.0f;
					sum = 1.0f;
				}
				weights.push_back(w0 / sum);
				weights.push_back(w1 / sum);
				weights.push_back(w2 / sum);
				weights.push_back(w3 / sum);
			}
			indices.push_back(vertices.size() - 1);
		};
		auto push_triangle = [&](uint16_t a, uint16_t b, uint16_t c) {
			const ThreediIRVertex *va = get_vertex(a);
			const ThreediIRVertex *vb = get_vertex(b);
			const ThreediIRVertex *vc = get_vertex(c);
			if (va == nullptr || vb == nullptr || vc == nullptr) {
				return;
			}

			push_vertex(*va);
			push_vertex(*vb);
			push_vertex(*vc);
		};

		for (size_t i = 0; i + 2 < decoded_indices.size(); i += 3) {
			push_triangle(decoded_indices[i], decoded_indices[i + 1], decoded_indices[i + 2]);
		}

		if (vertices.is_empty()) {
			continue;
		}
		Dictionary surface;
		surface["primitive_index"] = static_cast<int64_t>(prim_index);
		surface["material_index"] = prim.material_index;
		surface["material_array_index"] = material_array_index_for_id(ir, prim.material_index);
		surface["part_index"] = prim.part_index;
		surface["vertex_offset"] = static_cast<int64_t>(prim.vertex_offset);
		surface["vertices"] = vertices;
		surface["normals"] = normals;
		surface["uvs"] = uvs;
		surface["uvs2"] = uvs2;
		if (!tangents.is_empty() && tangents.size() == vertices.size() * 4) {
			surface["tangents"] = tangents;
		}
		if (skinned && bones.size() == vertices.size() * 4 && weights.size() == vertices.size() * 4) {
			surface["bones"] = bones;
			surface["weights"] = weights;
			surface["is_skinned"] = true;
		}
		surface["indices"] = indices;
		result.push_back(surface);
	}
	return result;
}

uint64_t NovaObjectData::_submesh_cache_key(int p_lod_index, bool p_skeletal, int p_bone_count, bool p_native_frame) {
	return static_cast<uint64_t>(p_lod_index) |
			(static_cast<uint64_t>(p_skeletal ? 1 : 0) << 16) |
			(static_cast<uint64_t>(p_native_frame ? 1 : 0) << 17) |
			(static_cast<uint64_t>(p_bone_count) << 24);
}

Array NovaObjectData::build_lod_submeshes(int p_lod_index, bool p_skeletal, int p_bone_count,
		bool p_native_frame) const {
	Array result;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return result;
	}
	// Memo hit: hand back a deep copy of the ENTRY dictionaries (so a caller's
	// edits never taint the cache) whose ArrayMesh refs stay SHARED —
	// Array::duplicate(true) does not duplicate Resources, and that sharing is
	// the point: N models from one data render one set of meshes.
	const uint64_t cache_key = _submesh_cache_key(p_lod_index, p_skeletal, p_bone_count, p_native_frame);
	const auto cached = submesh_cache.find(cache_key);
	if (cached != submesh_cache.end()) {
		return cached->second.duplicate(true);
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	const Array surfaces = get_lod_surfaces(p_lod_index);
	for (int i = 0; i < surfaces.size(); ++i) {
		const Dictionary surface = surfaces[i];
		const int part_index = static_cast<int>(surface.get("part_index", 0));
		const int material_array_index = static_cast<int>(surface.get("material_array_index", surface.get("material_index", 0)));

		PackedVector3Array vertices = surface.get("vertices", PackedVector3Array());
		if (vertices.is_empty()) {
			continue;
		}
		PackedVector3Array normals = surface.get("normals", PackedVector3Array());
		PackedFloat32Array tangents = surface.get("tangents", PackedFloat32Array());
		PackedInt32Array mesh_indices = surface.get("indices", PackedInt32Array());
		if (p_native_frame) {
			// Undo the baked (-x,y,z) import flip: native positions/normals/tangents, and
			// reverse each triangle's winding — the source D3D clockwise-front order is only
			// CCW-correct for Godot BECAUSE of that mirror; unmirrored it must be re-reversed.
			// (See header: the FP viewmodel path, paired with NovaSkeletalAnim model_bind.)
			for (int v = 0; v < vertices.size(); ++v) {
				const Vector3 p = vertices[v];
				vertices.set(v, Vector3(-p.x, p.y, p.z));
			}
			for (int v = 0; v < normals.size(); ++v) {
				const Vector3 n = normals[v];
				normals.set(v, Vector3(-n.x, n.y, n.z));
			}
			for (int t = 0; t + 3 < tangents.size(); t += 4) {
				tangents.set(t, -tangents[t]);          // tangent x back to native
				tangents.set(t + 3, -tangents[t + 3]);  // bitangent handedness follows the mirror
			}
			for (int t = 0; t + 2 < mesh_indices.size(); t += 3) {
				const int32_t tmp = mesh_indices[t + 1];
				mesh_indices.set(t + 1, mesh_indices[t + 2]);
				mesh_indices.set(t + 2, tmp);
			}
		}

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		arrays[Mesh::ARRAY_NORMAL] = normals;
		arrays[Mesh::ARRAY_TEX_UV] = surface.get("uvs", PackedVector2Array());
		arrays[Mesh::ARRAY_TEX_UV2] = surface.get("uvs2", PackedVector2Array());
		if (!tangents.is_empty() && tangents.size() == vertices.size() * 4) {
			arrays[Mesh::ARRAY_TANGENT] = tangents;
		}
		// Skinning arrays (4 bones + 4 weights per vertex). ArrayMesh requires both present
		// together; only attach when both are valid for this surface's vertex count.
		PackedInt32Array bones = surface.get("bones", PackedInt32Array());
		PackedFloat32Array weights = surface.get("weights", PackedFloat32Array());
		bool surface_skinned = bones.size() == vertices.size() * 4 && weights.size() == vertices.size() * 4;
		// Rigid "fake skinning": when a skeleton will be applied (p_skeletal) but this surface
		// has no per-vertex skin, fully weight every vertex (1.0) to a single bone = the part's
		// subobject index. The .bad skeleton is authored so subobject i <-> bone i, so the rigid
		// part follows that bone. [orig: rigid weapon parts ride a bone via fake skinning.]
		if (!surface_skinned && p_skeletal) {
			const int bone = p_bone_count > 0 ? CLAMP(part_index, 0, p_bone_count - 1) : MAX(part_index, 0);
			const int vcount = static_cast<int>(vertices.size());
			bones.resize(vcount * 4);
			weights.resize(vcount * 4);
			for (int v = 0; v < vcount; ++v) {
				bones.set(v * 4 + 0, bone);
				bones.set(v * 4 + 1, 0);
				bones.set(v * 4 + 2, 0);
				bones.set(v * 4 + 3, 0);
				weights.set(v * 4 + 0, 1.0f);
				weights.set(v * 4 + 1, 0.0f);
				weights.set(v * 4 + 2, 0.0f);
				weights.set(v * 4 + 3, 0.0f);
			}
			surface_skinned = true;
		}
		if (surface_skinned) {
			arrays[Mesh::ARRAY_BONES] = bones;
			arrays[Mesh::ARRAY_WEIGHTS] = weights;
		}
		arrays[Mesh::ARRAY_INDEX] = mesh_indices;

		Ref<ArrayMesh> mesh;
		mesh.instantiate();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		mesh->surface_set_name(0, vformat("material_%d", material_array_index));

		Vector3 abs = Vector3();
		int parent_index = -1;
		if (part_index >= 0 && static_cast<size_t>(part_index) < lod.part_count) {
			const ThreediIRPart &part = lod.parts[part_index];
			abs = godot_vec3(part.abs_position);
			parent_index = part.parent_index;
		}

		const size_t prim_index = static_cast<size_t>(static_cast<int64_t>(surface.get("primitive_index", 0)));
		Dictionary entry;
		entry["robj_index"] = part_index;
		entry["part_index"] = part_index;
		entry["material_index"] = material_array_index;
		entry["source_material_index"] = surface.get("material_index", material_array_index);
		entry["is_alpha"] = prim_index < lod.primitive_count ? primitive_is_alpha(lod, prim_index) : false;
		entry["mesh"] = mesh;
		entry["abs"] = abs;
		entry["parent_index"] = parent_index;
		entry["primitive_index"] = surface.get("primitive_index", i);
		entry["is_skinned"] = surface_skinned;
		result.push_back(entry);
	}
	// Keep the pristine copy; the caller gets its own entry dictionaries. The
	// empty early-outs above are deliberately NOT cached.
	submesh_cache.emplace(cache_key, result);
	return result.duplicate(true);
}

Dictionary NovaObjectData::eval_material_runtime(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Dictionary out;
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return out;
	}
	ThreediMaterial material = {};
	copy_ir_material(ir.materials[p_index], material);
	const std::vector<std::string> ctrl_names = control_register_names(ir);
	const std::unordered_map<std::string, uint16_t> ctrl_values = control_values_from_dict(p_ctrl_values);
	const renderer::MaterialRuntime runtime = renderer::eval_material_runtime(
			material, retail_runtime_time_ms(p_time_ms), ctrl_names, ctrl_values);
	out["uv_offset"] = Vector2(runtime.uv.offset_u, runtime.uv.offset_v);
	out["uv_scale"] = Vector2(runtime.uv.scale_u, runtime.uv.scale_v);
	out["uv_rotation"] = runtime.uv.rotation;
	out["rgb_mod"] = Vector3(runtime.rgb_r, runtime.rgb_g, runtime.rgb_b);
	out["alpha_mod"] = runtime.alpha;
	return out;
}

int NovaObjectData::compute_anim_frame(int p_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	if (!has_ir || p_index < 0 || static_cast<size_t>(p_index) >= ir.material_count) {
		return 0;
	}
	ThreediMaterial material = {};
	copy_ir_material(ir.materials[p_index], material);
	const std::vector<std::string> ctrl_names = control_register_names(ir);
	const std::unordered_map<std::string, uint16_t> ctrl_values = control_values_from_dict(p_ctrl_values);
	return renderer::compute_anim_frame(material, 0,
			retail_runtime_time_ms(p_time_ms), ctrl_names, ctrl_values);
}

Dictionary NovaObjectData::evaluate_panm(int p_lod_index, int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Dictionary out;
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return out;
	}
	const ThreediIRLod &lod = ir.lods[p_lod_index];
	if (lod.part_count == 0 || lod.parts == nullptr) {
		return out;
	}

	uint16_t ctrl_table[512];
	std::memset(ctrl_table, 0, sizeof(ctrl_table));
	const Array keys = p_ctrl_values.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String key = keys[i];
		const std::string name = to_std(key);
		for (size_t r = 0; r < ir.control_register_count && r < 256; ++r) {
			if (name == ir.control_registers[r].name) {
				ctrl_table[r * 2] = static_cast<uint16_t>(std::clamp(static_cast<int>(p_ctrl_values[keys[i]]), 0, 65535));
				break;
			}
		}
	}

	std::vector<ThreediPartAnimation> effective_anims;
	_effective_panm_for_lod(p_lod_index, effective_anims);
	const ThreediPartAnimation *anims =
			effective_anims.empty() ? nullptr : effective_anims.data();
	const size_t node_count = effective_anims.size();

	size_t input_count = std::max(lod.part_count, node_count);
	for (size_t i = 0; i < node_count && anims != nullptr; ++i) {
		input_count = std::max(input_count, static_cast<size_t>(anims[i].subobject_index) + 1);
		input_count = std::max(input_count, static_cast<size_t>(anims[i].parent_subobject) + 1);
	}

	std::vector<ThreediMatrix4x4> base_transforms(input_count);
	std::vector<ThreediVec3> pivots(input_count, ThreediVec3{0, 0, 0});
	for (size_t i = 0; i < input_count; ++i) {
		threedi_mat4_identity(&base_transforms[i]);
	}
	for (size_t i = 0; i < lod.part_count; ++i) {
		const ThreediIRPart &part = lod.parts[i];
		base_transforms[i].m[12] = part.abs_position[0];
		base_transforms[i].m[13] = part.abs_position[1];
		base_transforms[i].m[14] = part.abs_position[2];
		pivots[i] = ThreediVec3{part.abs_position[0], part.abs_position[1], part.abs_position[2]};
	}

	std::vector<ThreediMatrix4x4> panm_matrices(node_count);
	std::vector<int> part_to_node(lod.part_count, -1);
	if (node_count > 0 && anims != nullptr) {
		const int rc = threedi_panm_build_node_matrices(
				anims,
				node_count,
				pivots.data(),
				nullptr,
				base_transforms.data(),
				nullptr,
				retail_runtime_time_ms(p_time_ms),
				ctrl_table,
				panm_matrices.data());
		if (rc == 0) {
			for (size_t i = 0; i < node_count; ++i) {
				const uint8_t sub = anims[i].subobject_index;
				if (sub < lod.part_count) {
					part_to_node[sub] = static_cast<int>(i);
				}
			}
		}
	}

	for (size_t i = 0; i < lod.part_count; ++i) {
		const int node_index = part_to_node[i];
		const ThreediMatrix4x4 &src = (node_index >= 0 && static_cast<size_t>(node_index) < panm_matrices.size())
				? panm_matrices[node_index]
				: base_transforms[i];
		out[static_cast<int>(i)] = panm_matrix_to_transform(src);
	}
	return out;
}

Array NovaObjectData::evaluate_lights(int64_t p_time_ms, const Dictionary &p_ctrl_values) const {
	Array out;
	if (!has_ir || ir.light_count == 0) {
		return out;
	}
	(void)p_ctrl_values;
	for (size_t i = 0; i < ir.light_count; ++i) {
		const ThreediIRLight &light = ir.lights[i];
		if ((light.flags & 0x04) != 0) {
			continue;
		}
		ThreediLight runtime_light = {};
		copy_ir_light(light, runtime_light);
		const std::array<uint8_t, 4> color_start = {
			runtime_light.color_start[0], runtime_light.color_start[1], runtime_light.color_start[2], runtime_light.color_start[3]
		};
		const std::array<uint8_t, 4> color_end = {
			runtime_light.color_end[0], runtime_light.color_end[1], runtime_light.color_end[2], runtime_light.color_end[3]
		};
		const renderer::LightRuntime runtime = renderer::eval_light_runtime(
				runtime_light.style,
				runtime_light.phase,
				runtime_light.rate,
				color_start,
				color_end,
				retail_runtime_time_ms(p_time_ms),
				0);
		Dictionary entry;
		entry["position"] = godot_vec3(light.offset);
		entry["color"] = Color(runtime.r, runtime.g, runtime.b, 1.0f);
		entry["intensity"] = runtime.intensity;
		entry["atten_start"] = light.attenuation_start;
		entry["atten_end"] = light.attenuation_end;
		entry["subobject"] = light.part_index;
		entry["disable_corona"] = (light.flags & 0x01) != 0;
		entry["disable_lightterrain"] = (light.flags & 0x02) != 0;
		entry["disable_lightobjects"] = false;
		out.push_back(entry);
	}
	return out;
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
	animation.cycle_frame_time = static_cast<int16_t>(std::clamp(dict_int(p_params, "cycle_frame_time", animation.cycle_frame_time), -32768, 32767));
	_notify_object_changed(UPDATE_MTRL);
	return OK;
}

Error NovaObjectData::set_light_colors(int p_light_index, const Color &p_start, const Color &p_end) {
	if (!has_ir || p_light_index < 0 || static_cast<size_t>(p_light_index) >= ir.light_count) {
		return ERR_INVALID_PARAMETER;
	}
	ThreediIRLight &light = ir.lights[p_light_index];
	light.color_start[0] = p_start.r;
	light.color_start[1] = p_start.g;
	light.color_start[2] = p_start.b;
	light.color_end[0] = p_end.r;
	light.color_end[1] = p_end.g;
	light.color_end[2] = p_end.b;
	_notify_object_changed(UPDATE_LGHT);
	return OK;
}

Error NovaObjectData::set_part_animation_flags(int p_lod_index, int p_anim_index, int p_flags) {
	if (!has_ir || p_lod_index < 0 || static_cast<size_t>(p_lod_index) >= ir.lod_count) {
		return ERR_INVALID_PARAMETER;
	}
	ThreediIRLod &lod = ir.lods[p_lod_index];
	if (p_anim_index < 0 || static_cast<size_t>(p_anim_index) >= lod.part_animation_count) {
		return ERR_INVALID_PARAMETER;
	}
	lod.part_animations[p_anim_index].flags = static_cast<uint32_t>(p_flags);
	_notify_object_changed(UPDATE_PANM);
	return OK;
}
