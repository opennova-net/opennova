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
// start ends the file) and split by Text_SplitIntoTokens @ 0x52D1B0 (white space between tokens; a quote
// toggles a quoted run, the quotes dropped); a line whose first or second character is '/' is read for
// nothing. The first pass takes `VERSION n` (the last one), and a version other than 40 stops the read: the
// defaults are written over the file instead. The second takes `GAMETYPE "name"` (the row of the name, the
// first of the twelve; none for a name no row has [orig: sub_52D850 @ 0x52D850]), `FIELD "name" n` (the
// first of a GAMETYPE's lines clears the row's list; a name the table has, its value a byte), `VAR "name" n`
// (a name the table has, its value a word) and `EXP_FANFARE a b` (two bytes, kept when both are other than 0
// and the second is the greater); FIELD, VAR and EXP_FANFARE take three tokens, GAMETYPE two; anything else
// is read for nothing.
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
// This lib models the file: its blocks as the reader takes them, in the file's order, each with its FIELD
// entries (a list, in order) and its VAR values, the version and the fanfare; the in-memory row image and the
// defaults the file is read over are the runtime's (inmatch::load_session_score_config, which still scans the
// text itself). The writer is ScoreConfig_SaveFile's form, and the shipped score.ini is that writer's output
// byte for byte; over a file's modeled layout (textlayout) a hand-written file is generated as it was.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <formats/textlayout/text_layout.h>

namespace opennova {
namespace score {

// A FIELD or a VAR line's entry: its name (as the file writes it) and its value (a FIELD's the byte the row
// holds, a VAR's the word). A FIELD entry's note names its line in the file's layout (0: none).
struct Entry {
	std::string name;
	int32_t value = 0;
	uint64_t note = 0;
};

// One `GAMETYPE "NAME"` block: its FIELD entries in the file's order (two of a name both kept: the reader
// appends each), its VAR values (one per name, the last line's: the reader writes the word again).
struct GameTypeBlock {
	std::string name;
	std::vector<Entry> fields; // FIELD statements, authored order
	std::vector<Entry> vars;   // VAR statements, authored order (one per name)
	uint64_t note = 0;         // its lines in the file's layout (0: none)
};

struct File {
	int32_t version = 0;             // VERSION n (the last); 0 for none
	int32_t exp_fanfare[2] = {0, 0}; // EXP_FANFARE a b, the two bytes as read
	bool has_exp_fanfare = false;    // an EXP_FANFARE line was read
	std::vector<GameTypeBlock> blocks;
	uint64_t note = 0; // the file's own record in its layout (0: none)
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
// A name's id in its table, -1 for none.
int32_t field_id(std::string_view name);
int32_t var_id(std::string_view name);
// The game types' rows' names, by row index [orig: GameType_CreateDefaultSettings @ 0x52DD00]: TDM 1, COOP
// 2, ... DM 11; row 0 has none the writer puts down.
const std::vector<std::string> &game_type_names();
// The row a GAMETYPE line's name selects, the first of the name without case [orig: sub_52D850 @ 0x52D850];
// -1 for a name no row has.
int game_type_row(std::string_view name);
// Whether the reader keeps the fanfare: both bytes other than 0, the second the greater [orig: @ 0x52DC75..0x52DC9F].
bool exp_fanfare_kept(const File &file);

// The file as ScoreConfig_LoadFile reads it (the lines and tokens above): every GAMETYPE line a block, its
// FIELD and VAR lines whose name the tables have its entries, VERSION and EXP_FANFARE the file's; every other
// line read for nothing. Never fails on a text (the game reads any); false only for no data.
bool parse(const uint8_t *data, size_t size, File &out, std::string &error);
// The same read with the file's layout modeled (`notes` filled; the file's, each block's and each FIELD's note
// set).
bool parse(const uint8_t *data, size_t size, File &out, std::string &error, textlayout::Notes &notes);

// The file's text as ScoreConfig_SaveFile @ 0x52CDD0 writes it, from scratch (ADR 0003): its header, `VERSION
// 40`, `EXP_FANFARE a b`, the FIELD names in a comment block, then each block after two blank lines, its
// FIELD lines and, after a blank one, its VAR lines in the VAR table's order, each line ending CR LF
// (File_WriteLineToHandle @ 0x437010). Over the file's modeled layout where the file has one (each line as the
// file had it but for a changed value's; an entry put down anew after the one before it in the writer's order;
// the blocks and their FIELD lines in the document's order). The text is read again: one that would not read
// back as the file is written in the writer's form, `rewritten` set. A block of more than 34 FIELD lines is
// written whole: the reader drops the lines past its row's 34 (sub_52CD70 @ 0x52CD70), which a file may hold.
// False with the reason for a block or an entry no line of the reader's reads (a name the tables lack, a name
// holding a quote).
bool write(const File &file, std::vector<uint8_t> &out, std::string &error);
bool write(const File &file, const textlayout::Notes *notes, std::vector<uint8_t> &out, std::string &error,
           bool *rewritten = nullptr);

// Value lookups by NAME, without case.
const GameTypeBlock *block_at(const File &file, int row);
int32_t var_value(const GameTypeBlock &block, std::string_view name, int32_t fallback);
int32_t field_value(const GameTypeBlock &block, std::string_view name, int32_t fallback);

// Value equality — true iff two models read the same (the layout aside).
bool equal(const File &a, const File &b);

} // namespace score
} // namespace opennova
