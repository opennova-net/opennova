// The HUD declutter mask/level model — see hud_declutter.h for the witness
// map. [orig: HUD_ParseHudposToken @ 0x59F370; CRenderState_SetLayerVisibility
//  @ 0x59B0F0]

#include <runtime/hud/hud_declutter.h>

#include <base/io/strutil.h>
#include <formats/def/def.h>

using namespace opennova::def;

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
	// Retail stores the level word as handed over — the cfg cell @0x24D20BC
	// takes the parsed int and the callers pass it straight into
	// CRenderState_SetLayerVisibility. There is NO clamp: an out-of-range level
	// simply selects a bit no authored mask carries, and the whole gated HUD
	// hides until the next huddetail cycle wraps the level back to 0.
	// [orig: `mov layerIndex, eax` @0x4E060B/@0x4E0614; the cfg apply
	//  @0x55154d; the death force-3 @0x42e41c]
	level_ = level;
	rebuild();
}

int HudDeclutter::cycle_level() {
	// level + 1, wrapping past 3 to 0 — the compare is signed and runs on the
	// SUM, so a level the cfg parked above 3 wraps to 0 on the first press.
	// [orig: Input_HandleActionBinding_0 @ 0x4E0601..0x4E0624:
	//  `mov eax,layerIndex; add eax,ebx; cmp eax,3; mov layerIndex,eax;
	//   jle short; xor eax,eax; mov layerIndex,eax`]
	int next = level_ + 1;
	if (next > kDeclutterLevelMax) {
		next = 0;
	}
	set_level(next);
	return level_;
}

void HudDeclutter::apply_level(int level) {
	rebuild_at(level);
}

void HudDeclutter::rebuild() {
	rebuild_at(level_);
}

void HudItemFlash::set(int index, int32_t value) {
	// [orig: RenderState_SetLayerVisibilityByIndex @ 0x5A3020 — the store
	//  @ 0x5A302A]
	if (index < 0 || index >= kCount) return;
	timers_[static_cast<size_t>(index)] = value;
}

void HudItemFlash::tick(int32_t now) {
	if (now == last_tick_) return;
	// The x86 `sub` wraps; the clamp compare is signed (`jle`).
	const int32_t delta = static_cast<int32_t>(
			static_cast<uint32_t>(now) - static_cast<uint32_t>(last_tick_));
	last_tick_ = now;
	for (int32_t &t : timers_) {
		if (t == 0) continue;
		t = t > delta ? static_cast<int32_t>(
				static_cast<uint32_t>(t) - static_cast<uint32_t>(delta)) : 0;
	}
}

void HudDeclutter::rebuild_at(int level) {
	// [orig: CRenderState_SetLayerVisibility @ 0x59B0F0 — for slot 0..23,
	//  dword_2723C80[slot] = ((uint8_t)(1 << level) & byte_2723CE0[slot]) != 0]
	// The shift is x86's `shl edx, cl`, so the count is taken modulo 32, and
	// the `and cl, dl` that follows keeps only the LOW BYTE of the result:
	// levels 4..7 test bits no HUDDECLUT arm ever authors (everything hides),
	// levels 8..31 leave a zero byte (everything hides), and level 32 aliases
	// level 0 again. A negative level lands the same way through cl.
	// [orig: `shl edx, cl` @0x59B0FB; `mov cl, byte_2723CE0[eax]; and cl, dl`
	//  @0x59B100..0x59B106]
	const unsigned shift = static_cast<unsigned>(level) & 31u;
	const uint8_t bit = static_cast<uint8_t>(1u << shift);
	for (int slot = 0; slot < kDeclutterSlotCount; ++slot) {
		visible_[static_cast<size_t>(slot)] =
				(bit & masks_[static_cast<size_t>(slot)]) != 0;
	}
}

bool declutter_from_hudpos(const DefHudPosFile &file, HudDeclutter &out) {
	HudDeclutter authored;
	authored.begin_authoring();
	bool any_row = false;
	for (int slot = 0; slot < kDeclutterSlotCount; ++slot) {
		const char *token = declutter_token_name(slot);
		for (size_t i = 0; i < file.hud.declutter_count; ++i) {
			const DefDeclutterEntry &row = file.hud.declutter[i];
			if (!strutil::iequals(row.name, token)) continue;
			int flags[4];
			for (int f = 0; f < 4; ++f) flags[f] = row.flags[f] != 0 ? 1 : 0;
			authored.set_mask(slot, HudDeclutter::mask_from_flags(flags));
			any_row = true;
			break;
		}
	}
	if (any_row) out = authored;
	return any_row;
}

} // namespace opennova::hud
