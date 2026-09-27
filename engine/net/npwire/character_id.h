#pragma once

// ---- ClientAuth character_id bit-pack (the CI0/CI1 join vars) ----------
//
// Retail's game ClientAuth does not invent a network-only player id: it
// uploads the two profile character selections (one per alignment) packed
// from Avatars.def as [alignment:1 | combo:6 | division:4 | nationality:5]
// (bit 15 down to bit 0). The same word is the player's entity+0x15C wire
// NetId, the minimap/character-slot registry key, and the weapon.sav side
// header's +4 u16 — one packing everywhere. The nationality/division fields
// are the authored `nationality N` / `division D` numbers (they index retail's
// fixed 32/16-entry arrays), the combo field the authored `combo <id>`; the
// registry lookup matches all three plus the alignment bit against the combo
// entry (+0/+4/+8/+276) [orig: packer EntitySlot_LookupAndPackEntry
// @0x57AD40 (@0x57ae47); decoder MinimapSlot_FindByPackedId @0x57a270;
// PlayerProfile_InitDefaults @0x54BB40; serialized by
// CNapiServerInfo_SerializeToSession @0x4C3650]. The companion avatar byte is
// the selected combo's head voice unless the profile carries an explicit
// override (Avatars_ResolveSelectionIndex (ex sub_57AE60) @0x57AE60).

#include <cstdint>

namespace opennova {

namespace character_id {

inline constexpr uint16_t kNationalityMask = 0x1F; // 5 bits, bits 0..4
inline constexpr int kDivisionShift = 5;
inline constexpr uint16_t kDivisionMask = 0x0F;    // 4 bits, bits 5..8
inline constexpr int kComboShift = 9;
inline constexpr uint16_t kComboMask = 0x3F;       // 6 bits, bits 9..14
inline constexpr int kAlignmentShift = 15;         // 1 bit: 0 good / 1 evil

constexpr uint16_t pack(int nationality, int division, int combo, int alignment) {
	return static_cast<uint16_t>(
			(nationality & kNationalityMask) |
			((division & kDivisionMask) << kDivisionShift) |
			((combo & kComboMask) << kComboShift) |
			((alignment != 0 ? 1 : 0) << kAlignmentShift));
}

constexpr int nationality(uint16_t packed) { return packed & kNationalityMask; }
constexpr int division(uint16_t packed) {
	return (packed >> kDivisionShift) & kDivisionMask;
}
constexpr int combo(uint16_t packed) { return (packed >> kComboShift) & kComboMask; }
constexpr int alignment(uint16_t packed) { return (packed >> kAlignmentShift) & 1; }

// The stock-Avatars.def fresh-profile defaults [orig: PlayerProfile_InitDefaults
// @0x54bbe0..0x54bc24; wire: retail join f=199140]: {nat=0,div=0,combo=1,good}
// and {nat=7,div=0,combo=1,evil} (JoinerConnection seeds the same two).
static_assert(pack(0, 0, 1, 0) == 0x0200);
static_assert(pack(7, 0, 1, 1) == 0x8207);
static_assert(nationality(0x8207) == 7 && division(0x8207) == 0 &&
              combo(0x8207) == 1 && alignment(0x8207) == 1);
static_assert(alignment(0x0200) == 0 && combo(0x0200) == 1);

} // namespace character_id

} // namespace opennova
