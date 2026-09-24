#pragma once

#include <cstdint>
#include <runtime/renderer/object_lod.h>

#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

class Camera3D;

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

	// A vertical fov over a viewport of that size (the scripted seam's form):
	// the horizontal tangent follows the aspect and the viewport is the
	// focal's width.
	static ObjectLodFrame make(const Transform3D &p_camera_transform,
			float p_vertical_fov_degrees, float p_viewport_width,
			float p_viewport_height);
	// The frame a view draws: the frustum from its camera's own tangents
	// (camera_tangents), the focal and frame scale from the viewport width
	// the image reaches the surface at (retail's viewport is the whole
	// surface, and its focal is half that width over tan(fov_h / 2)).
	static ObjectLodFrame from_camera(const Camera3D *p_camera, float p_viewport_width);
	static ObjectLodFrame from_tangents(const Transform3D &p_camera_transform,
			float p_tan_half_horizontal, float p_tan_half_vertical,
			float p_viewport_width);

	// The half-angle tangents of the frustum a camera draws over its own
	// viewport: Godot's fov is horizontal under KEEP_WIDTH and vertical
	// otherwise, the other axis following the viewport aspect. False for a
	// null camera or an empty viewport.
	static bool camera_tangents(const Camera3D *p_camera, float &r_tan_half_horizontal,
			float &r_tan_half_vertical);

	// The largest axis scale of a basis: the uniform entity scale a bound
	// sphere radius is multiplied by.
	static float uniform_scale(const Basis &p_basis);

	// CMDL coordinates are source model axes: (x,y,z) maps to Godot (y,z,x).
	// The native sphere already includes the authored scale, so remove that
	// scale from the presentation basis before placing its offset center.
	static Vector3 projection_center(const Transform3D &p_transform,
			const opennova::renderer::ObjectProjectionSphere &p_sphere,
			int32_t p_entity_scale_q16);

	// Whether a world bound sphere touches the view frustum (the near/side
	// plane rejection alone, no projection): project()'s own first step.
	bool sphere_in_frustum(const Vector3 &p_center, float p_radius) const;

	// Project a world bound sphere. Returns false when the sphere lies wholly
	// outside the frustum (r_radius_q16 untouched); otherwise r_radius_q16 is
	// the engine's projected radius in Q16.16 pixels (a sphere the eye sits
	// inside reports the witnessed behind-eye radius).
	bool project(const Vector3 &p_center, float p_radius,
			int32_t &r_radius_q16) const;
	bool project_q16(const Vector3 &p_center, int32_t p_radius_q16,
			int32_t &r_projected_q16) const;
};

} // namespace godot
