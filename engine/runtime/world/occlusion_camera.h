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
//  g_EnvFogDistCurrent @ 0x26C681C; g_EnvWaterHeightFixed @ 0x26C6454; the
//  force-indoors attribute g_BmsAttribFlags & 0x10 @ 0x5ca1c8 -> |= 2]

#include <runtime/renderer/object_lod.h> // object_lod_focal_pixels
#include <runtime/world/collision.h> // kBlinkIndoorsBit
#include <runtime/world/geom.h>
#include <runtime/world/occlusion.h>
#include <runtime/world/presentation_frame.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <base/io/fixed.h>
#include <base/io/bam.h>

namespace opennova::world {

// The mission attribute that forces the indoors letter into every frame's
// accum view [orig: g_BmsAttribFlags & 0x10 @ 0x5ca1c8 -> |= 2].
inline constexpr uint32_t kMissionAttribForceIndoors = 0x10u;
inline bool mission_forces_indoors(uint32_t bms_attrib_flags) {
	return (bms_attrib_flags & kMissionAttribForceIndoors) != 0;
}

// The frame's letter view: the local player's blink letters with that attribute's
// indoors letter ORed in, the one word both the collect's camera and the frame's
// pass gates (renderer/scene_pass_gates.h) read [orig: g_BmsAttribFlags & 0x10
// @ 0x5ca1c8 -> accum |= 2].
inline uint32_t frame_blink_letters(uint32_t local_blink_flags, bool force_indoors) {
	return local_blink_flags | (force_indoors ? kBlinkIndoorsBit : 0u);
}

// The fov word's radians per unit (Q16 degrees) as the game stores it: 2.663160705566406e-07, a
// little under the exact pi / 180 / 65536 (2.663161090e-07) [orig: dbl_7C3620, the
// Viewport_BuildProjectionMatrix half-angle's multiplier @ 0x410fdb].
inline constexpr double kViewFovWordRadians = 0x1.1df467707c94dp-22;

// The shell's view, presentation frame: a unit forward/right/up triad, the
// eye, and the viewport the game builds the view over: its horizontal field of
// view (the fov word, Q16 degrees, here in degrees) and its rect's sides in
// pixels (the main view's the screen's, whatever the aspect mode stretches the
// drawn picture to; the Inset's its own; a water mirror's its square target),
// plus the environment words and the session's object detail the occlusion
// frame carries. No near distance: retail's occlusion planes pass through the
// eye and its projector carries its own near word (kViewportNearQ16,
// world/occlusion.h).
struct OcclusionViewSpec {
	float eye[3] = { 0.0f, 0.0f, 0.0f };
	float forward[3] = { 0.0f, 0.0f, -1.0f };
	float right[3] = { 1.0f, 0.0f, 0.0f };
	float up[3] = { 0.0f, 1.0f, 0.0f };
	float fov_h_deg = 90.0f;
	float viewport_width = 0.0f;
	float viewport_height = 0.0f;
	float fog_dist_units = 0.0f;
	float water_z_units = 0.0f;
	uint32_t local_blink_flags = 0;
	bool force_indoors = false;
	int32_t object_detail = renderer::kObjectLodDetailLevelMax;
};

// The view a water mirror's reflected subscene draws from: at or above the
// plane (presentation y = water_z_units) the eye mirrored in it with yaw kept
// and pitch and roll negated, below it the live view unchanged; the
// projection that follows the mirror's collect is built over that block, so
// the subscene's own collects (its death pieces) test against it. The triad
// takes environment/water_mirror.h build_water_mirror_view's proper mirror
// (forward and right reflected, the reflected up negated).
// [orig: Render_MainScene @ 0x5c1240 - `cmp eax, g_EnvWaterHeightFixed; jl`
//  @ 0x5c1367..0x5c1370, z' = 2wh - z @ 0x5c1379..0x5c137b, pitch and roll
//  negated @ 0x5c138e / @ 0x5c1390, the unchanged copy @ 0x5c13f6..0x5c1414;
//  the second Viewport_BuildProjectionMatrix @ 0x5c170b ahead of
//  Water_RenderReflectedWorldScene @ 0x5c171f]
inline OcclusionViewSpec mirror_reflected_view(const OcclusionViewSpec &view) {
	OcclusionViewSpec out = view;
	if (view.eye[1] < view.water_z_units) return out;
	out.eye[1] = 2.0f * view.water_z_units - view.eye[1];
	out.forward[1] = -view.forward[1];
	out.right[1] = -view.right[1];
	out.up[0] = -view.up[0];
	out.up[2] = -view.up[2];
	return out;
}

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
	//
	// The side planes are the viewport's, never the drawn frustum's: the
	// horizontal half-angle the fov word's half, the focal the rect's
	// half-width over its tangent rounded half up, the vertical half-angle the
	// rect's half-height (bottom - top + 2) >> 1 over that focal, each sine and
	// cosine truncated to Q22 and read back * 2^-22. So a view whose picture
	// the aspect mode stretches culls by the rect's own ratio (D-OCC-12).
	// [orig: Viewport_BuildProjectionMatrix @ 0x410fb0 - the half fov
	//  @ 0x410fc0..0x410fdb, the focal @ 0x410fe1..0x410ff7, the Q22 cosine and
	//  sine @ 0x410ffc..0x41101d, the half-height over the focal and its
	//  atan @ 0x411022..0x411041, its cosine and sine @ 0x411043..0x411056,
	//  the side planes' rows * flt_7C3610 (2^-22) @ 0x4113b4..0x411571]
	const int32_t fov_q16 = static_cast<int32_t>(std::lround(static_cast<double>(view.fov_h_deg) * 65536.0));
	const double half_h = static_cast<double>(fov_q16 >> 1) * kViewFovWordRadians;
	const int32_t width = std::max<int32_t>(1, static_cast<int32_t>(std::lround(view.viewport_width)));
	const int32_t height = std::max<int32_t>(1, static_cast<int32_t>(std::lround(view.viewport_height)));
	const double tan_h = std::tan(half_h);
	const int32_t focal = static_cast<int32_t>(static_cast<double>(width) * 0.5 / tan_h + 0.5);
	const double half_v = std::atan2(static_cast<double>((height + 1) >> 1), static_cast<double>(focal));
	const auto q22 = [](double v) {
		return static_cast<float>(static_cast<double>(static_cast<int32_t>(v * 4194304.0)) * (1.0 / 4194304.0));
	};
	const float ch = q22(std::cos(half_h)), sh = q22(std::sin(half_h));
	const float cv = q22(std::cos(half_v)), sv = q22(std::sin(half_v));
	float normals[5][3];
	for (int i = 0; i < 3; ++i) {
		normals[0][i] = f[i];
		normals[1][i] = r[i] * ch + f[i] * sh; // left
		normals[2][i] = -r[i] * ch + f[i] * sh; // right
		normals[3][i] = u[i] * cv + f[i] * sv; // bottom
		normals[4][i] = -u[i] * cv + f[i] * sv; // top
	}
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
	cam.focal_pixels = renderer::object_lod_focal_pixels(view.viewport_width, static_cast<float>(tan_h));
	cam.fog_dist = to_fixed(view.fog_dist_units);
	cam.water_z = to_fixed(view.water_z_units);
	// The mission-attribute force-indoors override ORs the indoors bit into
	// the frame's accum view.
	cam.local_blink_flags = frame_blink_letters(view.local_blink_flags, view.force_indoors);
	cam.object_detail = view.object_detail;
}

} // namespace opennova::world
