#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace opennova::np {

// The anti-cheat row hashed by the retail function currently named
// AnimMap_GetSlotChecksum in the IDB is one g_CharAttr CHARACTER record, not
// animation-map data. The boot loader owns sixteen fixed 31-dword records.
// [orig: CharAttr_LoadFromDef @0x412140; checksum @0x412AA0]
inline constexpr std::size_t kCharAttrChallengeRowBytes = 124;
inline constexpr std::size_t kCharAttrChallengeRowCount = 16;

using CharAttrChallengeRow =
		std::array<uint8_t, kCharAttrChallengeRowBytes>;

struct CharAttrChallengeTable {
	std::array<CharAttrChallengeRow, kCharAttrChallengeRowCount> rows{};
};

// Parse the checksum-visible bytes produced by CharAttr_LoadFromDef. `out` is
// always cleared first. CHARACTER1..CHARACTER16 are loaded sequentially and the
// first missing section terminates enumeration, exactly like retail.
//
// A false result means there was no source buffer. A syntactically valid or
// invalid non-empty config still has a well-defined zero/inactive table, just
// as a successfully opened ConfigFile with absent CHARACTER sections does.
bool parse_charattr_challenge_table(
		const uint8_t *data, std::size_t size, CharAttrChallengeTable &out);

// Retail selects `(uint8_t(class_id - 1) & 0x0f)`, then rejects the row unless
// its active dword is nonzero and its embedded class byte equals class_id.
// This deliberately makes class 0 and every out-of-range class inactive.
const CharAttrChallengeRow *find_charattr_challenge_row(
		const CharAttrChallengeTable &table, uint8_t class_id);

// S2C 0x41 clears one checksum-visible property across all sixteen rows.
// The first payload byte is the property id (a missing byte defaults to 0):
// 0=ATTRIBUTES, 2=STEALTH, 3=HPBONUS, 4=MANABONUS, 5=RECOIL_MUTE, 6=XHAIR_MUTE,
// 7=SCOPE_MUTE, 8=XHAIRDX_MUTE, 9=RELOAD_MUTE. Id 1 and ids >= 10 are the only
// no-ops.
void clear_charattr_challenge_property(
		CharAttrChallengeTable &table, uint8_t property_id);

} // namespace opennova::np
