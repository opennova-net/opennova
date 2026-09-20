#pragma once

// The engine particle types -> Godot value edges the particle bindings share
// (EffectScene, EffectWorld, ParticleRenderer, ParticleCompositorEffect).

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/particle/effect_scene.h>
#include <runtime/renderer/particle_frame.h>

#include <cstdint>
#include <cstring>

namespace godot {

// A 64-bit engine token / counter as Godot's int64, bit pattern preserved.
inline int64_t token_to_godot(std::uint64_t value) noexcept {
	int64_t result = 0;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

inline Vector3 godot_vector(const opennova::particle::Vec3 &value) noexcept {
	return Vector3(value.x, value.y, value.z);
}

inline Transform3D godot_pose(const opennova::particle::EffectPose &value) noexcept {
	Basis basis;
	basis.set_column(0, godot_vector(value.right));
	basis.set_column(1, godot_vector(value.up));
	basis.set_column(2, godot_vector(value.forward));
	return Transform3D(basis, godot_vector(value.position));
}

inline AABB godot_aabb(const opennova::renderer::ParticleAabb &bounds) {
	const Vector3 minimum(bounds.min.x, bounds.min.y, bounds.min.z);
	const Vector3 maximum(bounds.max.x, bounds.max.y, bounds.max.z);
	return AABB(minimum, maximum - minimum);
}

} // namespace godot
