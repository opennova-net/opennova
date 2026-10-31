#pragma once

// THE HUD'S PROJECTION: the main viewport's words the HUD's projector reads, as the scene frame
// builds them from its view block, and the projector itself. Every world point the HUD draws a
// label, a marker or a cue at goes through it: the attach labels, the friendly tags, the
// targeting and vehicle cues, the vehicle-bay logos and the capture-point labels.
// [orig: Viewport_BuildProjectionMatrix @0x410FB0 over g_MainViewport @0xA78360, rebuilt from
//  the main scene's block after the NVG and Inset passes (Render_ProcessMainSceneFrame
//  @0x5CA5D2 / @0x5CA958) so the HUD pass reads the main view's; the focal word the pass
//  stores, HUD_RenderAllOverlays @0x5A813D..0x5A8152; Math_FixedPointTransformPoint22
//  @0x615810 (world::CollisionMatrix::transform_point) then HUD_ClipPointToFrustumAndProject
//  @0x591E50]

#include <array>
#include <cstdint>

#include <runtime/hud/hud_combat.h> // HudProjectedPoint
#include <runtime/world/collision_model.h> // CollisionMatrix: the fixed view matrix

namespace opennova::world {
struct PlayerCameraPose;
}

namespace opennova::hud {

// The view block a viewport build reads: the eye (16.16 mission units), the three view angles
// (BAM, the yaw the heading), the horizontal field of view word (Q16 degrees) and the
// viewport's inclusive pixel rect.
// [orig: Viewport_BuildProjectionMatrix @0x410FB0 - the block's +4..+0xC eye, +0x10..+0x18
//  angles, +0x24..+0x30 rect, +0x3C fov]
struct HudViewBlock {
	std::array<int32_t, 3> pos_q16{};
	int32_t yaw_bam = 0;
	int32_t pitch_bam = 0;
	int32_t roll_bam = 0;
	int32_t fov_q16 = 0;
	int32_t left = 0, top = 0, right = -1, bottom = -1;
};

// The viewport's words the projector reads. The view space is retail's: x forward (the depth),
// y left, z up. Each side plane passes through the eye (distance 0), its normal Q22.
struct HudViewport {
	bool valid = false;
	// The world->view matrix: Q22 rotation rows, 16.16 translation [orig: viewport+0xBC,
	// g_ViewportViewMatrixFixed @0xA7841C].
	world::CollisionMatrix view;
	// The near distance the projector culls short of [orig: `mov [ebx+78h], 800h` @0x411093,
	// read as dword_A783D8].
	int32_t near_q16 = 0x800;
	// The four side planes in the projector's test order: the top (its flag 4), the bottom (8),
	// the left (1) and the right (2), each {nx, ny, nz, d} [orig: viewport+0x9C, +0xAC, +0x7C,
	// +0x8C, read at 0xA783FC, 0xA7840C, 0xA783DC, 0xA783EC].
	std::array<std::array<int32_t, 4>, 4> planes{};
	// The rect's centre, 16.16 pixels [orig: viewport+0x4C / +0x50, dword_A783AC / dword_A783B0].
	int32_t center_x_q16 = 0;
	int32_t center_y_q16 = 0;
	// The focal length, 16.16 pixels: the viewport's +0x40 the HUD pass shifts into
	// dword_2723EA4 [orig: HUD_RenderAllOverlays @0x5A813D / @0x5A8152].
	int32_t focal_q16 = 0;
	// The overlay surface's sides, the edges a clipped point is forced onto
	// [orig: g_OverlayCtx @0x24C1420 / dword_24C1424].
	int32_t surface_w = 0;
	int32_t surface_h = 0;
	// The vertical stretch the projector divides the up axis by (flt_8409E8)
	// [orig: Render_GetViewportScaleY @0x58A910].
	float scale_y = 1.0f;
	// The view block's eye, the words a label measures its distance from (g_ViewPos): the
	// block without the main scene's optics offsets, which land in the viewport only
	// [orig: HUD_DrawEntityLabel @0x5A3A9C..0x5A3AB0].
	std::array<int32_t, 3> view_pos_q16{};
};

// The viewport over a view block, the HUD's words of the build: the focal and the side planes
// from the fov word and the rect, the centre, the near word and the fixed view matrix
// rotX(roll) * rotY(pitch) * rotZ(yaw) * T(-eye).
// [orig: Viewport_BuildProjectionMatrix @0x410FB0 - the focal @0x410FC0..0x410FF7, the
//  half-angles' Q22 cosines and sines @0x410FFC..0x411056, the planes @0x41106E..0x4110F2,
//  the centre @0x411114..0x411131, the angles' Q22 sines and cosines @0x41115C..0x4111ED,
//  the matrix @0x4111F0..0x411298]
HudViewport hud_viewport_build(const HudViewBlock &block, float scale_y, int32_t surface_w,
		int32_t surface_h);

// The main scene's viewport for this display frame: the composed camera's block (`camera`,
// with the optics offsets) at the fov over the whole surface, the stretch the aspect mode
// takes, and the view block's eye (`view_block`) the labels measure from. Invalid over an
// empty surface.
HudViewport hud_main_viewport(const world::PlayerCameraPose &camera,
		const world::PlayerCameraPose &view_block, float fov_h_deg, int aspect_mode,
		int32_t surface_w, int32_t surface_h);

// HUD_ClipPointToFrustumAndProject over a VIEW-space point (x forward): the up axis over the
// stretch, then each side plane the point lies behind pulls it onto the plane and raises the
// plane's flag; a point short of the near distance raises 16 and keeps its view words, any
// other projects to whole surface pixels (x, y) through the focal; then the flagged edges force
// x to 0 / the surface width and y to 0 / the surface height. Returns the flags.
// [orig: HUD_ClipPointToFrustumAndProject @0x591E50 - the stretch @0x591E5B..0x591E76; the
//  plane tests, each `jns` past its clamp on a dot >= 0: @0x591EB7 (4, y to the top edge) /
//  @0x59204F (8, y to the bottom) / @0x5921E4 (1, x to the left) / @0x592379 (2, x to the
//  right); the near test @0x5924EC (0x10 @0x592575); the projection @0x5924F2..0x592570; the
//  edge forcing @0x592578..0x5925A8]
uint8_t hud_clip_point_to_frustum_and_project(const HudViewport &viewport, int32_t point[3]);

// A world point (16.16 mission) through the viewport: Math_FixedPointTransformPoint22 into view
// space, its depth read there, then the projector. An invalid viewport projects nothing.
// [orig: HUD_DrawEntityMarker @0x593140 - the transform @0x593168..0x59317C, the projector
//  @0x593194; HUD_DrawVehicleSeatAndArmoryLabels @0x5A3628 / @0x5A3655;
//  HUD_DrawEntityLabel @0x5A3B47 / @0x5A3B51]
HudProjectedPoint hud_project_world_point(const HudViewport &viewport,
		const std::array<int32_t, 3> &world_q16);

} // namespace opennova::hud
