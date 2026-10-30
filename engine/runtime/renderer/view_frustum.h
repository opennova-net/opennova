#pragma once

// A view's clip planes and the bound-sphere test every view cull shares.
// Retail tests a sphere against its viewport's half-planes, each rejecting it
// once the sphere's signed distance falls below -radius: the depth leg past
// the near word and short of the far reach, then the four side planes through
// the eye [orig: Viewport_TransformAndClipPoint @ 0x4115e0 - the depth leg's
// rejects @ 0x41162f..0x41164a, the side planes' @ 0x4116a4, @ 0x4116e1,
// @ 0x411722, @ 0x411763; the side planes built by
// Viewport_BuildProjectionMatrix @ 0x4113b4..0x411571]. A view hands its eye,
// its unit forward/right/up triad, the half-angle tangents of the frustum it
// draws, and the near and far distances its depth leg runs between.

#include <cmath>

namespace opennova::renderer {

struct ViewFrustum {
	// Inward planes, a x + b y + c z + d >= 0 inside: the near plane, the four
	// sides (left, right, bottom, top), then the far plane when the view has
	// one. No plane (count 0): no clip state.
	float planes[6][4] = {};
	int count = 0;
};

// The frustum of a view at `eye` looking along `forward` (right and up its
// other unit axes) over the half-angle tangents `tan_half_horizontal` and
// `tan_half_vertical`; the near plane `near_distance` along the forward axis
// (0 passes it through the eye), the far plane `far_distance` along it (0 or
// less: none). A tangent that is not positive gives no frustum.
inline ViewFrustum view_frustum(const float eye[3], const float forward[3],
		const float right[3], const float up[3], float tan_half_horizontal,
		float tan_half_vertical, float near_distance, float far_distance) {
	ViewFrustum frustum;
	if (!(tan_half_horizontal > 0.0f) || !(tan_half_vertical > 0.0f)) {
		return frustum;
	}
	const float cos_h = 1.0f / std::sqrt(1.0f + tan_half_horizontal * tan_half_horizontal);
	const float sin_h = tan_half_horizontal * cos_h;
	const float cos_v = 1.0f / std::sqrt(1.0f + tan_half_vertical * tan_half_vertical);
	const float sin_v = tan_half_vertical * cos_v;
	const auto set_plane = [&frustum](const float normal[3], float d) {
		float *plane = frustum.planes[frustum.count++];
		plane[0] = normal[0];
		plane[1] = normal[1];
		plane[2] = normal[2];
		plane[3] = d;
	};
	const auto through_eye = [eye](const float normal[3]) {
		return -(normal[0] * eye[0] + normal[1] * eye[1] + normal[2] * eye[2]);
	};
	float normal[3];
	set_plane(forward, through_eye(forward) - near_distance);
	for (int i = 0; i < 3; ++i) normal[i] = right[i] * cos_h + forward[i] * sin_h;
	set_plane(normal, through_eye(normal)); // left
	for (int i = 0; i < 3; ++i) normal[i] = -right[i] * cos_h + forward[i] * sin_h;
	set_plane(normal, through_eye(normal)); // right
	for (int i = 0; i < 3; ++i) normal[i] = up[i] * cos_v + forward[i] * sin_v;
	set_plane(normal, through_eye(normal)); // bottom
	for (int i = 0; i < 3; ++i) normal[i] = -up[i] * cos_v + forward[i] * sin_v;
	set_plane(normal, through_eye(normal)); // top
	if (far_distance > 0.0f) {
		for (int i = 0; i < 3; ++i) normal[i] = -forward[i];
		set_plane(normal, through_eye(normal) + far_distance);
	}
	return frustum;
}

// Whether a sphere at `center` of `radius` touches the frustum: no plane's
// signed distance below -radius. A frustum with no planes passes everything.
inline bool view_frustum_sphere_visible(const ViewFrustum &frustum, const float center[3],
		float radius) {
	for (int index = 0; index < frustum.count; ++index) {
		const float *plane = frustum.planes[index];
		if (plane[0] * center[0] + plane[1] * center[1] + plane[2] * center[2] + plane[3] <
				-radius) {
			return false;
		}
	}
	return true;
}

} // namespace opennova::renderer
