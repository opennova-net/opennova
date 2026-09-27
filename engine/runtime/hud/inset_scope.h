#pragma once
#include <array>
namespace opennova::hud {
// Weapon Inset terrain inset geometry. 33 paired stops, BAM step 0x08000000;
// the initial half-table step truncates to index zero. The strip between the
// inner ring and the doubled outer ring is NOT a visible band: it is black
// drawn additively (one / one) with depth writes on and the depth compare off,
// a near-depth mask that clips the square scene pass that follows it to the
// circle. The second strip fills the disc opaque with the fog / water colour at
// far depth. The shell clips by drawing the scene target through the inner
// polygon only.
// [orig: Render_WeaponInsetScene @0x5C9740; bounds @0x5C9753..0x5C9821;
// fov @0x5C98EA; near @0x5C992D; strips @0x5C9AE8..0x5C9CD2 (mask strip render
// state 6 = mode 0x222 with pass flags 0x600000, CGfxShader_ApplyPass
// @0x683221..0x683249; the scene pass @0x5C9DE9 runs after both)]
struct InsetScopePoint {
	float x = 0, y = 0;
};
struct InsetScopeGeometry {
	int left = 0, top = 0, right = 0, bottom = 0;
	float fov_h_deg = 0;
	float center_x = 0, center_y = 0, radius_x = 0, radius_y = 0, stroke = 1;
	int cross_dx = 0, cross_dy = 0;
	std::array<InsetScopePoint, 33> inner{}, outer{}, uv{};
	bool valid = false;
};
InsetScopeGeometry inset_scope_geometry(
		float width, float height, int aspect_mode, float current_fov_over_zoom);
} // namespace opennova::hud
