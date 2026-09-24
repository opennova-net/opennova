// The overlay camera record (the dev tools' world-space overlays, ADR 0039
// d6 as amended: an overlay is a draw layer of its window, drawn by the
// engine on the Game window's ImGui draw list over the game image). The shell
// pushes the game viewport's active camera as ONE mission-frame
// view-projection — it composes projection x view x the presentation map
// once — so every overlay layer works in the mission frame the engine's
// records already carry (x east, y north, z up; world/presentation_frame.h).
//
// Header-only and ImGui-free: the projection math is shared by the overlay
// canvas and the shell's test seam and compiles in every flavour.
#pragma once

#include <algorithm>
#include <cmath>

namespace opennova::devtools {

struct OverlayCamera {
	bool valid = false;
	// Mission frame -> clip space, column-major (element [c * 4 + r]).
	float view_projection[16] = {};
	float eye[3] = {};     // mission frame
	float forward[3] = {}; // mission frame, unit
	float near_distance = 0.05f;
	// The viewport size the projection was built for; an image of another
	// size (a resize in flight) skips the overlay for that frame.
	int viewport_width = 0;
	int viewport_height = 0;
};

// The game image's screen rectangle the overlay maps onto.
struct OverlayRect {
	float min_x = 0.0f;
	float min_y = 0.0f;
	float max_x = 0.0f;
	float max_y = 0.0f;
	float width() const { return max_x - min_x; }
	float height() const { return max_y - min_y; }
};

// Signed distance of a mission point in front of the camera.
inline float overlay_view_depth(const OverlayCamera &camera, const float p[3]) {
	return (p[0] - camera.eye[0]) * camera.forward[0] + (p[1] - camera.eye[1]) * camera.forward[1] +
			(p[2] - camera.eye[2]) * camera.forward[2];
}

// Project a mission point onto the image rect. The pixel mapping matches
// Godot's Camera3D::unproject_position: x = min_x + (ndc.x * 0.5 + 0.5) * w,
// y = min_y + (0.5 - ndc.y * 0.5) * h. False behind the near plane.
inline bool overlay_project_point(const OverlayCamera &camera, const OverlayRect &rect,
		const float p[3], float out[2]) {
	if (!camera.valid || overlay_view_depth(camera, p) < camera.near_distance) return false;
	const float *m = camera.view_projection;
	const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
	const float cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
	const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
	if (!(cw > 1.0e-6f)) return false;
	const float nx = cx / cw;
	const float ny = cy / cw;
	out[0] = rect.min_x + (nx * 0.5f + 0.5f) * rect.width();
	out[1] = rect.min_y + (0.5f - ny * 0.5f) * rect.height();
	return std::isfinite(out[0]) && std::isfinite(out[1]);
}

// Project a mission segment: clipped against the near plane in view depth,
// then in 2D (Liang-Barsky) against the image rect widened by one image size
// on every side, so the draw list never receives runaway coordinates. False
// when nothing of the segment survives.
inline bool overlay_project_segment(const OverlayCamera &camera, const OverlayRect &rect,
		const float a[3], const float b[3], float out_a[2], float out_b[2]) {
	if (!camera.valid) return false;
	const float near = camera.near_distance;
	float pa[3] = {a[0], a[1], a[2]};
	float pb[3] = {b[0], b[1], b[2]};
	const float da = overlay_view_depth(camera, pa);
	const float db = overlay_view_depth(camera, pb);
	if (da < near && db < near) return false;
	if (da < near || db < near) {
		const float t = (near - da) / (db - da);
		float *moved = da < near ? pa : pb;
		for (int i = 0; i < 3; ++i) moved[i] = a[i] + (b[i] - a[i]) * t;
		// Nudge onto the visible side of the plane against rounding.
		const float nudge = near * 1.0e-3f;
		for (int i = 0; i < 3; ++i) moved[i] += camera.forward[i] * nudge;
	}
	float sa[2];
	float sb[2];
	if (!overlay_project_point(camera, rect, pa, sa) || !overlay_project_point(camera, rect, pb, sb)) {
		return false;
	}
	// Liang-Barsky against the guard band.
	const float gx0 = rect.min_x - rect.width();
	const float gx1 = rect.max_x + rect.width();
	const float gy0 = rect.min_y - rect.height();
	const float gy1 = rect.max_y + rect.height();
	const float dx = sb[0] - sa[0];
	const float dy = sb[1] - sa[1];
	float t0 = 0.0f;
	float t1 = 1.0f;
	const float p[4] = {-dx, dx, -dy, dy};
	const float q[4] = {sa[0] - gx0, gx1 - sa[0], sa[1] - gy0, gy1 - sa[1]};
	for (int i = 0; i < 4; ++i) {
		if (p[i] == 0.0f) {
			if (q[i] < 0.0f) return false;
			continue;
		}
		const float r = q[i] / p[i];
		if (p[i] < 0.0f) {
			t0 = std::max(t0, r);
		} else {
			t1 = std::min(t1, r);
		}
		if (t0 > t1) return false;
	}
	out_a[0] = sa[0] + dx * t0;
	out_a[1] = sa[1] + dy * t0;
	out_b[0] = sa[0] + dx * t1;
	out_b[1] = sa[1] + dy * t1;
	return true;
}

}  // namespace opennova::devtools
