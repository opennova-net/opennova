#pragma once

namespace opennova::def {
struct DefHudPosFile;  // formats/def/def.h
}

// The HUD declutter system: hudpos.def HUDDECLUT_* masks x the persisted
// hud_detail level -> the 24-slot per-element visibility table the overlay
// walk consults before each gated draw.
// [orig: masks byte_2723CE0 built by HUD_ParseHudposToken @ 0x59F370; level
//  @ 0x24D20BC (cfg int "hud_detail", parse @ 0x550339, default 0 @ 0x54d3d8,
//  apply @ 0x55154d, saved @ 0x54c80d); rebuild
//  CRenderState_SetLayerVisibility @ 0x59B0F0 -> dword_2723C80]
// Unported residual: the WAC/mission event action (type 37) force-applies
// level 0 through a side path WITHOUT touching the global [orig:
//  RenderState_SetLayerVisibilityByIndex @ 0x5A3020, flash-timer array
//  dword_2723CF8] — tracked, not modeled here.
// Witness record: docs/interface/hud-re.md.

#include <array>
#include <cstdint>

namespace opennova::hud {

// The 24 HUDDECLUT slots in retail's parse-arm order. Slots 0/1/5/9/10/11/12/
// 14/15/16 are parsed but have NO retail draw-site reader — nothing may gate
// on them. [orig: the HUD_ParseHudposToken arm table @ 0x59F370; the read
// sites are the per-slot dword_2723C80 cmps cited at each element]
enum HudDeclutterSlot : int {
	kDeclutterMsnTitle = 0,
	kDeclutterFarpInfo = 1,
	kDeclutterBreathTime = 2,
	kDeclutterWaypoint = 3,
	kDeclutterAltGrp = 4,
	kDeclutterSpeed = 5,
	kDeclutterCkptCond = 6,
	kDeclutterDmgBar = 7,
	kDeclutterWpnGrp = 8,
	kDeclutterExpPoints = 9,
	kDeclutterNwStat = 10,
	kDeclutterCfgDisp = 11,
	kDeclutterLollis = 12,
	kDeclutterXhairs = 13,
	kDeclutterVelVect = 14,
	kDeclutterFarpInd = 15,
	kDeclutterTargDmgDisp = 16,
	kDeclutterSpinmap = 17,
	kDeclutterTrgtCnt = 18,
	kDeclutterTeamId = 19,
	kDeclutterPwrBar = 20,
	kDeclutterClock = 21,
	kDeclutterHudLs = 22,
	kDeclutterChat = 23,
	kDeclutterSlotCount = 24,
};

// The AUTHORED declutter level range: 0..3, one visibility bit per level in
// each mask, and the wrap point of the huddetail cycle. It is NOT a clamp on
// the stored level: retail's rebuild shifts an 8-bit bit selector, so a level
// outside 0..3 simply matches no authored mask bit and hides every gated
// element (see HudDeclutter::set_level / rebuild).
inline constexpr int kDeclutterLevelMax = 3;

// The authored token suffix for a slot ("MSNTITLE".."CHAT"; nullptr out of
// range) and the reverse lookup (-1 for an unknown token — retail simply has
// no parse arm for it, e.g. the dead JOX HUDDECLUT_CTAPE row).
const char *declutter_token_name(int slot);
int declutter_slot_from_token(const char *token);

// The all-visible per-slot table HudFrameState defaults to (see the
// declutter_visible note in hud_frame.h).
std::array<bool, kDeclutterSlotCount> declutter_all_visible();

class HudDeclutter;

// The HUDDECLUT mask table from a parsed hudpos file [orig:
// HUD_ParseHudposToken @0x59F370 -> byte_2723CE0]: a file that authors ANY
// known row is applied faithfully from the zeroed table (an unauthored slot
// then stays hidden at every level, like retail's). Returns false and leaves
// `out` untouched when the file authors no known row at all (the test
// harness's minimal layouts; retail never ships one), so the embedder keeps
// the all-visible default instead of blanking the whole HUD.
bool declutter_from_hudpos(const opennova::def::DefHudPosFile &file, HudDeclutter &out);

// The mask table + level + rebuild rule. Construction leaves every mask
// all-bits (0xF = visible at every level) — the embedder/test-harness default
// for a HUD with no hudpos declutter rows; a real hudpos feed starts from
// begin_authoring(), where retail's zeroed table makes an UNAUTHORED slot
// hidden at every level. [orig: byte_2723CE0 is BSS-zero; only an authored
// HUDDECLUT_* arm stores a nonzero mask @ 0x59F370]
class HudDeclutter {
public:
	HudDeclutter();

	// Zero every mask — the parse-start state of retail's table (unauthored
	// slot = 0 = hidden at every level).
	void begin_authoring();

	// One authored row's mask byte: bit i (1/2/4/8 for detail level 0..3) set
	// iff value i is nonzero. [orig: each HUD_ParseHudposToken arm @ 0x59F370
	//  ors 1/2/4/8 per nonzero atof result into byte_2723CE0[slot]]
	static uint8_t mask_from_flags(const int flags[4]);

	void set_mask(int slot, uint8_t mask);
	uint8_t mask(int slot) const;

	// The persisted hud_detail level, stored VERBATIM (retail has no clamp),
	// and the huddetail action cycle: level + 1, wrapping past 3 to 0;
	// returns the new level.
	// [orig: level @ 0x24D20BC; Input_HandleActionBinding_0
	//  @ 0x4E0601..0x4E0624]
	void set_level(int level);
	int level() const { return level_; }
	int cycle_level();

	// visible[slot] = ((uint8_t)(1 << (level & 31)) & mask[slot]) != 0, rebuilt
	// on every mask or level change — retail's 32-bit `shl` narrowed to the
	// mask byte by the following `and cl, dl`.
	// [orig: CRenderState_SetLayerVisibility @ 0x59B0F0 -> dword_2723C80]
	const std::array<bool, kDeclutterSlotCount> &visible() const {
		return visible_;
	}

private:
	void rebuild();

	std::array<uint8_t, kDeclutterSlotCount> masks_;
	std::array<bool, kDeclutterSlotCount> visible_;
	int level_ = 0;
};

} // namespace opennova::hud
