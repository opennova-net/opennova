// score.ini — NovaLogic's scoring configuration.
//
// One text file shipped in the game archives (`VERSION 40` in retail JO). The
// engine slurps it into a malloc'd buffer `dword_24E3E84`, freed by
// [orig: ScoreConfig_FreeBuffer @ 0x52D2E0], and indexes it as a table of
// FIXED 452-BYTE ROWS: one row per GAMETYPE block, in FILE ORDER
// [orig: load_scoring_table_for_game_type @ 0x52D300 — `score_type_index *= 452`].
//
// The row index is chosen from the session's g_GameType, and the shipped file's
// block order matches that ladder exactly:
//
//   index | block | g_GameType arm [orig: @0x52D300]
//   ------+-------+---------------------------------------------------
//     0   | COOP  | the `default` arm, immediately remapped to 2
//     1   | TDM   | g_GameType == 0x10000
//     2   | COOP  | (g & 0xFFFDFFFF) == 0x10020 && (g & 0x20000)  <- objective Co-op
//     3   | TKOTH | 65537        4 | KOTH  | 1         5 | SD  | 589826
//     6   | AD    | 65538        7 | CTF   | 65540     8 | FB  | 65544
//     9   | AAS   | 65552       10 | CAC   | 327696   11 | DM  | g_GameType == 0
//
// The index-2 predicate is literally `game_type::is_waypoint_family(g) &&
// game_type::is_objective(g)` (engine/base/gameprofile/game_type.h). Index 12 exists in
// the ladder (g_GameType == 8) but the loader's own `score_type_index <= 11`
// guard drops it, and the shipped file carries only 12 blocks — so it can never
// resolve. Retail's two COOP blocks (0 and 2) are byte-identical in the shipped
// file, which is why the 0 -> 2 remap is unobservable.
//
// Individual values are read as `row + 300 + 4 * entryIndex`
// [orig: ScoreConfig_GetRowEntry (ex sub_52D430) @ 0x52D430], and `row + 24` holds the block's team count
// (`g_scoreTeamCount` @0x52D300). Per ADR 0030 only the file knowledge lives
// here; the retail consumers are the scoring dispatch
// [orig: GameEvent_ProcessScoring @ 0x52F550] and the change-gated S2C 0x81
// score mirror [orig: Server_UpdateCaptureZoneProximity @ 0x5086A0].
//
// STAGED, NOT WIRED (2026-09-20 tidy): the live score.ini reader is
// inmatch::load_session_score_config (engine/runtime/inmatch/session_status.cpp),
// which still scans the text itself while applying the witnessed selection
// rules (the VERSION 40 gate, reset-and-replace on a duplicate GAMETYPE
// section, the 34-FIELD cap) on top of world::default_match_score_values. It
// is the owner-to-be: that function reading its rows through score::parse.
// The write-only world::ScoreRules feed that used to include this header is
// gone (world::Match owns the awards). Consumed by
// tests/score/score_roundtrip_test.cpp only until then
// (scripts/lint/orphan_header_check.py).
//
// COVERAGE GAP (stated per ADR 0030 for a partial port): this lib models the
// file's TEXT grammar — the authored VERSION / EXP_FANFARE / GAMETYPE / FIELD /
// VAR statements. The engine's in-memory 452-byte row IMAGE is a separate
// artifact; the `entryIndex` ordering that maps a VAR name to its `300 + 4*i`
// slot is NOT yet witnessed (only two indices are known from call sites:
// 0x24 = the scoring interval and 0x0C = the capture threshold
// [orig: Server_UpdateCaptureZoneProximity preamble @0x5086A0: entry 0x24
//  @0x5086C4 (defaults to 0xFFFF when < 1 @0x5086D5), entry 0x0C @0x5086E5
//  (clamped to >= 1 @0x5086F6)]). Until
// that ordering is witnessed, look values up
// BY NAME with `var_value()`; do not synthesise a row image.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {
namespace score {

// A `FIELD "NAME" n` or `VAR "NAME" n` statement, in authored order.
struct Entry {
	std::string name;
	int32_t value = 0;
};

// One `GAMETYPE "NAME"` block. Position in File::blocks IS the engine's row
// index [orig: @0x52D300].
struct GameTypeBlock {
	std::string name;
	std::vector<Entry> fields; // FIELD statements, authored order
	std::vector<Entry> vars;   // VAR statements, authored order
};

struct File {
	int32_t version = 0;           // VERSION n
	int32_t exp_fanfare[2] = {0, 0}; // EXP_FANFARE a b
	std::vector<GameTypeBlock> blocks;
};

// The witnessed row-index ladder [orig: load_scoring_table_for_game_type
// @ 0x52D300]. Returns the score-table row for a g_GameType code word, or -1
// when the loader's `<= 11` guard would reject it. The 0 -> 2 remap is applied
// here exactly as the original applies it (`if (!score_type_index) index = 2`).
int row_for_game_type(uint32_t game_type);

// Parse score.ini text. Comments are `//` to end of line; statements are
// whitespace-separated with quoted names. Returns false and fills `error` on a
// malformed statement.
bool parse(const uint8_t *data, size_t size, File &out, std::string &error);

// Canonical text for `file`. Written from scratch, never echoing input bytes
// (docs/adr/0003-no-raw-passthrough-create-from-scratch.md): comments and the
// authored blank-line layout are NOT preserved, so a round trip is byte-exact
// only against canonically-formatted input. parse->write->parse is idempotent
// for any input.
bool write(const File &file, std::vector<uint8_t> &out, std::string &error);

// Value lookups by NAME (see the coverage gap in the file header — the
// name -> entryIndex ordering behind `ScoreConfig_GetRowEntry`'s `300 + 4*i` is unwitnessed,
// so name lookup is the only sound access path today).
const GameTypeBlock *block_at(const File &file, int row);
int32_t var_value(const GameTypeBlock &block, std::string_view name, int32_t fallback);
int32_t field_value(const GameTypeBlock &block, std::string_view name, int32_t fallback);

// Value equality — true iff two models would write() identical bytes.
bool equal(const File &a, const File &b);

} // namespace score
} // namespace opennova
