// score.ini — NovaLogic's scoring configuration.
//
// STAGED, NOT WIRED (2026-09-20 tidy): the live score.ini reader is
// inmatch::load_session_score_config (engine/runtime/inmatch/session_status.cpp),
// which still scans the text itself while applying the witnessed selection
// rules (the VERSION 40 gate, reset-and-replace on a duplicate GAMETYPE
// section, the 34-FIELD cap) on top of world::default_match_score_values. It
// is the owner-to-be: that function reading its rows through score::parse.
// The editor's score table reads and writes the file through it (the editor
// trunk, PR #665). Consumed by tests/score/score_roundtrip_test.cpp on master
// until then (scripts/lint/orphan_header_check.py).
//
// One text file read loose by its bare name from the game's folder (`VERSION 40` in retail JO). The
// game types' default settings are made first, twelve FIXED 452-BYTE ROWS [orig:
// GameType_CreateDefaultSettings @ 0x52DD00, its buffer 0x1534 bytes: the two fanfare bytes, then the rows
// from +4], and the file read over them when it is there, else the defaults written to it [orig: @
// 0x52F50C..0x52F528, File_IsSingleFile then ScoreConfig_LoadFile @ 0x52D8A0 or ScoreConfig_SaveFile @
// 0x52CDD0]. A row is its name (+4), its FIELD list (the count at +20, up to 34 pairs of an id and a byte at
// +24 [orig: sub_52CD70 @ 0x52CD70]) and its 39 VAR words (+296), read as `row + 300 + 4 * entryIndex`
// [orig: ScoreConfig_GetRowEntry (ex sub_52D430) @ 0x52D430] by the scoring dispatch [orig:
// GameEvent_ProcessScoring @ 0x52F550] and the change-gated S2C 0x81 score mirror [orig:
// Server_UpdateCaptureZoneProximity @ 0x5086A0]. A VAR's entryIndex is its id in the VAR name table
// (var_names below, [orig: the VAR names @ 0x830348]), a FIELD's id its id in the FIELD name table ([orig:
// the FIELD names @ 0x830240]): the two entries the capture zones read are ids 36 ALIVEQUANTUM, the scoring
// interval, and 12 ZONEQUANTUM, the capture threshold [orig: Server_UpdateCaptureZoneProximity preamble @
// 0x5086A0: entry 0x24 @0x5086C4 (defaults to 0xFFFF when < 1 @0x5086D5), entry 0x0C @0x5086E5 (clamped to >=
// 1 @0x5086F6)]. The rows' buffer is freed by ScoreConfig_FreeBuffer @ 0x52D2E0.
//
// The reader [orig: ScoreConfig_LoadFile @ 0x52D8A0] reads the file in two passes over its lines, each line
// cut by Text_ReadLine @ 0x52D110 (a CR LF, an LF or a lone CR ends it; a NUL ends it too, and at a line's
// start ends the file; past 2047 characters it is cut and the next byte stepped over) and split by
// Text_SplitIntoTokens @ 0x52D1B0 (white space between tokens; a quote toggles a quoted run, the quotes
// dropped); a line whose first or second character is '/' is read for nothing. The first pass takes
// `VERSION n` (the last one), and a version other than 40 stops the read: the defaults are written over the
// file instead. The second takes `GAMETYPE "name"` (the row of the name, the
// first of the twelve; none for a name no row has [orig: sub_52D850 @ 0x52D850]), `FIELD "name" n` (the
// first of a GAMETYPE's lines clears the row's list; a name the table has, its value a byte), `VAR "name" n`
// (a name the table has, its value a word) and `EXP_FANFARE a b` (two bytes, kept when both are other than 0
// and the second is the greater); FIELD, VAR and EXP_FANFARE take three tokens, GAMETYPE two; anything else
// is read for nothing. A GAMETYPE line of a name no row has selects none, and the FIELD and VAR lines after it
// are read for nothing [orig: `if (currentGameType)` @ 0x52DB7C, @ 0x52DC05].
//
// The row index is chosen from the session's g_GameType [orig: ScoreConfig_LoadScoringTableForGameType @
// 0x52D300], and the defaults' writer puts the rows down in that ladder's order:
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
// guard drops it. The writer puts row 2 down first under index 0 (`if (!i) i = 2` @ 0x52CF4C), so the
// shipped file's two COOP blocks are both row 2's, and its blocks stand in the ladder's order.
//
// This lib models the file: its blocks as the reader takes them, in the file's order, each the row its
// GAMETYPE line selects with its FIELD entries (a list, in order, the 34 the row holds) and its VAR values, each
// entry by its id in its name table, the version and the fanfare; the in-memory row image and the defaults the
// file is read over are the runtime's (inmatch::load_session_score_config, which still scans the text itself).
// No spelling of the file's is kept: the writer is ScoreConfig_SaveFile's form, which puts down each name from
// its table by the id, and the shipped score.ini is that writer's output byte for byte.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {
namespace score {

// A FIELD or a VAR line's entry: its id in its name table (field_names / var_names: the row holds the id, and the
// writer puts down the table's name of it [orig: ScoreConfig_SaveFile @ 0x52CFC0..0x52CFF5, @ 0x52D030..0x52D06B])
// and its value (a FIELD's the byte the row holds, a VAR's the word).
struct Entry {
	int32_t id = 0;
	int32_t value = 0;
};

// One `GAMETYPE "NAME"` block: the row its name selects (0..11, game_type_row; the writer puts down the row's own
// name [orig: @ 0x52CF81]), its FIELD entries in the file's order (two of an id both kept: the reader appends
// each, up to the row's 34), its VAR values (one per id, the last line's: the reader writes the word again).
struct GameTypeBlock {
	int row = 2;
	std::vector<Entry> fields; // the row's FIELD list, in order
	std::vector<Entry> vars;   // one per id
};

struct File {
	int32_t version = 40; // VERSION n (the last; the reader's 0 for none); the game's own 40 (kVersion)
	// The fanfare pair the reader stores: the two bytes of the last EXP_FANFARE line whose pair passes its gate
	// (exp_fanfare_kept), each line stored over the last; the defaults' 0 0 where none passes [orig:
	// ScoreConfig_LoadFile @ 0x52DC75..0x52DC9F]. The game's writer puts it down as its one EXP_FANFARE line.
	int32_t exp_fanfare[2] = {0, 0};
	std::vector<GameTypeBlock> blocks;
};

// The witnessed row-index ladder [orig: ScoreConfig_LoadScoringTableForGameType
// @ 0x52D300]. Returns the score-table row for a g_GameType code word, or -1
// when the loader's `<= 11` guard would reject it. The 0 -> 2 remap is applied
// here exactly as the original applies it (`if (!score_type_index) index = 2`).
int row_for_game_type(uint32_t game_type);

// The version the reader takes [orig: ScoreConfig_LoadFile @ 0x52DA8A, `version == 40`].
inline constexpr int32_t kVersion = 40;
// A row's FIELD list holds 34 [orig: sub_52CD70 @ 0x52CD70, `count >= 34` refuses].
inline constexpr size_t kMaxFields = 34;

// The FIELD and VAR name tables, each name with its id, in the tables' order [orig: the FIELD names @
// 0x830240, ids 1..32; the VAR names @ 0x830348, ids 0..37]; a name compares without case.
struct Name {
	const char *name;
	int32_t id;
};
const std::vector<Name> &field_names();
const std::vector<Name> &var_names();
// A name's id in its table, -1 for none; an id's name in its table, null for none.
int32_t field_id(std::string_view name);
int32_t var_id(std::string_view name);
const char *field_name(int32_t id);
const char *var_name(int32_t id);
// The game types' rows' names, by row index [orig: GameType_CreateDefaultSettings @ 0x52DD00]: TDM 1, COOP
// 2, ... DM 11; row 0 has none the writer puts down.
const std::vector<std::string> &game_type_names();
// The row a GAMETYPE line's name selects, the first of the name without case [orig: sub_52D850 @ 0x52D850];
// -1 for a name no row has.
int game_type_row(std::string_view name);
// Whether the reader keeps the model's fanfare pair from a line: both bytes other than 0, the second the greater
// [orig: @ 0x52DC75..0x52DC9F]. A pair that does not reads back as the defaults' 0 0.
bool exp_fanfare_kept(const File &file);

// The file as ScoreConfig_LoadFile reads it (the lines and tokens above): VERSION the file's (0 for none); at
// version 40 every GAMETYPE line of a row's name a block, its FIELD lines whose name the table has its entries
// up to the row's 34 [orig: sub_52CD70 @ 0x52CD81, `count >= 34` refuses] and its VAR lines', the pair its
// EXP_FANFARE lines store; every other line read for nothing, and at any other version every line but VERSION
// (no block, the defaults' fanfare: the game reads none of the file and writes its defaults over it). Never
// fails on a text (the game reads any); false only for no data.
bool parse(const uint8_t *data, size_t size, File &out, std::string &error);

// The file's text as ScoreConfig_SaveFile @ 0x52CDD0 writes it, from the model alone (ADR 0003): its header,
// `VERSION 40` (the model's version: a File's own is the game's 40, a file read at another keeps it),
// `EXP_FANFARE a b`, the FIELD names in a comment block, then each block after two blank lines, its row's name,
// its FIELD lines in the model's order and, after a blank one, its VAR lines in the VAR table's order, each name
// the table's of its id and each line ending CR LF (File_WriteLineToHandle @ 0x437010). The text read again is
// the model (parse, equal); false with the reason, nothing written, for a model it would not be: a row or an id
// the tables lack, more FIELD entries than a row's 34, two VAR entries of an id, a fanfare pair other than the
// defaults' 0 0 the reader's gate refuses (exp_fanfare_kept), blocks or a pair at a version other than 40 (the
// reader takes none of them).
bool write(const File &file, std::vector<uint8_t> &out, std::string &error);

// Value lookups by an entry's name, without case (its table's id).
const GameTypeBlock *block_at(const File &file, int row);
int32_t var_value(const GameTypeBlock &block, std::string_view name, int32_t fallback);
int32_t field_value(const GameTypeBlock &block, std::string_view name, int32_t fallback);

// Value equality — true iff two models read the same.
bool equal(const File &a, const File &b);

} // namespace score
} // namespace opennova
