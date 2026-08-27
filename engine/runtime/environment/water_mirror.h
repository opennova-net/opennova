// The water reflection mirror view (env #30) — the witnessed offscreen
// prerender's camera derivation as a typed record, ported from nova_water.gd
// (2026-08-10 de-scripting). Retail prerenders the mirrored scene into
// Water_ReflectionTexture BEFORE the main frame [orig: Render_TerrainScene
// @ 0x610c80 -> Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene
// @ 0x5c1240]; the prerender packs the live camera block {x, y, z, yaw,
// pitch, roll} and the mirrored view builds inside render_main_scene's
// view-matrix section. Hex-Rays elides the exact mirror transform, but the
// witnessed texm3x2 rows PIN its form: they sample the RTT at u = screen U
// (no horizontal flip) and v ~ 1 - screen V (env_water_render.h
// WaterStripRows), which only holds when the offscreen camera is the
// UP-PRESERVED proper mirror — reflect the basis about the plane, then
// negate the reflected up column. The raw reflection alone is IMPROPER
// (det -1: every triangle's winding flips, so faces cull backwards);
// negating the up column restores det +1 (the conjugated rotation = yaw
// kept, pitch/roll negated). Retail rebuilds projection for the square RTT
// while preserving the source's horizontal field
// [orig: Viewport_BuildProjectionMatrix @ 0x410fb0; bounds @ 0x5c1476].
// The witnessed collection filter follows the LIVE view side per frame:
// above water only the flag-0x400 population enters the mirror — vehicles by
// item type [orig: Entity_InitFromModel @ 0x40e208..0x40e20a] plus records
// whose BMS attribute authors Reflective [orig: Entity_SpawnFromBMSRecord
// @ 0x40ed1d..0x40ed2b] — while a below-water view collects unfiltered
// [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0
// — filterMask = camera_below_water ? 0 : 0x400, applied by every collector
// including the sector-building walk @ 0x5c6c32..0x5c6c39].
// NEAR-PLANE NOTE (TRACKED approximation, env #30 ledger): retail clips the
// mirrored scene against the water surface at waterHeight - 0.1
// [orig: plane block wh - 0.1, render_main_scene @ 0x5c1240]; the reimpl
// exposes no oblique clip plane and does not clip.
#pragma once

#include <formats/env/env.h>

#include <algorithm>
#include <cmath>

namespace opennova::env {

// Retail allocates a square 256 RTT at water detail 2; only detail >= 3 or
// the capture override selects 512 [orig: Water_CreateReflectionRenderTarget @ 0x5c08d1..0x5c0937].
// The reimpl has no higher-detail/capture selector, so its witnessed mapping
// is 256.
inline constexpr int kReflectionRttSize = 256;

// The witnessed reflected-scene dim (env #37's mechanism): after the mirrored
// sky/terrain/world render into the RTT, detail >= 2 multiplies the WHOLE
// target by vertex color 0x404040 — a fullscreen 4-vertex strip drawn with
// SetRenderState(D3DRS_SRCBLEND = D3DBLEND_DESTCOLOR, D3DRS_DESTBLEND =
// D3DBLEND_ZERO), i.e. out = dst * 64/255, then SRCALPHA/INVSRCALPHA restored
// [orig: render_main_scene @ 0x5c1727 detail gate; blend states
// @ 0x5c1856..0x5c186a; quad color 0xFF404040 + TRIANGLESTRIP draw
// @ 0x5c186c..0x5c189e; restore @ 0x5c18a3..0x5c18bf]. The celestial bodies
// and the sun glow render AFTER the dim [orig: @ 0x5c18fb/0x5c1904], so
// retail's mirrored sun/moon/glare stay bright; the reimpl's celestials live
// in the shared 3D world and dim with the scene — a TRACKED residual on the
// env #37 row.
inline constexpr float kReflectionDimFactor = 64.0f / 255.0f;

enum class MirrorProjection {
	kPerspective,
	kOrthogonal,
	kFrustum,
};

// Plain-value inputs describing the live source camera (the shell extracts
// them from its camera node; keep_aspect_height marks a vertically-authored
// fov/size axis).
struct MirrorSourceView {
	// Basis columns (x, y, z) and origin of the live camera's world
	// transform.
	Vec3 basis_x{};
	Vec3 basis_y{};
	Vec3 basis_z{};
	Vec3 origin{};
	MirrorProjection projection = MirrorProjection::kPerspective;
	float fov_deg = 75.0f;
	float ortho_size = 1.0f;
	float frustum_size = 1.0f;
	float frustum_offset_x = 0.0f;
	float frustum_offset_y = 0.0f;
	bool keep_aspect_height = true;
	float viewport_width = 0.0f;
	float viewport_height = 0.0f;
	float v_offset = 0.0f;
};

// The derived mirror camera + sampling state the applier installs.
struct WaterMirrorView {
	Vec3 basis_x{};
	Vec3 basis_y{};
	Vec3 basis_z{};
	Vec3 origin{};
	bool below_water = false;
	// The square pass receives the source's HORIZONTAL extent (fov for
	// perspective, size for ortho/frustum) with the frustum offset's Y
	// negated for the vertical mirror.
	float horizontal_fov_deg = 75.0f;
	float horizontal_size = 1.0f;
	float frustum_offset_x = 0.0f;
	float frustum_offset_y = 0.0f;
	// Preserving horizontal FOV makes the square mirror's X focal scale
	// match; its Y focal scale is source_height/source_width of the main
	// camera's — the strip rows' texm3x2 result converts through this scale
	// so a fixed reflected world point stays registered while the view
	// rotates.
	float uv_scale_x = 1.0f;
	float uv_scale_y = 1.0f;
	// The proper mirror negates the reflected UP column; the local vertical
	// offset negates too so the effective camera origin is the geometric
	// reflection of the source rather than shifted oppositely.
	float v_offset = 0.0f;
};

inline WaterMirrorView build_water_mirror_view(const MirrorSourceView &source,
		float water_height) {
	WaterMirrorView view;
	// position' = (x, 2*wh - y, z); each basis column reflects about the
	// plane normal n = (0, 1, 0) as c' = c - 2*n*dot(c, n) (flip the Y
	// component), then the reflected up column negates — see the header
	// witness note.
	view.basis_x = Vec3{source.basis_x.x, -source.basis_x.y, source.basis_x.z};
	view.basis_y = Vec3{-source.basis_y.x, source.basis_y.y, -source.basis_y.z};
	view.basis_z = Vec3{source.basis_z.x, -source.basis_z.y, source.basis_z.z};
	view.origin = Vec3{source.origin.x, 2.0f * water_height - source.origin.y,
			source.origin.z};
	view.below_water = source.origin.y < water_height;

	const float aspect = source.viewport_height > 0.0f
			? source.viewport_width / source.viewport_height
			: 1.0f;
	view.uv_scale_x = 1.0f;
	view.uv_scale_y = aspect != 0.0f ? 1.0f / aspect : 1.0f;
	view.v_offset = -source.v_offset;

	switch (source.projection) {
		case MirrorProjection::kOrthogonal: {
			float horizontal_size = source.ortho_size;
			if (source.keep_aspect_height) {
				horizontal_size *= aspect;
			}
			view.horizontal_size = horizontal_size;
		} break;
		case MirrorProjection::kFrustum: {
			// The shell's frustum size is always the vertical span (its
			// projection builder does not reinterpret the axis), so the
			// square pass always receives vertical * aspect, with the
			// offset's Y negated for the vertical mirror.
			view.horizontal_size = source.frustum_size * aspect;
			view.frustum_offset_x = source.frustum_offset_x;
			view.frustum_offset_y = -source.frustum_offset_y;
		} break;
		case MirrorProjection::kPerspective:
		default: {
			float horizontal_fov = source.fov_deg;
			if (source.keep_aspect_height) {
				constexpr float kDegToRad = 0.01745329251994329577f;
				constexpr float kRadToDeg = 57.29577951308232088f;
				horizontal_fov = kRadToDeg * 2.0f *
						std::atan(std::tan(source.fov_deg * kDegToRad * 0.5f) *
								aspect);
			}
			// The camera device accepts [1, 179] degrees; very thin but still
			// drawable viewports asymptotically approach 180.
			view.horizontal_fov_deg = std::clamp(horizontal_fov, 1.0f, 179.0f);
		} break;
	}
	return view;
}

} // namespace opennova::env
