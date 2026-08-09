// Internal header for the ObjectData translation-unit family
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

inline String control_register_name_for(const Threedi3di3 &model, int32_t reg) {
	if (reg < 0 || static_cast<uint32_t>(reg) >= model.ctrl.count) {
		return String();
	}
	return from_native(model.ctrl.registers[reg].name);
}

inline bool resolve_control_register_index(const Threedi3di3 &model, const String &name, int32_t &out_reg) {
	if (name.is_empty()) {
		out_reg = -1;
		return true;
	}
	const std::string needle = to_std(name);
	for (uint32_t i = 0; i < model.ctrl.count; ++i) {
		if (needle == model.ctrl.registers[i].name) {
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
