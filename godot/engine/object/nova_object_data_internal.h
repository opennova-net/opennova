// Internal header for the NovaObjectData translation-unit family
// (nova_object_data*.cpp) ONLY — one class, several TUs, split along the
// file's concern seams (document / bind / state / materials / geometry /
// panm edit / runtime eval). Carries the family's common includes plus every
// helper more than one TU uses, in namespace novaobj (each TU opens it with
// `using`). Not part of the engine's public include surface.
#pragma once

#include "object/nova_object_data.h"

#include "util/nova_string_convert.h"

#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>

using namespace godot;

namespace novaobj {

using opennova::to_std;

inline std::string to_native_path(const String &path) {
	String global = path;
	if (ProjectSettings::get_singleton() != nullptr) {
		global = ProjectSettings::get_singleton()->globalize_path(path);
	}
	return to_std(global);
}

inline String from_native(const char *value) {
	return String(value == nullptr ? "" : value);
}

inline void copy_cstr(char *dst, size_t dst_size, const char *src) {
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

inline uint8_t to_u8_color(float value) {
	const int rounded = static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
	return static_cast<uint8_t>(std::clamp(rounded, 0, 255));
}

inline uint8_t to_u8_255(float value) {
	const int rounded = static_cast<int>(std::lround(std::clamp(value, 0.0f, 255.0f)));
	return static_cast<uint8_t>(std::clamp(rounded, 0, 255));
}

inline int16_t clamp_to_i16(int value) {
	return static_cast<int16_t>(std::clamp<int>(value,
			std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
}

inline std::string resolve_relative_file(const String &dir, const char *filename) {
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

inline int project_lod_count(const TdpProject &project) {
	for (int i = 0; i < TDP_MAX_LODS; ++i) {
		if (project.lods[i].scene_file[0] == '\0') {
			return i;
		}
	}
	return TDP_MAX_LODS;
}

inline void copy_transform(const ThreediIRTransform &src, ThreediTransform &dst) {
	dst.control = src.control;
	dst.control_param = src.control_param;
	dst.rate = src.rate;
	dst.start = src.start;
	dst.end = src.end;
}

inline void copy_ir_part_animation(const ThreediIRPartAnimation &src, ThreediPartAnimation &dst) {
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

inline void copy_ir_material(const ThreediIRMaterial &src, ThreediMaterial &dst) {
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

inline void copy_ir_light(const ThreediIRLight &src, ThreediLight &dst) {
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

inline String control_register_name_for(const ThreediModelIR &ir, int32_t reg) {
	if (reg < 0 || static_cast<size_t>(reg) >= ir.control_register_count) {
		return String();
	}
	return from_native(ir.control_registers[reg].name);
}

inline bool resolve_control_register_index(const ThreediModelIR &ir, const String &name, int32_t &out_reg) {
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

inline Vector3 godot_vec3(const float v[3]) {
	return Vector3(-v[0], v[1], v[2]);
}

} // namespace novaobj
