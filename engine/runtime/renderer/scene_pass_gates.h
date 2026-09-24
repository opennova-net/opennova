#pragma once

// The blink-letter and waterline gates of the passes one frame draws: the
// main frame's terrain sector pass and sky bracket, and the water mirror's
// own sky bracket. The sky bracket is the dome AND the sun/moon discs
// (sub_579CB0 -> render_skybox -> render_celestial_bodies); the sun glow,
// the water glint and the sun veil are drawn outside it and are never gated
// here.

#include <runtime/world/collision.h>

#include <cstdint>

namespace opennova::renderer {

struct ScenePassGates {
	// The main frame's PolyTrn sector pass: skipped while the indoors letter
	// is set [orig: Render_ProcessMainSceneFrame @ 0x5ca197..0x5ca19f -> the
	// skip @ 0x5ca84f over Terrain_RenderSkyboxPass @ 0x5ca867, the sector
	// batch]; the water mirror [orig: render_main_scene @ 0x5c1353] and the
	// weapon inset's scene [orig: terrain_scene_render @ 0x5d0570] skip their
	// own PolyTrn pass on the same letter.
	bool terrain = true;
	// The main frame's sky bracket: drawn only while the sky letter is clear
	// AND the eye is strictly above the water plane (the `jg` keeps the
	// bracket only above; exact equality skips it) [orig:
	// Render_ProcessMainSceneFrame @ 0x5ca1a3..0x5ca1bd -> @ 0x5ca7c4..0x5ca81a].
	bool sky = true;
	// The water mirror's sky bracket: its own indoors-letter gate, the same
	// letter that skips the mirror's PolyTrn pass [orig: render_main_scene
	// @ 0x5c1342..0x5c1353 -> @ 0x5c166b..0x5c1676].
	bool mirror_sky = true;
};

inline ScenePassGates scene_pass_gates(uint32_t blink_flags,
		bool eye_at_or_below_water) {
	ScenePassGates gates;
	gates.terrain = (blink_flags & world::kBlinkIndoorsBit) == 0;
	gates.sky = (blink_flags & world::kBlinkSkyOffBit) == 0 &&
			!eye_at_or_below_water;
	gates.mirror_sky = (blink_flags & world::kBlinkIndoorsBit) == 0;
	return gates;
}

} // namespace opennova::renderer
