#pragma once

// The engine particle types -> Godot value edges the particle bindings share
// (EffectScene, EffectWorld, ParticleRenderer, ParticleCompositorEffect).

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/particle/effect_scene.h>
#include <runtime/particle/emitter.h>
#include <runtime/renderer/particle_frame.h>

#include <cmath>
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

// A camera's six planes (Camera3D::get_frustum) as the simulator's clip state: Godot
// planes answer `normal . p - d`, the simulator wants `a x + b y + c z + d >= 0` inside.
// get_frustum points its normals outward, so a set the inside probe reads "under" is
// flipped; a set that already reads the probe inside is kept. Not valid unless six
// finite planes.
inline opennova::particle::ParticleViewFrustum particle_view_frustum(
		const TypedArray<Plane> &p_planes, const Vector3 &p_inside_probe) {
	opennova::particle::ParticleViewFrustum frustum;
	if (p_planes.size() != 6) {
		return frustum;
	}
	float sign = 1.0f;
	{
		const Plane first = p_planes[0];
		if (first.normal.dot(p_inside_probe) - first.d < 0.0f) {
			sign = -1.0f;
		}
	}
	for (int index = 0; index < 6; ++index) {
		const Plane plane = p_planes[index];
		if (!std::isfinite(plane.normal.x) || !std::isfinite(plane.normal.y) ||
				!std::isfinite(plane.normal.z) || !std::isfinite(plane.d)) {
			return opennova::particle::ParticleViewFrustum{};
		}
		frustum.planes[index][0] = sign * plane.normal.x;
		frustum.planes[index][1] = sign * plane.normal.y;
		frustum.planes[index][2] = sign * plane.normal.z;
		frustum.planes[index][3] = -sign * plane.d;
	}
	frustum.valid = true;
	return frustum;
}

} // namespace godot
