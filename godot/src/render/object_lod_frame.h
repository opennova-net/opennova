#pragma once

#include <cstdint>

#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

// One display frame's camera terms shared by every authored-RLOD evaluation:
// the individual ObjectModels (ObjectModel::update_authored_lods) and the
// placer's retained static instances (MissionObjectPlacer::update_static_lods).
// The frame scale, the projected radius, the sub-pixel floor and the selector
// are engine facts (runtime/renderer/object_lod.h); this struct only converts
// the Godot camera into the integers the engine consumes and rejects bound
// spheres entirely outside the view frustum so an off-screen model keeps its
// level without an evaluation.
struct ObjectLodFrame {
	Vector3 origin;
	Vector3 forward;
	Vector3 right;
	Vector3 up;
	float tan_half_vertical = 0.0f;
	float tan_half_horizontal = 0.0f;
	// The frustum side planes' 1/cos terms: a sphere is outside a side plane
	// when its axis offset past the plane exceeds radius * this factor.
	float vertical_plane_scale = 1.0f;
	float horizontal_plane_scale = 1.0f;
	int32_t focal_pixels = 0;
	float projection_scale = 0.0f;
	bool valid = false;

	static ObjectLodFrame make(const Transform3D &p_camera_transform,
			float p_vertical_fov_degrees, float p_viewport_width,
			float p_viewport_height);

	// The largest axis scale of a basis: the uniform entity scale a bound
	// sphere radius is multiplied by.
	static float uniform_scale(const Basis &p_basis);

	// Whether a world bound sphere touches the view frustum (the near/side
	// plane rejection alone, no projection): the coarse test a group of
	// instances (a 512-unit static cell) runs before any member is projected.
	bool sphere_in_frustum(const Vector3 &p_center, float p_radius) const;

	// Project a world bound sphere. Returns false when the sphere lies wholly
	// outside the frustum (r_radius_q16 untouched); otherwise r_radius_q16 is
	// the engine's projected radius in Q16.16 pixels (a sphere the eye sits
	// inside reports the witnessed behind-eye radius).
	bool project(const Vector3 &p_center, float p_radius,
			int32_t &r_radius_q16) const;
};

} // namespace godot
