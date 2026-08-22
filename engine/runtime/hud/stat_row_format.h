#pragma once

#include <cstdint>
#include <string>

namespace opennova::hud {

// THE POST-ROUND STAT BOARD's ROW FORMATTING — how each cell of the stat.mnu
// results table is rendered [orig: populate_stat_results_list @0x562240].
//
// The board is a table of Name, Squad and one column per ACTIVE stat field.
// This carries the per-cell formatting and the row colouring; the 0x56 decode
// and fold already supply the values, and the table sort orders the rows.

// A stat field renders one of three ways, selected by the field's own type
// [orig: the `[14] == 1` / `== 2` tests around @0x562300].
enum class StatFieldKind {
	Integer = 0, // plain "%i"
	Time = 1,    // "m:ss" — see below
	Other = 2,
};

// AN UNSET VALUE IS A DASH, NOT A ZERO [orig: strcpy(buf, "-") @0x5622C2 and
// the value arm @0x5622F4]. That distinction is the whole point of the column:
// a player who has not scored shows "-", and a player who scored zero shows
// "0". Rendering both as 0 loses which is which.
inline constexpr const char *kStatUnset = "-";
// A value whose field type is unrecognised renders "??" rather than being
// dropped or guessed [orig: strcpy(buf, "??")].
inline constexpr const char *kStatUnknown = "??";

// TIME FIELDS RENDER AS MINUTES AND SECONDS, from a value in SECONDS
// [orig: the sprintf with `/ 60` and `% 60`]. The seconds are zero-padded and
// the minutes are not, so 61 becomes "1:01" rather than "1:1" or "01:01".
std::string stat_format_time(int32_t seconds);

// One cell's text. `present` is false for a value the board has no entry for.
std::string stat_format_value(int32_t value, StatFieldKind kind, bool present);

// The gametext key for a stat column's header. The board has a LARGE and a
// SMALL layout with separate key families, and each key has a `!`-prefixed
// fallback [orig: the four sprintf arms @0x5622...]:
//   large: STROVER_STATFIELD%02ld      fallback !STROVER_STATFIELD%02ld
//   small: STROVER_STATFIELDSMALL%02ld fallback !STROVER_STATFIELDSMALL%02ld
// A field with no entry in the table renders "Unk entry %d" rather than an
// empty header, so a missing key is visible instead of silently blank.
std::string stat_field_key(int field_id, bool large_layout, bool fallback);
std::string stat_field_unknown_key(int field_id);

// Row colouring is by TEAM, with the local player's row highlighted
// [orig: the team compare and the local-player test @0x5623... — the row is
//  matched by the player's slot against the local player's].
inline constexpr uint32_t kStatRowTeam1 = 0xFF00FFFFu; // cyan
inline constexpr uint32_t kStatRowTeam2 = 0xFFFF0000u; // red
inline constexpr uint32_t kStatRowNeutral = 0xFFFFFFFFu;

inline uint32_t stat_row_color(int team) {
	if (team == 1) return kStatRowTeam1;
	if (team == 2) return kStatRowTeam2;
	return kStatRowNeutral;
}

// The local player's row is highlighted by IDENTITY, not by position — the
// board is sorted, so the local row can be anywhere in it.
inline bool stat_row_is_local(int row_slot, int local_slot) {
	return row_slot == local_slot;
}

// A player with no squad shows a dash in the squad column, same rule as an
// unset stat [orig: strcpy(squad_tag_buf, "-")].
inline std::string stat_squad_text(const std::string &squad) {
	return squad.empty() ? std::string(kStatUnset) : squad;
}

} // namespace opennova::hud
