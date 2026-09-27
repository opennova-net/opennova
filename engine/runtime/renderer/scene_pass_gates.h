#pragma once

// The blink-letter and waterline gates of the passes one frame draws: the
// main frame's terrain passes, detail-foliage passes and sky bracket, and the
// water mirror's own sky bracket. The sky bracket is the dome AND the sun/moon discs
// (SkyDome_RenderWithSkyfog -> Render_Skybox -> Render_CelestialBodies); the sun glow,
// the water glint and the sun veil are drawn outside it and are never gated
// here.

#include <runtime/world/collision.h>

#include <cstdint>

namespace opennova::renderer {

struct ScenePassGates {
	// The main frame's PolyTrn passes: the traversal (and with it the
	// near-foliage patch collection) and the sector pass, both skipped while
	// the indoors letter is set [orig: Render_ProcessMainSceneFrame
	// @ 0x5ca197..0x5ca19f -> the traversal skip @ 0x5CA645..0x5CA64E over
	// sub_60FF50 @ 0x5CA654, the sector-pass skip @ 0x5ca84f over
	// Terrain_RenderMainSectorPass @ 0x5ca867]; the water mirror [orig:
	// Render_MainScene @ 0x5c1353], the weapon Inset on the main frame's
	// value [orig: Render_WeaponInsetScene @ 0x5C9A23..0x5C9A2B over
	// sub_60FF50 @ 0x5C9A2F, @ 0x5C9D57..0x5C9D5F; the push @ 0x5CA948] and
	// the NVG scene [orig: NVG_RenderSceneToTarget @ 0x5D055C (the letter
	// test), the skip @ 0x5d0570] skip their own PolyTrn pass on the same
	// letter.
	bool terrain = true;
	// The scene core's two detail-foliage passes, skipped on the same letter
	// [orig: Terrain_RenderWorldScene @ 0x5C93D5..0x5C93E0 ->
	// Foliage_RenderDetailPatchesPass skips @ 0x5C95BD..0x5C95BF (formatType
	// 0) / @ 0x5C965D..0x5C965F (formatType 1)]. The entity collector and the
	// BySide waves, the MODEL masks drawn inside them included, take no
	// letter and still run indoors [orig: Terrain_RenderWorldScene @ 0x5C94F0,
	// @ 0x5C951F..0x5C9638], so this gates the detail tier alone.
	bool detail_foliage = true;
	// The main frame's sky bracket: drawn only while the sky letter is clear
	// AND the eye is strictly above the water plane (the `jg` keeps the
	// bracket only above; exact equality skips it) [orig:
	// Render_ProcessMainSceneFrame @ 0x5ca1a3..0x5ca1bd -> @ 0x5ca7c4..0x5ca81a].
	bool sky = true;
	// The water mirror's outdoors flag, clear under the indoors letter
	// [orig: Render_MainScene @ 0x5c1342..0x5c1353]. The one flag gates three
	// things of the mirror: its sky bracket [orig: @ 0x5c166b..0x5c1676], its
	// PolyTrn pass [orig: @ 0x5c14a7..0x5c14ab] and its clear colour, skyfog
	// outdoors and black under the letter [orig: @ 0x5c1474, @ 0x5c1597]
	// (EnvironmentState::water_mirror_clear_color).
	bool mirror_sky = true;
};

inline ScenePassGates scene_pass_gates(uint32_t blink_flags,
		bool eye_at_or_below_water) {
	ScenePassGates gates;
	gates.terrain = (blink_flags & world::kBlinkIndoorsBit) == 0;
	gates.detail_foliage = (blink_flags & world::kBlinkIndoorsBit) == 0;
	gates.sky = (blink_flags & world::kBlinkSkyOffBit) == 0 &&
			!eye_at_or_below_water;
	gates.mirror_sky = (blink_flags & world::kBlinkIndoorsBit) == 0;
	return gates;
}

// The passes a letter change reopens. The letters gate the frame they are
// read in: retail reads them at the top of the main frame, ahead of its
// terrain traversal and sector pass [orig: Render_ProcessMainSceneFrame
// @ 0x5ca192 ahead of @ 0x5ca645..0x5ca654 and @ 0x5ca84f over @ 0x5ca867],
// and the scene core latches its water label from them ahead of its two water
// passes [orig: Terrain_RenderWorldScene @ 0x5c93b8..0x5c93cd ahead of
// @ 0x5c95d4..0x5c95ea]. A letter that clears on a tick therefore draws the
// terrain (the indoors letter) or the water (the water letter) in that tick's
// own frame, so a device that already ran those passes closed this frame
// re-runs them on the edge. A letter that sets needs no edge: the pass it
// closes is simply skipped from that frame on.
struct ScenePassGateEdges {
	bool terrain_opened = false;
	bool water_opened = false;
};

inline ScenePassGateEdges scene_pass_gate_edges(uint32_t prev_letters,
		uint32_t next_letters) {
	const uint32_t cleared = prev_letters & ~next_letters;
	ScenePassGateEdges edges;
	edges.terrain_opened = (cleared & world::kBlinkIndoorsBit) != 0;
	edges.water_opened = (cleared & world::kBlinkWaterOffBit) != 0;
	return edges;
}

} // namespace opennova::renderer
