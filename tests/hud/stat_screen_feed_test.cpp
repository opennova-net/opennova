// The stat.mnu RESULTLIST feed (hud/stat_screen_feed.h): the column set with
// the field->string map and the SMALL/disabled toggle, the width split, the
// per-player rows joined by slot to the board, the cell formats, the team
// colours, the fill's cell values and its local selection after the sort, and
// the tab filter
// [orig: StatScreen_PopulateStatResultsList @0x562240; StatScreen_StatFilterTabHandler
//  @0x562140].
#include <cstdio>
#include <string>
#include <vector>

#include <base/title/title.h>
#include <formats/mnu/mnu.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/mission_exit.h>
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
	const std::vector<StatScreenRow> rows = stat_screen_rows(make_board(), players, false);
	CHECK(rows.size() == 2); // team 0 skipped, no board row skipped
	CHECK(rows[0].name == "Ace" && rows[0].squad == "ALPHA" && rows[0].color_argb == 0xFF00AFFFu);
	// field 1 -> "7", field 5 -> "%2i:%02i" of 125 s = " 2:05", field 31 (-1)
	// -> "-", field 99 -> "??".
	CHECK(rows[0].cells.size() == 4);
	CHECK(rows[0].cells[0] == "7" && rows[0].cells[1] == " 2:05" && rows[0].cells[2] == "-" &&
			rows[0].cells[3] == "??");
	CHECK(rows[1].name == "Bee" && rows[1].squad == "-" && rows[1].color_argb == 0xFFFF0000u);
	CHECK(rows[1].cells[0] == "2" && rows[1].cells[1] == " 1:01" && rows[1].cells[2] == "9");
	// With the disabled field listed, field 19 always prints "%i" (even -1).
	const std::vector<StatScreenRow> all = stat_screen_rows(make_board(), players, true);
	CHECK(all[0].cells.size() == 5 && all[0].cells[2] == "-1");
}

// stat.mnu's STAT screen with its RESULTLIST (no COLUMN, no MULTISELECT).
mnu::Document stat_document() {
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
	return doc;
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
	const mnu::Document doc = stat_document();
	menu::MenuRuntime rt;
	rt.open_document(&doc, "stat.mnu", "");
	const int id = rt.widget_id("RESULTLIST");
	CHECK(id >= 0);
	std::vector<StatScreenColumn> columns = stat_screen_columns(make_board(), false, 750);
	CHECK(columns.size() >= 3);
	std::vector<StatScreenPlayer> players = { {3, 1, "J\xF6rg", "\xC9quipe"}, {5, 2, "Jack", ""} };
	const std::vector<StatScreenRow> rows = stat_screen_rows(make_board(), players, false);
	fill_stat_results(rt, id, columns, rows, -1);
	CHECK(rt.table_row_count(id) == 2);
	// The stat sort ran (kills descending: 7 then 2), so row 0 is the slot-3 row.
	CHECK(rt.table_cell_text(id, 0, 0) == "J\xF6rg" && rt.table_cell_text(id, 0, 1) == "\xC9quipe");
	CHECK(rt.table_cell_text(id, 1, 0) == "Jack" && rt.table_cell_text(id, 1, 1) == "-");
	CHECK(rt.table_cell_text(id, 0, 2) == "7");
	rt.table_set_column_ascending(id, 0, true);
	rt.table_sort_by_column(id, 0);
	CHECK(rt.table_cell_text(id, 0, 0) == "Jack" && rt.table_cell_text(id, 1, 0) == "J\xF6rg");
}

// The fill's cell values and its local row (D-MNU-51): each row goes in with the
// slot's team as its column-0 value and its slot id as column 1's, and after the
// sort the first row whose column-1 value is the local player entity's slot id
// is selected; a slot no row holds selects none. Ours flagged a row before the
// sort, by the 0x1D header's board index, and wrote neither value.
// [orig: StatScreen_PopulateStatResultsList @0x562240 - the CTableWnd_AddRow
//  call with the team @0x562565, the column-1 value call @0x562576, the sort
//  @0x5626e6..0x5626f9, the row walk @0x562700..0x562754 comparing
//  g_LocalPlayerEntity +0x154 @0x56271e..0x56272e, CTableWnd_SetRowSelected
//  @0x562754]
void test_fill_selects_the_local_slot_after_the_sort() {
	const mnu::Document doc = stat_document();
	menu::MenuRuntime rt;
	rt.open_document(&doc, "stat.mnu", "");
	const int id = rt.widget_id("RESULTLIST");
	const std::vector<StatScreenColumn> columns = stat_screen_columns(make_board(), false, 750);
	// The roster lists slot 5 first; the sort (kills descending, 7 then 2) puts
	// slot 3's row first.
	const std::vector<StatScreenPlayer> players = { { 5, 2, "Bee", "" }, { 3, 1, "Ace", "ALPHA" } };
	const std::vector<StatScreenRow> rows = stat_screen_rows(make_board(), players, false);
	fill_stat_results(rt, id, columns, rows, 5);
	CHECK(rt.table_row_count(id) == 2);
	CHECK(rt.table_cell_text(id, 0, 0) == "Ace" && rt.table_cell_text(id, 1, 0) == "Bee");
	CHECK(rt.table_cell_text(id, 0, 1) == "ALPHA" && rt.table_cell_text(id, 1, 1) == "-");
	CHECK(rt.table_cell_value(id, 0, 0) == 1 && rt.table_cell_value(id, 0, 1) == 3);
	CHECK(rt.table_cell_value(id, 1, 0) == 2 && rt.table_cell_value(id, 1, 1) == 5);
	CHECK(rt.table_selected_rows(id) == std::vector<int>{ 1 });
	fill_stat_results(rt, id, columns, rows, 3);
	CHECK(rt.table_selected_rows(id) == std::vector<int>{ 0 });
	// Slot 9 holds no row: nothing is selected.
	fill_stat_results(rt, id, columns, rows, 9);
	CHECK(rt.table_selected_rows(id).empty());
}

// The tab filter shows or hides the filled rows by their column-0 value (the team),
// keeping the rows, their order and the selection (D-MNU-53): tab 1 the team-2 rows,
// tab 2 the team-1 rows, tab 0 every row, another index nothing.
// [orig: StatScreen_StatFilterTabHandler @0x562140; CTableWnd_SetRowVisible @0x6401c0]
void test_tab_filter_hides_filled_rows() {
	const mnu::Document doc = stat_document();
	menu::MenuRuntime rt;
	rt.open_document(&doc, "stat.mnu", "");
	const int id = rt.widget_id("RESULTLIST");
	const std::vector<StatScreenColumn> columns = stat_screen_columns(make_board(), false, 750);
	const std::vector<StatScreenPlayer> players = { { 5, 2, "Bee", "" }, { 3, 1, "Ace", "ALPHA" } };
	fill_stat_results(rt, id, columns, stat_screen_rows(make_board(), players, false), 5);
	CHECK(rt.table_row_count(id) == 2);
	CHECK(rt.table_row_value(id, 0) == 1 && rt.table_row_value(id, 1) == 2);
	CHECK(rt.table_row_visible(id, 0) && rt.table_row_visible(id, 1));
	stat_screen_filter_rows(rt, id, 1);
	CHECK(!rt.table_row_visible(id, 0) && rt.table_row_visible(id, 1));
	CHECK(rt.table_row_count(id) == 2 && rt.table_cell_text(id, 0, 0) == "Ace");
	CHECK(rt.table_selected_rows(id) == std::vector<int>{ 1 });
	stat_screen_filter_rows(rt, id, 2);
	CHECK(rt.table_row_visible(id, 0) && !rt.table_row_visible(id, 1));
	stat_screen_filter_rows(rt, id, 7);
	CHECK(rt.table_row_visible(id, 0) && !rt.table_row_visible(id, 1));
	stat_screen_filter_rows(rt, id, 0);
	CHECK(rt.table_row_visible(id, 0) && rt.table_row_visible(id, 1));
	CHECK(rt.table_selected_rows(id) == std::vector<int>{ 1 });
}

// DFX2-97: the STAT screen's DONE. dfx2.exe's show hides it, its menu-screen pass shows it while
// the joiner's S2C 0x25 round-reset hold is up, and its press stores mission exit 4 and ends the
// end-round linger; Jointops.exe registers no DONE and its pass's call is an empty stub.
// [orig: StatScreen_ShowCallback @0x561780 (@0x561812..0x561834); StatScreen_UpdateDoneButton
//  @0x561000; StatScreen_OnDone @0x561130 (@0x561143, @0x56114d) (dfx2.exe); nullsub_130 @0x562130]
void test_done_button() {
	mnu::Document doc = stat_document();
	mnu::Window done;
	done.name = "DONE";
	done.type = mnu::WindowType::Button;
	doc.screens[0].roots[0].children.push_back(done);
	menu::MenuRuntime rt;
	rt.open_document(&doc, "stat.mnu", "");
	const int id = rt.widget_id("DONE");
	CHECK(id >= 0 && rt.is_widget_shown(id));
	const Title jo;  // Jointops.exe's row: no DONE
	Title dfx2;
	dfx2.stat_screen_done = true;
	CHECK(!stat_screen_hide_done(rt, &jo) && rt.is_widget_shown(id));
	CHECK(!stat_screen_show_done(rt, &jo, true));
	// The show hides it; the pass shows it only while the hold is up.
	CHECK(stat_screen_hide_done(rt, &dfx2) && !rt.is_widget_shown(id));
	CHECK(!stat_screen_show_done(rt, &dfx2, false) && !rt.is_widget_shown(id));
	CHECK(stat_screen_show_done(rt, &dfx2, true) && rt.is_widget_shown(id));
	// A menu with no DONE does nothing.
	menu::MenuRuntime bare;
	const mnu::Document plain = stat_document();
	bare.open_document(&plain, "stat.mnu", "");
	CHECK(!stat_screen_hide_done(bare, &dfx2) && !stat_screen_show_done(bare, &dfx2, true));
	// The press: a joiner's runtime takes exit 4 and its linger ends; JO's title, or no joiner
	// runtime (the authority, whose DONE never shows), takes nothing.
	inmatch::ClientRuntime joiner("DoneJoiner");
	joiner.state().end_round_linger_ticks = 1240;
	CHECK(!stat_screen_done_pressed(&jo, &joiner));
	CHECK(joiner.state().end_round_linger_ticks == 1240 &&
			joiner.mission_exit_reason() != inmatch::kMissionExitRoundOver);
	CHECK(!stat_screen_done_pressed(&dfx2, nullptr));
	CHECK(stat_screen_done_pressed(&dfx2, &joiner));
	CHECK(joiner.state().end_round_linger_ticks == 0);
	CHECK(joiner.mission_exit_reason() == inmatch::kMissionExitRoundOver);
}

} // namespace

int main() {
	test_columns();
	test_rows();
	test_fill_puts_the_roster_bytes_in_the_cells();
	test_fill_selects_the_local_slot_after_the_sort();
	test_tab_filter_hides_filled_rows();
	test_done_button();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("stat_screen_feed_test OK\n");
	return 0;
}
