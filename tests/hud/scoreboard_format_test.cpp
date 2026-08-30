// The Tab player list's row policy [orig: HUD_DrawKillList @0x423A30; the
// glyph chain @0x423ef1-0x4240e8; the two row formats @0x7c4bc4 / @0x7c4c00].
// The interesting content here is ORDER and MODE: the glyph suffix appends in
// a witnessed sequence that is NOT the bit order and has one glyph outside the
// bracket, and the row format/column/color all switch on whether the game type
// is a team mode.
#include <cstdio>
#include <string>

#include <runtime/hud/hud_scoreboard.h>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

ScoreboardEntry player(const char *name, uint8_t slot, int16_t score,
                       uint8_t team) {
	ScoreboardEntry e;
	e.name = name;
	e.slot_id = slot;
	e.score1 = score;
	e.team = team;
	return e;
}

// Types 0/1/8 are the non-team modes; everything else columns by team.
void test_mode_split() {
	CHECK(scoreboard_is_non_team(0));
	CHECK(scoreboard_is_non_team(1));
	CHECK(scoreboard_is_non_team(8));
	CHECK(!scoreboard_is_non_team(0x10000));   // TDM
	CHECK(!scoreboard_is_non_team(0x10010));   // A&S
}

// A zero status word yields NO suffix at all — retail's parser zeroes the word
// when the detail global is clear, so this reproduces the gate.
void test_no_glyphs_when_word_is_zero() {
	CHECK(scoreboard_status_glyphs(0).empty());
}

// The append order is witnessed and is NOT the numeric bit order: R r g b y…
void test_glyph_append_order() {
	// 0x0001 R, 0x0002 b, 0x0004 g, 0x0008 r — set all four; the drawer emits
	// them R, r, g, b.
	CHECK(scoreboard_status_glyphs(0x000F) == " [Rrgb]");
	// A later-tested bit still lands after the earlier ones.
	CHECK(scoreboard_status_glyphs(0x0001 | 0x1000) == " [RA]");
	CHECK(scoreboard_status_glyphs(0x0010 | 0x0020) == " [ZC]");
}

// Bit 0x400 is the one glyph that lands AFTER the closing bracket.
void test_trailing_s_is_outside_the_bracket() {
	CHECK(scoreboard_status_glyphs(0x0400) == " []S");
	CHECK(scoreboard_status_glyphs(0x0401) == " [R]S");
}

// Non-team PLAYER rows lead with the score; team rows have no score column,
// and neither do spectator rows in EITHER mode (the spectator arm shares the
// team format). Both end with the two-digit SLOT id in brackets.
void test_row_formats() {
	const ScoreboardEntry e = player("SPAGHETTI", 7, 12, 1);
	CHECK(scoreboard_row_text(e, /*non_team=*/true) == " 12 SPAGHETTI [07]");
	CHECK(scoreboard_row_text(e, /*non_team=*/false) == "SPAGHETTI [07]");
	// The slot id is zero-padded to two digits, and a wide score keeps its
	// three-column field.
	const ScoreboardEntry big = player("A-99", 0, 100, 2);
	CHECK(scoreboard_row_text(big, true) == "100 A-99 [00]");
	// The score is the record's SIGN-EXTENDED read — a negative score prints
	// negative, not as 65534 [orig: the movsx @0x42fb9d]. The entry type is
	// signed now, so the pin passes -2 as -2.
	const ScoreboardEntry neg = player("OWN-GOAL", 4, -2, 1);
	CHECK(scoreboard_row_text(neg, true) == " -2 OWN-GOAL [04]");
	// A spectator never shows a score, even in non-team mode
	// [orig: the spectator arm @0x423e04 takes the no-score format].
	ScoreboardEntry s = player("WATCHER", 9, 33, 0);
	s.spectator = true;
	CHECK(scoreboard_row_text(s, true) == "WATCHER [09]");
	CHECK(scoreboard_row_text(s, false) == "WATCHER [09]");
	// The glyph suffix rides on the end of either format.
	ScoreboardEntry g = player("elk road", 3, 5, 1);
	g.status_flags = 0x0001;
	CHECK(scoreboard_row_text(g, false) == "elk road [03] [R]");
}

// Non-team mode alternates columns by row ordinal; team mode maps team 1 -> A
// and team 2 -> B; spectators always take their own column.
void test_columns() {
	const ScoreboardEntry a = player("x", 1, 0, 1);
	const ScoreboardEntry b = player("y", 2, 0, 2);
	CHECK(scoreboard_column_x(a, true, 0) == kColumnAX);
	CHECK(scoreboard_column_x(a, true, 1) == kColumnBX);
	CHECK(scoreboard_column_x(a, true, 2) == kColumnAX);
	CHECK(scoreboard_column_x(a, false, 5) == kColumnAX);   // team 1, ordinal ignored
	CHECK(scoreboard_column_x(b, false, 0) == kColumnBX);   // team 2
	ScoreboardEntry s = player("spec", 4, 0, 0);
	s.spectator = true;
	CHECK(scoreboard_column_x(s, true, 0) == kColumnSpectatorX);
	CHECK(scoreboard_column_x(s, false, 1) == kColumnSpectatorX);
}

// Team modes take the team palette; non-team rows and spectators take the
// active HUD color (a client setting, so it is passed in).
void test_row_colors() {
	const uint32_t hud = 0xFF00FF00u;
	const ScoreboardEntry a = player("x", 1, 0, 1);
	const ScoreboardEntry b = player("y", 2, 0, 2);
	CHECK(scoreboard_row_color(a, false, hud) == kTeamAColor);
	CHECK(scoreboard_row_color(b, false, hud) == kTeamBColor);
	CHECK(scoreboard_row_color(a, true, hud) == hud);   // non-team: HUD color
	ScoreboardEntry s = player("spec", 4, 0, 1);
	s.spectator = true;
	CHECK(scoreboard_row_color(s, false, hud) == hud);  // spectators too
}

// With more than two sides configured the board alternates pages on bit 7 of
// the HUD frame counter: teams 1/2 in the palette pair, then teams 3/4 in the
// page's own yellow/pink literals; two sides never page
// [orig: HUD_DrawKillList @0x423cd0-0x423cf1].
void test_team_page() {
	const ScoreboardTeamPage two = scoreboard_team_page(2, 0x80);
	CHECK(two.team_a == 1 && two.team_b == 2);
	CHECK(two.color_a == kTeamAColor && two.color_b == kTeamBColor);
	const ScoreboardTeamPage first = scoreboard_team_page(4, 0x7F);
	CHECK(first.team_a == 1 && first.team_b == 2);
	const ScoreboardTeamPage second = scoreboard_team_page(4, 0x80);
	CHECK(second.team_a == 3 && second.team_b == 4);
	CHECK(second.color_a == 0xFFFFFF00u && second.color_b == 0xFFFF027Fu);
	// The predicate is > 2 (three sides page too) and the counter wraps back
	// to the first page every 128 frames.
	CHECK(scoreboard_team_page(3, 0x180).team_a == 3);
	CHECK(scoreboard_team_page(3, 0x100).team_a == 1);
	// The page drives the column and the color of the rows it admits.
	const ScoreboardEntry c = player("c", 5, 0, 3);
	const ScoreboardEntry d = player("d", 6, 0, 4);
	CHECK(scoreboard_column_x(c, false, 0, second) == kColumnAX);
	CHECK(scoreboard_column_x(d, false, 0, second) == kColumnBX);
	CHECK(scoreboard_row_color(c, false, 0xFF00FF00u, second) == kTeamCColor);
	CHECK(scoreboard_row_color(d, false, 0xFF00FF00u, second) == kTeamDColor);
	// The default page keeps the two-team calls exact.
	const ScoreboardEntry a = player("a", 1, 0, 1);
	CHECK(scoreboard_column_x(a, false, 0) == kColumnAX);
	CHECK(scoreboard_row_color(a, false, 0xFF00FF00u) == kTeamAColor);
}

// The layout constants are raw retail design-space numbers; pin the handful the
// drawer derives others from so a stray edit is visible.
void test_layout_constants() {
	CHECK(kBoardX1 == 20 && kBoardY1 == 78 && kBoardX2 == 1004 && kBoardY2 == 550);
	CHECK(kHeaderX == 502 && kHeaderY == 105 && kHeaderStep == 20);
	CHECK(kListGap == 20 && kListBottom == 490 && kRowPitch == 18);
	CHECK(kColumnAX == 190 && kColumnBX == 690 && kColumnSpectatorX == 440);
	CHECK(kRankDx == -40 && kIconDx == -20 && kIconSize == 16);
	CHECK(kFooterX == 502 && kFooterY == 510);
	CHECK(kRankColor == 0xFFFFFF00u);
}

} // namespace

int main() {
	test_mode_split();
	test_no_glyphs_when_word_is_zero();
	test_glyph_append_order();
	test_trailing_s_is_outside_the_bracket();
	test_row_formats();
	test_columns();
	test_row_colors();
	test_team_page();
	test_layout_constants();
	if (failures == 0) std::printf("scoreboard_format_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
