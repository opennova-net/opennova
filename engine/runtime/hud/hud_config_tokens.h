#pragma once

// The HUD's persisted config tokens and session flags: the defaults, ranges
// and cycle rules the presenting shell round-trips through its settings
// store (the store itself — a cfg file — is device work), plus the friendly
// tag anchor lift. HudFrameState (hud_frame.h) carries the live values; this
// header is where their policy lives so no presenter restates a clamp.

#include <cstdint>

namespace opennova::hud {

// The HUD color-scheme index (0 white / 1 green / 2 hudpos hud_textcolor /
// 3 light blue / 4 yellow / 5 salmon), persisted like retail's config token
// (read at boot, written back on cycle). [orig: config token
// "hud_color_index" @0x5502eb, default 2 @0x54d2a6; applied to the live index
// @0x55152f; cycled 0..5 by the `hudcolor` action (catalog row 76 -> dispatch
// code 10) @0x49afc7]
inline constexpr int kHudColorIndexDefault = 2;
inline constexpr int kHudColorIndexCount = 6;
inline int clamp_hud_color_index(int index) {
	if (index < 0) return 0;
	if (index >= kHudColorIndexCount) return kHudColorIndexCount - 1;
	return index;
}
inline int next_hud_color_index(int index) {
	return (index + 1) % kHudColorIndexCount;
}

// The HUD declutter level, persisted like retail's config token round trip.
// Default 0 = everything the masks author at level 0; level 3 = the
// whole-pass early-out (the blank HUD). [orig: cfg int "hud_detail" — parse
// @0x550339, default 0 @0x54d3d8, applied to the live level @0x55154d, saved
// @0x54c80d; the level drives CRenderState_SetLayerVisibility @0x59B0F0; the
// level-3 early-out @0x5A80C4]
inline constexpr int kHudDetailLevelDefault = 0;
inline constexpr int kHudDetailLevelMax = 3;
inline constexpr int kHudDetailLevelBlank = 3;
inline int clamp_hud_detail_level(int level) {
	if (level < 0) return 0;
	if (level > kHudDetailLevelMax) return kHudDetailLevelMax;
	return level;
}
// The `huddetail` cycle (dispatch code 19): 0 -> 1 -> 2 -> 3 -> 0.
inline int next_hud_detail_level(int level) {
	return level >= kHudDetailLevelMax ? 0 : level + 1;
}

// The showhud 2-bit FP-view flags, session state like retail's
// process-lifetime global. Bit 0 = the FP gun/viewmodel draw, bit 1 = the
// corner spinmap block; default 3 = both (the cfg gun-visible option writes
// 3/2). [orig: g_FpWeaponViewFlags — cycle (flags + 1) & 3 @0x4E0561; bit0
// read Player_RenderFirstPersonViewModel @0x4DEDEA; bit1 read @0x5A8635; the
// option writes @0x5521CB/@0x5521D7]
inline constexpr uint32_t kShowHudFlagsDefault = 3;
inline constexpr uint32_t kShowHudFlagGun = 1;
inline constexpr uint32_t kShowHudFlagSpinmap = 2;
inline uint32_t next_showhud_flags(uint32_t flags) {
	return (flags + 1) & 3;
}

// The overhead-anchor addend above the entity's eye offset: 0x4000 = 0.25 u
// [orig: anchor z = z + entity[+116] + 0x4000 @0x5a3a84..0x5a3a98 — +116 is
// the stance-driven eye offset the sim restamps per body tick and feeds per
// tag; docs/interface/hud-re.md D-HUD-20].
inline constexpr float kFriendlyTagLiftUnits = 0.25f;

} // namespace opennova::hud
