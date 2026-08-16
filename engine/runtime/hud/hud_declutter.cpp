// The HUD declutter mask/level model — see hud_declutter.h for the witness
// map. [orig: HUD_ParseHudposToken @ 0x59F370; CRenderState_SetLayerVisibility
//  @ 0x59B0F0]

#include "hud/hud_declutter.h"

#include "io/strutil.h"

namespace opennova::hud {

namespace {

// Slot-ordered token suffixes, one per HUD_ParseHudposToken arm
// [orig: the "HUDDECLUT_..." string table feeding the arms @ 0x59F370].
constexpr const char *kTokenNames[kDeclutterSlotCount] = {
	"MSNTITLE",    // 0
	"FARPINFO",    // 1
	"BREATHTIME",  // 2
	"WAYPOINT",    // 3
	"ALTGRP",      // 4
	"SPEED",       // 5
	"CKPTCOND",    // 6
	"DMGBAR",      // 7
	"WPNGRP",      // 8
	"EXPPOINTS",   // 9
	"NWSTAT",      // 10
	"CFGDISP",     // 11
	"LOLLIS",      // 12
	"XHAIRS",      // 13
	"VELVECT",     // 14
	"FARPIND",     // 15
	"TARGDMGDISP", // 16
	"SPINMAP",     // 17
	"TRGTCNT",     // 18
	"TEAMID",      // 19
	"PWRBAR",      // 20
	"CLOCK",       // 21
	"HUDLS",       // 22
	"CHAT",        // 23
};

} // namespace

const char *declutter_token_name(int slot) {
	if (slot < 0 || slot >= kDeclutterSlotCount) {
		return nullptr;
	}
	return kTokenNames[slot];
}

int declutter_slot_from_token(const char *token) {
	if (token == nullptr) {
		return -1;
	}
	for (int slot = 0; slot < kDeclutterSlotCount; ++slot) {
		if (strutil::iequals(token, kTokenNames[slot])) {
			return slot;
		}
	}
	// No parse arm — the token authors nothing (the dead JOX HUDDECLUT_CTAPE
	// row lands here).
	return -1;
}

std::array<bool, kDeclutterSlotCount> declutter_all_visible() {
	std::array<bool, kDeclutterSlotCount> all{};
	all.fill(true);
	return all;
}

HudDeclutter::HudDeclutter() {
	// Embedder/test-harness default: all-bits masks keep every slot visible
	// at every level until a real hudpos declutter table is authored.
	masks_.fill(0x0F);
	rebuild();
}

void HudDeclutter::begin_authoring() {
	// [orig: byte_2723CE0 BSS-zero — an unauthored slot stays mask 0 = hidden
	// at every level]
	masks_.fill(0);
	rebuild();
}

uint8_t HudDeclutter::mask_from_flags(const int flags[4]) {
	// [orig: each parse arm ors bit i for a nonzero value i @ 0x59F370]
	uint8_t mask = 0;
	for (int i = 0; i < 4; ++i) {
		if (flags[i] != 0) {
			mask = static_cast<uint8_t>(mask | (1u << i));
		}
	}
	return mask;
}

void HudDeclutter::set_mask(int slot, uint8_t mask) {
	if (slot < 0 || slot >= kDeclutterSlotCount) {
		return;
	}
	masks_[static_cast<size_t>(slot)] = static_cast<uint8_t>(mask & 0x0F);
	rebuild();
}

uint8_t HudDeclutter::mask(int slot) const {
	if (slot < 0 || slot >= kDeclutterSlotCount) {
		return 0;
	}
	return masks_[static_cast<size_t>(slot)];
}

void HudDeclutter::set_level(int level) {
	level_ = level < 0 ? 0 : (level > kDeclutterLevelMax ? kDeclutterLevelMax
														 : level);
	rebuild();
}

int HudDeclutter::cycle_level() {
	// level + 1, wrapping past 3 to 0
	// [orig: Input_HandleActionBinding_0 @ 0x4E0601..0x4E0624].
	int next = level_ + 1;
	if (next > kDeclutterLevelMax) {
		next = 0;
	}
	set_level(next);
	return level_;
}

void HudDeclutter::rebuild() {
	// [orig: CRenderState_SetLayerVisibility @ 0x59B0F0 — for slot 0..23,
	// dword_2723C80[slot] = ((1 << level) & byte_2723CE0[slot]) != 0]
	for (int slot = 0; slot < kDeclutterSlotCount; ++slot) {
		visible_[static_cast<size_t>(slot)] =
				((1u << level_) & masks_[static_cast<size_t>(slot)]) != 0;
	}
}

} // namespace opennova::hud
