// The stat.mnu RESULTLIST feed (hud/stat_screen_feed.h): the column set with
// the field->string map and the SMALL/disabled toggle, the width split, the
// per-player rows joined by slot to the board, the cell formats, the team
// colours, the local selection, and the tab filter
// [orig: StatScreen_PopulateStatResultsList @0x562240; StatScreen_StatFilterTabHandler
//  @0x562140].
#include <cstdio>
#include <string>
#include <vector>

#include <formats/mnu/mnu.h>
#include <runtime/inmatch/stat_screen_feed.h>
#include <runtime/menu/menu_runtime.h>

using namespace opennova;
using namespace opennova::inmatch;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

EndRoundStats make_board() {
	EndRoundStats b;
	b.team_fields = {{1, 1}, {5, 1}, {19, 0}, {31, 1}, {99, 1}};
	EndRoundPlayerRow ace;
	ace.slot = 3;
	ace.team = 1;
	ace.per_team = {7, 125, -1, -1, 4};
	EndRoundPlayerRow bee;
	bee.slot = 5;
	bee.team = 2;
	bee.per_team = {2, 61, 3, 9, 0};
	b.players = {ace, bee};
	return b;
}

void test_columns() {
	const std::vector<StatScreenColumn> cols = stat_screen_columns(make_board(), false, 750);
	// NAME + Squad + the four enabled fields (19 is disabled).
	CHECK(cols.size() == 6);
	CHECK(cols[0].width == 150 && cols[0].header_fallback == "!Name");
	CHECK(cols[1].literal == "Squad" && cols[1].width == (750 - 150) / 5);
	CHECK(cols[2].header_key == "STROVER_STATFIELDSMALL01" && cols[2].field_index == 0);
	CHECK(cols[3].header_key == "STROVER_STATFIELDSMALL05");
	// 31 maps to string 27 through the dword_83C840 table.
	CHECK(cols[4].header_key == "STROVER_STATFIELDSMALL27" && cols[4].field_id == 31);
	CHECK(cols[5].header_key == "Unk entry 99");
	// The show-disabled toggle lists field 19 too and swaps to the large keys.
	const std::vector<StatScreenColumn> all = stat_screen_columns(make_board(), true, 750);
	CHECK(all.size() == 7 && all[4].header_key == "STROVER_STATFIELD19" &&
			all[2].header_key == "STROVER_STATFIELD01");
}

void test_rows() {
	std::vector<StatScreenPlayer> players = {
		{3, 1, "Ace", "ALPHA"}, {5, 2, "Bee", ""}, {7, 1, "NoRow", ""}, {9, 0, "Spectator", ""}};
	const std::vector<StatScreenRow> rows = stat_screen_rows(make_board(), players, false, 5);
	CHECK(rows.size() == 2); // team 0 skipped, no board row skipped
	CHECK(rows[0].name == "Ace" && rows[0].squad == "ALPHA" && rows[0].color_argb == 0xFF00AFFFu);
	CHECK(!rows[0].selected);
	// field 1 -> "7", field 5 -> "%2i:%02i" of 125 s = " 2:05", field 31 (-1)
	// -> "-", field 99 -> "??".
	CHECK(rows[0].cells.size() == 4);
	CHECK(rows[0].cells[0] == "7" && rows[0].cells[1] == " 2:05" && rows[0].cells[2] == "-" &&
			rows[0].cells[3] == "??");
	CHECK(rows[1].name == "Bee" && rows[1].squad == "-" && rows[1].color_argb == 0xFFFF0000u);
	CHECK(rows[1].selected);
	CHECK(rows[1].cells[0] == "2" && rows[1].cells[1] == " 1:01" && rows[1].cells[2] == "9");
	// With the disabled field listed, field 19 always prints "%i" (even -1).
	const std::vector<StatScreenRow> all = stat_screen_rows(make_board(), players, true, -1);
	CHECK(all[0].cells.size() == 5 && all[0].cells[2] == "-1");
	// The tab filter.
	CHECK(stat_screen_row_visible(0, 1) && stat_screen_row_visible(0, 2));
	CHECK(!stat_screen_row_visible(1, 1) && stat_screen_row_visible(1, 2));
	CHECK(stat_screen_row_visible(2, 1) && !stat_screen_row_visible(2, 2));
}

// The fill puts the roster's bytes into the NAME and Squad cells (D-MNU-46): the
// game's table holds the slot's name and squad tag in the code page and draws a
// glyph per byte, so a stock peer's "J\xF6rg" / "\xC9quipe" stay those bytes in
// the cells (the binding's UTF-8 round trip left "J\xEF\xBF\xBDrg", which drew
// three glyphs, and its decoded text "J\xC3\xB6rg" would draw two), and a sort
// on NAME orders the bytes: "Jack" before "J\xF6rg".
// [orig: StatScreen_PopulateStatResultsList @0x562240 - the CTableWnd_AddRow
//  call with the name @0x562565, the column-1 SetCellText with the squad
//  @0x562585; CTableWnd_SortByColumn @0x640900]
void test_fill_puts_the_roster_bytes_in_the_cells() {
	mnu::Document doc;
	mnu::Screen stat;
	stat.name = "STAT";
	mnu::Window root;
	root.name = "ROOT";
	root.type = mnu::WindowType::Window;
	mnu::Window table;
	table.name = "RESULTLIST";
	table.type = mnu::WindowType::Table;
	root.children.push_back(table);
	stat.roots.push_back(root);
	doc.screens.push_back(stat);
	menu::MenuRuntime rt;
	rt.open_document(&doc, "stat.mnu", "");
	const int id = rt.widget_id("RESULTLIST");
	CHECK(id >= 0);
	std::vector<StatScreenColumn> columns = stat_screen_columns(make_board(), false, 750);
	CHECK(columns.size() >= 3);
	std::vector<StatScreenPlayer> players = { {3, 1, "J\xF6rg", "\xC9quipe"}, {5, 2, "Jack", ""} };
	const std::vector<StatScreenRow> rows = stat_screen_rows(make_board(), players, false, -1);
	fill_stat_results(rt, id, columns, rows);
	CHECK(rt.table_row_count(id) == 2);
	// The stat sort ran (kills descending: 7 then 2), so row 0 is the slot-3 row.
	CHECK(rt.table_cell_text(id, 0, 0) == "J\xF6rg" && rt.table_cell_text(id, 0, 1) == "\xC9quipe");
	CHECK(rt.table_cell_text(id, 1, 0) == "Jack" && rt.table_cell_text(id, 1, 1) == "-");
	CHECK(rt.table_cell_text(id, 0, 2) == "7");
	rt.table_set_column_ascending(id, 0, true);
	rt.table_sort_by_column(id, 0);
	CHECK(rt.table_cell_text(id, 0, 0) == "Jack" && rt.table_cell_text(id, 1, 0) == "J\xF6rg");
}

} // namespace

int main() {
	test_columns();
	test_rows();
	test_fill_puts_the_roster_bytes_in_the_cells();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("stat_screen_feed_test OK\n");
	return 0;
}
