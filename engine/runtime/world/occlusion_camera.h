#pragma once

// The occlusion frame camera built from the presenting scene's view: the
// mission-fixed eye, the render-float eye, the five inward-facing frustum
// planes through the eye (forward + four sides) the portal traversal is
// seeded with and the batch cull tests, the Q22 world->view rows the
// three-ray probe offsets along, and the fog/water/blink words. The shell
// hands over its camera as presentation-frame vectors (presentation_frame.h)
// and the numbers below are the engine's — the reimpl stand-in for the
// retail viewport build.
// [orig: g_CameraFrustumPlanes5 @ 0xA7849C (D-OCC-12 host mapping); the fixed
//  view matrix @ 0xA7841C; position @ 0xA78364 / flt_27219C0;
//  Env_FogDistCurrent @ 0x26C681C; Env_WaterHeightFixed @ 0x26C6454; the
//  force-indoors attribute Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> |= 2]

#include <runtime/renderer/object_lod.h> // object_lod_focal_pixels
#include <runtime/world/collision.h> // kBlinkIndoorsBit
#include <runtime/world/geom.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/presentation_frame.h>

#include <cmath>
#include <cstdint>
#include <base/io/fixed.h>
#include <base/io/bam.h>

namespace opennova::world {

// The shell's view, presentation frame: a unit forward/right/up triad, the
// eye, the vertical field of view, the aspect (tan_h / tan_v of the drawn
// frustum) and the viewport width in pixels, plus the environment words the
// occlusion frame carries. No near distance: retail's occlusion planes pass
// through the eye and its projector carries its own near word
// (kViewportNearQ16, world/occlusion.h).
struct OcclusionViewSpec {
	float eye[3] = { 0.0f, 0.0f, 0.0f };
	float forward[3] = { 0.0f, 0.0f, -1.0f };
	float right[3] = { 1.0f, 0.0f, 0.0f };
	float up[3] = { 0.0f, 1.0f, 0.0f };
	float fov_y_deg = 90.0f;
	float aspect = 1.0f;
	float viewport_width = 0.0f;
	float fog_dist_units = 0.0f;
	float water_z_units = 0.0f;
	uint32_t local_blink_flags = 0;
	bool force_indoors = false;
};

// Render float world from a presentation direction: the presentation frame
// with x and z swapped ((-my, mz, mx)/65536 == (pz, py, px)).
inline void render_dir_from_presentation(const float p[3], float out[3]) {
	out[0] = p[2];
	out[1] = p[1];
	out[2] = p[0];
}

// A presentation direction as Q22 mission-axis rows: mission (x, -z, y).
inline void mission_dir_q22_from_presentation(const float p[3], int32_t out[3]) {
	out[0] = static_cast<int32_t>(std::lround(p[0] * io::kQ22One));
	out[1] = static_cast<int32_t>(std::lround(-p[2] * io::kQ22One));
	out[2] = static_cast<int32_t>(std::lround(p[1] * io::kQ22One));
}

namespace detail {
inline void normalize3(float v[3]) {
	const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len <= 0.0f) return;
	v[0] /= len;
	v[1] /= len;
	v[2] /= len;
}
inline float dot3(const float a[3], const float b[3]) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
} // namespace detail

inline void occlusion_camera_from_view(const OcclusionViewSpec &view,
		OcclusionFrameCamera &cam) {
	// The eye: presentation -> mission fixed, then the render float mirror.
	float eye_mission[3];
	mission_from_presentation(view.eye, eye_mission);
	cam.pos_fixed[0] = to_fixed(eye_mission[0]);
	cam.pos_fixed[1] = to_fixed(eye_mission[1]);
	cam.pos_fixed[2] = to_fixed(eye_mission[2]);
	render_float_from_fixed(cam.pos_fixed, cam.pos_float);

	float f[3], r[3], u[3];
	render_dir_from_presentation(view.forward, f);
	render_dir_from_presentation(view.right, r);
	render_dir_from_presentation(view.up, u);

	// The 5-plane view frustum, inward normals, render float: retail's
	// g_CameraFrustumPlanes5 (viewport+0x13C..+0x188), the planes the portal
	// traversal is seeded with. EVERY plane passes through the eye,
	// d = -(n . eye) — the forward plane too: it rejects only what lies
	// behind the eye, never a near distance, so an exterior door a hair in
	// front of the eye still clips in and banks its window wedge. (With the
	// shell camera's 0.2 u near here, the camera's own door vanished inside
	// that band and every outdoor candidate failed the camera-inside rule —
	// the Ghost Harvest spawn and every doorway crossing.) Retail stores the
	// forward plane last; it sits at index 0 here — the traversal's clip test
	// and wedge assembly read the planes as a set.
	// [orig: Viewport_BuildProjectionMatrix — the side planes
	//  @ 0x4113b4..0x411571, the forward plane (0,0,1) through the inverse
	//  view @ 0x411577..0x41158c with d = -(n . eye) @ 0x4115b0..0x4115c6;
	//  the seeds Terrain_TraversePortalsFromSection @ 0x5c7448..0x5c745b /
	//  Terrain_TraversePortalsFromExterior @ 0x5c7377]
	const double half_v = static_cast<double>(view.fov_y_deg) * io::kRadiansPerDegree * 0.5;
	const double tan_v = std::tan(half_v);
	const double tan_h = tan_v * (view.aspect > 0.0f ? static_cast<double>(view.aspect) : 1.0);
	const float th = static_cast<float>(tan_h);
	const float tv = static_cast<float>(tan_v);
	float normals[5][3];
	for (int i = 0; i < 3; ++i) {
		normals[0][i] = f[i];
		normals[1][i] = f[i] * th + r[i]; // left
		normals[2][i] = f[i] * th - r[i]; // right
		normals[3][i] = f[i] * tv + u[i]; // bottom
		normals[4][i] = f[i] * tv - u[i]; // top
	}
	for (int i = 1; i < 5; ++i) detail::normalize3(normals[i]);
	cam.frustum_count = 5;
	for (int i = 0; i < 5; ++i) {
		cam.frustum[i][0] = normals[i][0];
		cam.frustum[i][1] = normals[i][1];
		cam.frustum[i][2] = normals[i][2];
		cam.frustum[i][3] = -detail::dot3(normals[i], cam.pos_float);
	}

	// World->view rotation rows (mission axes, Q22): row 0 = forward (the
	// depth cull axis), rows 1/2 = the lateral axes.
	mission_dir_q22_from_presentation(view.forward, cam.view_rows_q22[0]);
	mission_dir_q22_from_presentation(view.right, cam.view_rows_q22[1]);
	mission_dir_q22_from_presentation(view.up, cam.view_rows_q22[2]);

	// The projector's focal length over the viewport width and the horizontal
	// half-angle (the person leg's sub-pixel floor reads it).
	cam.focal_pixels = renderer::object_lod_focal_pixels(view.viewport_width, tan_h);
	cam.fog_dist = to_fixed(view.fog_dist_units);
	cam.water_z = to_fixed(view.water_z_units);
	// The mission-attribute force-indoors override ORs the indoors bit into
	// the frame's accum view.
	cam.local_blink_flags = view.local_blink_flags |
			(view.force_indoors ? kBlinkIndoorsBit : 0u);
}

} // namespace opennova::world
