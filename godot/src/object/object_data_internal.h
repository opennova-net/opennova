// Internal header for the ObjectData translation-unit family
// (object_data*.cpp) ONLY — one class, several TUs, split along the
// file's concern seams (load / bind / inspection / materials / geometry /
// runtime eval). Carries the family's common includes plus every
// helper more than one TU uses, in namespace novaobj (each TU opens it with
// `using`). Not part of the engine's public include surface.
#pragma once

#include "object/object_data.h"

#include "util/string_convert.h"

#include <godot_cpp/classes/project_settings.hpp>

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

inline String control_register_name_for(const opennova::threedi::Threedi3di3 &model, int32_t reg) {
	if (reg < 0 || static_cast<uint32_t>(reg) >= model.ctrl.count) {
		return String();
	}
	return from_native(model.ctrl.registers[reg].name);
}

inline Vector3 godot_vec3(const float v[3]) {
	return Vector3(-v[0], v[1], v[2]);
}

} // namespace novaobj
