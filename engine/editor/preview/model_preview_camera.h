#pragma once

#include <cstdint>

#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

// How far the orbit camera's pitch goes either way (radians): just short of straight up or
// down, where its axes would turn over.
inline constexpr float kOrbitPitchLimit = 1.55f;

// A point in the model preview's space: the frame the renderer draws a model in (the
// model's own axes with x mirrored, y up: renderer/model_mesh_prepare), the model at the
// origin unrotated.
struct PreviewVec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

// A model-space point (a user point, a light, a pivot) in the preview's space.
inline PreviewVec3 preview_from_model(const float model[3]) { return PreviewVec3{-model[0], model[1], model[2]}; }

// The model preview's camera (ADR 0046 S10p2): it orbits a target at a distance, yaw about
// the vertical (0 looks along -z) and pitch the eye's elevation (positive looks down). It
// sees with the game's horizontal field of view (world::kPlayerCameraFovHDeg) across the
// device's width, so a model at a distance is as many pixels wide as the game draws it
// at that distance and width, and Auto picks the level the game would. The device's
// camera takes the same eye and axes (keep-width), and the overlays project through
// project(), so a marker sits on the pixel the renderer drew.
struct OrbitCamera {
	PreviewVec3 target;
	float yaw = 0.6f;
	float pitch = 0.35f;
	float distance = 5.0f;
	float near_plane = 0.05f;
	float far_plane = 500.0f;

	static float fov_horizontal_degrees();
	// The eye and its axes: right, up, and back (the eye looks along -back).
	PreviewVec3 eye() const;
	void axes(PreviewVec3 &right, PreviewVec3 &up, PreviewVec3 &back) const;
	// The focal length in pixels across a device `width` wide.
	static float focal_pixels(int width);
	// A point on the device (pixels from its top left) and its depth in front of the eye;
	// false behind the near plane.
	bool project(const PreviewVec3 &point, int width, int height, float &x, float &y, float *depth = nullptr) const;
	// The ray through device pixel (x, y): from the eye, along `direction` (not normalized: one
	// unit of it is one unit ahead of the eye); false for a device with no size.
	bool ray(float x, float y, int width, int height, PreviewVec3 &from, PreviewVec3 &direction) const;
	// The point under device pixel (x, y) on the plane through `through` that faces the eye
	// (a dragged marker keeps its depth); false when the ray misses it.
	bool on_view_plane(float x, float y, int width, int height, const PreviewVec3 &through, PreviewVec3 &out) const;
	// Look at a sphere so it fills the view (the device's aspect), from the current angles.
	void frame(const PreviewVec3 &center, float radius, int width, int height);
	// Mouse gestures in device pixels: orbit turns the angles, pan slides the target in
	// the view plane (the target stays under the mouse), dolly scales the distance.
	void orbit(float dx, float dy);
	void pan(float dx, float dy, int width);
	void dolly(float factor);
};

// The sphere a preview frames a model on: the sphere the game projects to pick its
// level (the collision block's bounds, world::collision_projection_sphere_from_3di), or
// the header's radius about the origin when that is empty.
void model_preview_sphere(const threedi::Threedi3di3 &model, PreviewVec3 &center, float &radius);

// The level the game draws `model` at through `camera` on a device `width` wide: its
// projection sphere projected the way the sector draw projects it, and the level walk over
// the model's thresholds at the highest detail profile [orig: Model_SelectRlodLevel @
// 0x5c3b20 via renderer::select_object_lod; Terrain_RenderWorldScene @ 0x5c944c for the
// frame scale]. `projected_q16` (optional): the projected radius in Q16.16 pixels. -1 for
// a model with no level; the last level when the walk selects none.
int model_preview_auto_lod(const threedi::Threedi3di3 &model, const OrbitCamera &camera, int width,
                           int32_t *projected_q16 = nullptr);

} // namespace opennova::editor
