#pragma once

// THE STAT SCREEN FEED (net-re §5.68; lives beside end_round_protocol in
// inmatch because it reads the npwire EndRoundStats board): what stat.mnu's RESULTLIST table shows
// after the 6-second end-round overlay — the column set and the per-player
// rows, computed from the reassembled S2C 0x56 board the way
// StatScreen_PopulateStatResultsList @0x562240 builds the CTableWnd. The embedder
// resolves the header keys (Overlays/STROVER_STATFIELD%02d or ...SMALL%02d
// through the dword_83C840 field->string map) and feeds the menu driver.
// [orig: StatScreen_PopulateStatResultsList @0x562240; the STAT show callback
//  StatScreen_ShowCallback (ex sub_562840) @0x562840 (hides RADIO_TAB_* when non-team, selects OVERALL);
//  StatScreen_StatFilterTabHandler @0x562140 (tab 1 = team 2 rows, tab 2 = team 1)]

#include <net/npwire/ingame_decode.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {
class MenuRuntime;
}

namespace opennova::inmatch {

struct StatScreenColumn {
	std::string header_key;      // "" for the NAME/Squad columns
	std::string header_fallback; // the "!..." fallback, or the literal
	std::string literal;         // "Squad" (retail literal) / the rtxt NAME lookup
	int width = 0;               // design px inside the table rect
	int field_id = 0;            // 0 for NAME / Squad
	int field_index = -1;        // index into the declared fields
	// The header TEXT the embedder resolved through its gametext table (a
	// keyed header resolves like the ladder: present + non-empty, else the
	// "!..." fallback stripped; the key-less NAME / Squad columns show their
	// fallback stripped). Empty until the embedder fills it.
	std::string header;
};

// The fill [orig: StatScreen_PopulateStatResultsList @0x562240]: the rows cleared
// @0x56227a, the width split read off the table's design rect [orig: CWnd_GetRect
// @0x5622c3], the column count set through the table's vtable +0x6C @0x5622fa,
// then one CTableWnd_InitRow per column. How CTableWnd_InitRow sets every column up
// [orig: @0x562346 / 0x56237a / 0x56242b — justify -1 (read as 1, centred),
// vjustify 32 (bottom)], and the sort the fill
// ends with [orig: sub_63EC30(table, 2, 0) then CTableWnd_SortByColumn(table, 2)
// @0x5626e6..0x5626f9]: the first stat column, descending. Between them the rows
// go in with their colours, a team row's replacing the table's [orig: the row
// colour override CTableWnd_SetRowColor @0x5625a9]; after it the local player's row is
// selected.
inline constexpr int kStatScreenColumnJustify = 1;
inline constexpr int kStatScreenColumnVJustify = 32;
inline constexpr int kStatScreenSortColumn = 2;
inline constexpr bool kStatScreenSortAscending = false;

// The columns [orig: @0x562280..0x5624a0]: NAME (150 px, the table's rtxt
// "NAME" string or "!Name"), "Squad", then one per enabled field with the
// SMALL keys (with the show-disabled toggle: every field, with the large
// keys), each of width (table_width - 150) / (columns - 1); a field outside
// the map header reads "Unk entry %d".
std::vector<StatScreenColumn> stat_screen_columns(const EndRoundStats &board,
		bool show_disabled, int table_width);

struct StatScreenRow {
	uint8_t slot = 0;
	uint8_t team = 0;
	std::string name;   // the player-slot table's name
	std::string squad;  // the slot's squad tag or "-"
	std::vector<std::string> cells; // one per field column, formatted
	uint32_t color_argb = 0xFFFFFFFFu; // team 1 0xFF00AFFF, team 2 0xFFFF0000
};

// One joinable player-slot entry: the roster the rows are walked from
// (retail walks the PLAYER SLOT table, team != 0, and joins the board row by
// slot id [orig: @0x5624b7..0x5624f4]).
struct StatScreenPlayer {
	uint8_t slot = 0;
	uint8_t team = 0;
	std::string name;
	std::string squad;
};

// The rows [orig: @0x5624a3..0x5626fa]: per player with team != 0 whose slot
// has a board row: name, squad or "-", then per listed field the row's
// positional value — field 5 "%2i:%02i", -1 "-", ids 1..4/6..18/20..32 "%i",
// 19 always "%i", anything else "??"; row colour team 1 0xFF00AFFF, team 2
// 0xFFFF0000. The local row is the fill's (fill_stat_results).
std::vector<StatScreenRow> stat_screen_rows(const EndRoundStats &board,
		const std::vector<StatScreenPlayer> &players, bool show_disabled);

// The tab filter [orig: StatScreen_StatFilterTabHandler @0x562140]: tab 0 shows every
// row, tab 1 only team 2, tab 2 only team 1. The game shows or hides each filled
// row by its column-0 value (the team) through CTableWnd_SetRowVisible @0x6401c0,
// keeping the rows, their order and their states; ours's tab change refills the
// table with the rows this admits (D-MNU-53).
bool stat_screen_row_visible(int tab_index, uint8_t team);

// The fill over the menu's table: the column count and one init per column
// (the header, the width, the justification, the compare: NAME and Squad as
// text, a stat field numerically), the rows with their team colours and their
// cell values (column 0 the slot's team, column 1 its slot id), the sort on the
// first stat column, descending, and after it the first row whose column-1
// value is `local_slot` (the local player entity's slot id, +0x154) selected
// [orig: the AddRow call's team @0x562550..0x562565, the column-1 value call
// @0x562576 (CTableWnd_SetCellValue @0x6420f0, ex
// CTableWnd_SetCellValue), the row walk @0x562700..0x562754] (D-MNU-51). The NAME
// and Squad cells are the player slot's name and squad tag as the roster holds
// them, its bytes in the code page, as the game's table holds, draws and sorts
// them [orig: the Napi_CopyString calls of the squad @0x56252c and the name
// @0x562548, the CTableWnd_AddRow call with the name @0x562565, the column-1
// CTableWnd_SetCellText with the squad @0x562585]: a code-page menu draws a
// cell's bytes as glyph indices. The headers, which the embedder resolved as
// UTF-8 text, go in as the page's bytes (utf8_to_retail_text) (D-MNU-46).
void fill_stat_results(menu::MenuRuntime &menu, int table,
		const std::vector<StatScreenColumn> &columns, const std::vector<StatScreenRow> &rows,
		int local_slot);

// The end-of-round SESSION facts the shell flow keys on (net-re 5.68), as one
// value the embedder fills from the role's folded ClientEndRoundStats plus
// the role's own facts (the game type, the round clock, the death-screen
// latch, the assigned team, the session-open bit); its Godot record wraps it
// by value (ADR 0043 d10).
struct EndRoundSessionState {
	bool header_known = false;
	bool board_known = false;
	uint32_t game_type = 0;
	int winner = 0;
	int team_score_0 = 0;
	int team_score_1 = 0;
	bool draw = false;
	int my_index = 0;
	int round_ticks = 0;
	bool death_screen = false;
	int local_team = 0;
	// The local player entity's slot id (+0x154) the fill selects the local row
	// by (fill_stat_results); -1 without a role runtime.
	int local_slot = -1;
	bool team_mode = false;
	bool session_open = false;
};

} // namespace opennova::inmatch
