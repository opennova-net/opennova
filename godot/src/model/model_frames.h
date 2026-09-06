#pragma once

// The axis maps between the 3DI3 on-disk frames and the presentation frame
// Godot shows, as Godot vectors. One owner per map lives in the engine
// (formats/threedi/threedi_build.h); this header only spells them with
// Vector3 so the projector and the exporter cannot drift apart.

#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>

#include <formats/threedi/threedi_build.h>

namespace godot {

inline Vector3 vec3_from_build(const opennova::threedi::ThreediBuildVec3 &v) {
	return Vector3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z));
}

inline opennova::threedi::ThreediBuildVec3 build_from_vec3(const Vector3 &v) {
	return opennova::threedi::ThreediBuildVec3{v.x, v.y, v.z};
}

// MODEL axes (VERT/STRP/ROBJ/LGHT) <-> presentation.
inline Vector3 presentation_from_model(float x, float y, float z) {
	return vec3_from_build(opennova::threedi::threedi_model_to_presentation(
			opennova::threedi::ThreediBuildVec3{x, y, z}));
}

inline opennova::threedi::ThreediBuildVec3 model_from_presentation(const Vector3 &p) {
	return opennova::threedi::threedi_presentation_to_model(build_from_vec3(p));
}

// MISSION axes (collision, user points) <-> presentation.
inline Vector3 presentation_from_mission(double x, double y, double z) {
	return vec3_from_build(opennova::threedi::threedi_mission_to_presentation(
			opennova::threedi::ThreediBuildVec3{x, y, z}));
}

inline opennova::threedi::ThreediBuildVec3 mission_from_presentation(const Vector3 &p) {
	return opennova::threedi::threedi_presentation_to_mission(build_from_vec3(p));
}

// Mission 16.16 integers -> presentation.
inline Vector3 presentation_from_q16(int32_t x, int32_t y, int32_t z) {
	return presentation_from_mission(x / 65536.0, y / 65536.0, z / 65536.0);
}

} // namespace godot
