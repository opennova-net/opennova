// The stat.mnu RESULTLIST feed (hud/stat_screen_feed.h): the column set with
// the field->string map and the SMALL/disabled toggle, the width split, the
// per-player rows joined by slot to the board, the cell formats, the team
// colours, the local selection, and the tab filter
// [orig: StatScreen_PopulateStatResultsList @0x562240; StatScreen_StatFilterTabHandler
//  @0x562140].
#include <cstdio>
#include <string>
#include <vector>

#include <runtime/inmatch/stat_screen_feed.h>

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
	CHECK(rows[0].name == "Ace" && rows[0].squad == "ALPHA" && rows[0].color_argb == 0xFF00BFFFu);
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

} // namespace

int main() {
	test_columns();
	test_rows();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("stat_screen_feed_test OK\n");
	return 0;
}
