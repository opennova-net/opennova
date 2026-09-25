// The water reflection mirror view (env #30) — the witnessed offscreen
// prerender's camera derivation as a typed record.
// Retail prerenders the reflected scene into
// Water_ReflectionTexture BEFORE the main frame [orig: Render_TerrainScene
// @ 0x610c80 -> Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene
// @ 0x5c1240]; the prerender packs the live camera block {x, y, z, yaw,
// pitch, roll} and render_main_scene mirrors it ONLY while the camera is at
// or above the plane: z' = 2wh - z, yaw kept, pitch and roll negated
// [orig: render_main_scene @ 0x5c1361..0x5c1370 (cmp cam.z, wh; jl),
// mirror @ 0x5c1376..0x5c139c] — the UP-PRESERVED proper mirror (reflect
// the basis about the plane, then negate the reflected up column; det +1,
// so no winding flips). The texm3x2 rows sample the RTT at u = screen U and
// v ~ 1 - screen V (env_water_render.h WaterStripRows), which that mirror
// satisfies. Below the plane the block is copied UNCHANGED [orig:
// @ 0x5c13f6..0x5c1414]: the RTT is the scene from the live eye (clipped to
// the part above the water, see the clip note below) and the underwater
// rows sample it upside down. The reflected pass projects with the MAIN
// view's field on both axes (see kReflectionRttSize).
// The witnessed collection filter follows the LIVE view side per frame:
// above water only the flag-0x400 population enters the mirror — vehicles by
// item type [orig: Entity_InitFromModel @ 0x40e208..0x40e20a] plus records
// whose BMS attribute authors Reflective [orig: Entity_SpawnFromBMSRecord
// @ 0x40ed1d..0x40ed2b] — while a below-water view collects unfiltered
// [orig: Terrain_CollectVisibleEntitiesForReflection @ 0x5c90a0
// — filterMask = camera_below_water ? 0 : 0x400, applied by every collector
// including the sector-building walk @ 0x5c6c32..0x5c6c39].
// CLIP: retail cuts the reflected scene at the water per pixel, on both
// camera sides, through the 4x4 GSysClip alpha texture (point, clamp; alpha 0
// in columns 0-1 and 255 in columns 2-3 [orig: Render_CreateSystemTextures
// @ 0x58acc0..0x58acf6]) under AlphaRef 0x80, fed a texgen u = world y +
// offset: objects keep y >= wh (g_WaterMirrorMatrix row 3 = 0.5 - wh,
// g_WaterMirrorActive = 1 [orig: Water_RenderReflectedWorldScene
// @ 0x5c8540..0x5c856a]) and the terrain keeps y >= wh - 0.05 (u = y + 0.45 -
// (wh - 0.1) [orig: render_main_scene @ 0x5c1561..0x5c1578 plane wh - 0.1,
// armed while wh != 0; render_terrain_sector_batch @ 0x6092c6..0x60935b]).
// The shaders port it as a discard in the mirror pass (the one camera whose
// mask omits the water layer, render/visual_layers.h). The reflected pass
// fogs with the dry weather block whatever side the eye is on
// (EnvironmentState::build_water_mirror_fog) and never swaps the terrain's
// stage 3 to the water noise (the mirror context's below-water word is 0
// [orig: render_main_scene @ 0x5c153d]).
#pragma once

#include <formats/env/env.h>

#include <cstdint>

#include <algorithm>
#include <cmath>

namespace opennova::env {

// Retail sizes the square reflection RTT from the water detail level:
// `if (dword_B4C3C0 || (size = 256, Water_DetailLevel >= 3)) size = 512;`
// i.e. 256 at detail 2, 512 at detail >= 3 or under the capture override,
// then `GTexRT_Construct(obj, size, size, 1, 1)` allocates it
// [orig: Water_CreateReflectionRenderTarget @ 0x5c08b0, the allocation body
// @ 0x5c08d1..0x5c0937, the size selector @ 0x5c08eb..0x5c08ed;
// Water_DetailLevel @ 0x24d2050]. The shipped
// max-quality path (our locked target) runs detail 3: Game_StartMission copies
// the adapter caps (`sub_5899E0(0)`/`sub_676850`) and with caps 0xFDF the
// detail-1 downgrade never fires [orig: Game_StartMission @ 0x524662..0x524668;
// downgrade @ 0x5c19da], so the live retail witness is a populated 512x512
// target. The reimpl carries no detail selector; it fixes the max-quality size.
// That square target renders with the MAIN view's projection: render_main_scene
// hands the main target's h/w as the projection's vertical scale
// [orig: render_main_scene @ 0x5c1255 (Render_GetTargetAspectRatio returns flt_8409EC), its
// Render_SetViewAndProjectionMatrices call @ 0x5c163e], so the 512 x 512
// texels cover exactly the main view's field (non-square texels, 512 rows
// across the vertical field) and the strip rows sample it at (screen U,
// 1 - screen V). A
// Godot camera renders square pixels only, so the port keeps the source
// projection and sizes the target round(512 x aspect) x 512
// (reflection_rtt_size): the same field and the same 512 rows across it, with
// round(512 x aspect) columns where retail has 512.
inline constexpr int kReflectionRttSize = 512;
// A degenerate layout (a collapsed or very thin view) would ask for more
// columns than a device texture holds; past this width the target keeps the
// source aspect with fewer rows.
inline constexpr int kReflectionRttMaxWidth = 16384;

struct ReflectionRttSize {
	int width = kReflectionRttSize;
	int height = kReflectionRttSize;
};

inline ReflectionRttSize reflection_rtt_size(float source_width, float source_height) {
	ReflectionRttSize size;
	if (!(source_width > 0.0f) || !(source_height > 0.0f)) {
		return size;
	}
	const double aspect = static_cast<double>(source_width) / source_height;
	const double columns = std::round(kReflectionRttSize * aspect);
	if (columns > kReflectionRttMaxWidth) {
		size.width = kReflectionRttMaxWidth;
		size.height = std::max(1, static_cast<int>(
				std::round(kReflectionRttMaxWidth / aspect)));
	} else {
		size.width = std::max(1, static_cast<int>(columns));
	}
	return size;
}

// The witnessed reflected-scene dim (env #37's mechanism): after the mirrored
// sky/terrain/world render into the RTT, detail >= 2 multiplies the WHOLE
// target by vertex color 0x404040 — a fullscreen 4-vertex strip drawn with
// SetRenderState(D3DRS_SRCBLEND = D3DBLEND_DESTCOLOR, D3DRS_DESTBLEND =
// D3DBLEND_ZERO), i.e. out = dst * 64/255, then SRCALPHA/INVSRCALPHA restored
// [orig: render_main_scene @ 0x5c1727 detail gate; blend states
// @ 0x5c1856..0x5c186a; quad color 0xFF404040 + TRIANGLESTRIP draw
// @ 0x5c186c..0x5c189e; restore @ 0x5c18a3..0x5c18bf]. The sun/moon discs
// and the sun glow are redrawn AFTER the dim, inside the far depth band
// [orig: render_main_scene @ 0x5c18fb / @ 0x5c1904], so retail's mirrored
// bodies over the sky stay bright. Both close the mirror's overlay pass
// (runtime/renderer/scene_overlay.h kMirrorOverlayOrder).
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
	// The reflected-scene camera: the mirror above/at the plane, the live
	// camera unchanged below it.
	Vec3 basis_x{};
	Vec3 basis_y{};
	Vec3 basis_z{};
	Vec3 origin{};
	bool below_water = false;
	// The source's own projection (the main view's field on both axes, the
	// same axis convention), with the frustum offset's Y negated for the
	// vertical mirror above the plane.
	MirrorProjection projection = MirrorProjection::kPerspective;
	float fov_deg = 75.0f;
	float size = 1.0f;
	bool keep_aspect_height = true;
	float frustum_offset_x = 0.0f;
	float frustum_offset_y = 0.0f;
	// The mirror target (reflection_rtt_size of the source viewport): its
	// aspect is the source's, so the strip rows' (screen U, 1 - screen V)
	// lookup lands on the reflected point without a rescale.
	ReflectionRttSize rtt{};
	// The proper mirror negates the reflected UP column; the local vertical
	// offset negates too so the effective camera origin is the geometric
	// reflection of the source rather than shifted oppositely. The unmirrored
	// below-water view keeps the source's.
	float v_offset = 0.0f;
};

// The reflected pass arms an object's CLIP technique per DRAW while it renders
// the mirror (g_WaterMirrorActive): the building pass arms a sector model whose
// bottom, z + the graphic's bound-block floor (graphicModel +0xB0 -> +0x28, the
// CMDL header bbox z-lo), lies below wh - 0.25 [orig: Terrain_RenderSectorModels
// @ 0x5c5e57..0x5c5e75], and the first entity wave an entity whose
// z - boundRadius (entity+0) lies below wh [orig: Terrain_RenderSectorEntities
// @ 0x5c7c1a..0x5c7c2e]. The BySide waves (the person entities) never arm it,
// nor does any draw outside those walks (the sky bracket's celestial bodies):
// an unarmed draw keeps its NORMAL technique in the mirror. All 16.16, signed.
enum class MirrorClipWave : uint8_t {
	kNone = 0,
	kSectorModel = 1,
	kEntity = 2,
};

inline bool water_mirror_clip_armed(MirrorClipWave wave, int32_t z_q16,
		int32_t extent_q16, int32_t water_height_q16) {
	switch (wave) {
		case MirrorClipWave::kSectorModel:
			return static_cast<int64_t>(z_q16) + extent_q16 <
					static_cast<int64_t>(water_height_q16) - 0x4000;
		case MirrorClipWave::kEntity:
			return static_cast<int64_t>(z_q16) - extent_q16 < water_height_q16;
		case MirrorClipWave::kNone:
		default:
			return false;
	}
}

// The same test as an offset from the draw's origin height in world units
// (armed <=> origin + offset < wh): the form a static MultiMesh row carries,
// since its instance origin is only known on the device. kNone never arms.
inline float water_mirror_clip_origin_offset(MirrorClipWave wave, int32_t extent_q16) {
	switch (wave) {
		case MirrorClipWave::kSectorModel:
			return static_cast<float>(static_cast<int64_t>(extent_q16) + 0x4000) /
					65536.0f;
		case MirrorClipWave::kEntity:
			return -static_cast<float>(extent_q16) / 65536.0f;
		case MirrorClipWave::kNone:
		default:
			return 3.0e38f;
	}
}

inline WaterMirrorView build_water_mirror_view(const MirrorSourceView &source,
		float water_height) {
	WaterMirrorView view;
	view.below_water = source.origin.y < water_height;
	if (view.below_water) {
		// Below the plane the camera block is copied unchanged
		// [orig: render_main_scene @ 0x5c1370 jl -> @ 0x5c13f6..0x5c1414].
		view.basis_x = source.basis_x;
		view.basis_y = source.basis_y;
		view.basis_z = source.basis_z;
		view.origin = source.origin;
		view.v_offset = source.v_offset;
	} else {
		// position' = (x, 2*wh - y, z) [orig: @ 0x5c1379..0x5c137b]; each
		// basis column reflects about the plane normal n = (0, 1, 0) as
		// c' = c - 2*n*dot(c, n) (flip the Y component), then the reflected up
		// column negates (pitch and roll negated @ 0x5c138e/@ 0x5c1390, yaw
		// kept) — see the header witness note.
		view.basis_x = Vec3{source.basis_x.x, -source.basis_x.y, source.basis_x.z};
		view.basis_y = Vec3{-source.basis_y.x, source.basis_y.y, -source.basis_y.z};
		view.basis_z = Vec3{source.basis_z.x, -source.basis_z.y, source.basis_z.z};
		view.origin = Vec3{source.origin.x, 2.0f * water_height - source.origin.y,
				source.origin.z};
		view.v_offset = -source.v_offset;
	}

	view.projection = source.projection;
	view.keep_aspect_height = source.keep_aspect_height;
	view.fov_deg = source.fov_deg;
	view.size = source.projection == MirrorProjection::kFrustum ? source.frustum_size
																 : source.ortho_size;
	view.frustum_offset_x = source.frustum_offset_x;
	view.frustum_offset_y = view.below_water ? source.frustum_offset_y
											 : -source.frustum_offset_y;
	view.rtt = reflection_rtt_size(source.viewport_width, source.viewport_height);
	return view;
}

} // namespace opennova::env
