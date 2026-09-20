#pragma once

// The env document's float triples <-> godot::Vector3, component for
// component (no axis map: util/axes.h owns those), shared by the env bindings.

#include <godot_cpp/variant/vector3.hpp>

#include <formats/env/env.h>

namespace godot {

inline Vector3 to_vector3(const opennova::env::Rgb &rgb) {
	return Vector3(rgb.r, rgb.g, rgb.b);
}

inline Vector3 to_vector3(const opennova::env::Vec3 &v) {
	return Vector3(v.x, v.y, v.z);
}

inline opennova::env::Rgb to_rgb(const Vector3 &v) {
	return opennova::env::Rgb{
		static_cast<float>(v.x), static_cast<float>(v.y),
		static_cast<float>(v.z)};
}

} // namespace godot
