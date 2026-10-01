// The Tab player list's row policy [orig: HUD_DrawKillList @0x423A30; the
// glyph chain @0x423ef1-0x4240e8; the two row formats @0x7c4bc4 / @0x7c4c00].
// The interesting content here is ORDER and MODE: the glyph suffix appends in
// a witnessed sequence that is NOT the bit order and has one glyph outside the
// bracket, and the row format/column/color all switch on whether the game type
// is a team mode.
#include <cstdio>
#include <string>
#include <vector>

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

const ScoreboardClassNames &class_names() {
	static const ScoreboardClassNames names = {
		"Medic", "Sniper", "Gunner", "Rifleman", "Engineer", "Unknown",
	};
	return names;
}

// One row through a fresh composer (no carried class stash).
std::string row_text(const ScoreboardEntry &e, bool non_team, bool su = false) {
	ScoreboardRowContext ctx;
	ctx.non_team = non_team;
	ctx.status_suffix = su;
	ScoreboardRowComposer composer(ctx, class_names());
	return composer.compose(e);
}

// The SU gate OFF draws no suffix at all; ON, a zero word still draws the
// empty pair [orig: the gate @0x423ef8 wraps the whole chain].
void test_status_suffix_gate() {
	CHECK(scoreboard_status_suffix(false, 0x000F).empty());
	CHECK(scoreboard_status_suffix(false, 0).empty());
	CHECK(scoreboard_status_suffix(true, 0) == " []");
}

// The append order is witnessed and is NOT the numeric bit order: R r g b y…
void test_glyph_append_order() {
	// 0x0001 R, 0x0002 b, 0x0004 g, 0x0008 r — set all four; the drawer emits
	// them R, r, g, b.
	CHECK(scoreboard_status_suffix(true, 0x000F) == " [Rrgb]");
	// A later-tested bit still lands after the earlier ones.
	CHECK(scoreboard_status_suffix(true, 0x0001 | 0x1000) == " [RA]");
	CHECK(scoreboard_status_suffix(true, 0x0010 | 0x0020) == " [ZC]");
}

// Bit 0x400 is the one glyph that lands AFTER the closing bracket.
void test_trailing_s_is_outside_the_bracket() {
	CHECK(scoreboard_status_suffix(true, 0x0400) == " []S");
	CHECK(scoreboard_status_suffix(true, 0x0401) == " [R]S");
}

// Non-team PLAYER rows lead with the score; team rows have no score column,
// and neither do spectator rows in EITHER mode (the spectator arm shares the
// team format). Every format carries the highlighted label run between the
// name and the two-digit SLOT id [orig: "%3i %s<ch>%s<co> [%02ld]"
// @0x7c4bc4, "%s<ch>%s<co> [%02ld]" @0x7c4c00].
void test_row_formats() {
	const ScoreboardEntry e = player("SPAGHETTI", 7, 12, 1);
	CHECK(row_text(e, /*non_team=*/true) == " 12 SPAGHETTI<ch><co> [07]");
	CHECK(row_text(e, /*non_team=*/false) == "SPAGHETTI<ch><co> [07]");
	// The slot id is zero-padded to two digits, and a wide score keeps its
	// three-column field.
	const ScoreboardEntry big = player("A-99", 0, 100, 2);
	CHECK(row_text(big, true) == "100 A-99<ch><co> [00]");
	// The score is the record's SIGN-EXTENDED read — a negative score prints
	// negative, not as 65534 [orig: the movsx @0x42fb9d].
	const ScoreboardEntry neg = player("OWN-GOAL", 4, -2, 1);
	CHECK(row_text(neg, true) == " -2 OWN-GOAL<ch><co> [04]");
	// A spectator never shows a score, even in non-team mode
	// [orig: the spectator arm @0x423e04 takes the no-score format].
	ScoreboardEntry s = player("WATCHER", 9, 33, 0);
	s.spectator = true;
	CHECK(row_text(s, true) == "WATCHER<ch><co> [09]");
	CHECK(row_text(s, false) == "WATCHER<ch><co> [09]");
	// The registry label fills the highlighted run.
	ScoreboardEntry l = player("Rock", 2, 3, 1);
	l.label = "[NL]";
	CHECK(row_text(l, false) == "Rock<ch>[NL]<co> [02]");
	// The glyph suffix rides on the end of either format, under the SU gate.
	ScoreboardEntry g = player("elk road", 3, 5, 1);
	g.status_flags = 0x0001;
	CHECK(row_text(g, false) == "elk road<ch><co> [03]");
	CHECK(row_text(g, false, /*su=*/true) == "elk road<ch><co> [03] [R]");
}

// Solo KOTH's timed rows count down from the time limit: (60T - score) / 60
// and % 60 in "%2i:%02i" [orig: @0x423e7d..0x423ec9].
void test_koth_countdown_rows() {
	ScoreboardRowContext ctx;
	ctx.non_team = true;
	ctx.timed = true;
	ctx.time_limit = 5;
	ScoreboardRowComposer composer(ctx, class_names());
	CHECK(composer.compose(player("Holder", 1, 75, 0)) == " 3:45 Holder<ch><co> [01]");
	CHECK(composer.compose(player("Fresh", 2, 0, 0)) == " 5:00 Fresh<ch><co> [02]");
	// Past the limit the truncating divide goes negative per field.
	CHECK(composer.compose(player("Over", 3, 305, 0)) == " 0:-5 Over<ch><co> [03]");
	// Spectators keep the no-score format under the timed flag.
	ScoreboardEntry s = player("Spec", 4, 0, 0);
	s.spectator = true;
	CHECK(composer.compose(s) == "Spec<ch><co> [04]");
}

// The same-team class suffix: a team-mode row whose live entity shares the
// local team appends " (class)" AFTER the status suffix; other teams don't.
// A same-team SPECTATOR stashes its class without consuming it, so the next
// drawn row carries it — the retail leak [orig: the stash @0x423dc3, the
// consume gated on !spectator @0x4240f0].
void test_class_suffix_and_leak() {
	ScoreboardRowContext ctx;
	ctx.non_team = false;
	ctx.status_suffix = true;
	ctx.local_team = 1;
	ScoreboardRowComposer composer(ctx, class_names());
	ScoreboardEntry medic = player("Doc", 1, 0, 1);
	medic.has_entity = true;
	medic.player_class = 5;
	CHECK(composer.compose(medic) == "Doc<ch><co> [01] [] (Medic)");
	ScoreboardEntry enemy = player("Foe", 2, 0, 2);
	enemy.has_entity = true;
	enemy.player_class = 6;
	CHECK(composer.compose(enemy) == "Foe<ch><co> [02] []");
	// An out-of-range class takes the unknown row.
	ScoreboardEntry odd = player("Odd", 3, 0, 1);
	odd.has_entity = true;
	odd.player_class = 2;
	CHECK(composer.compose(odd) == "Odd<ch><co> [03] [] (Unknown)");
	// The leak: a same-team spectator stashes Engineer and draws no suffix...
	ScoreboardEntry spec = player("Watch", 4, 0, 1);
	spec.spectator = true;
	spec.has_entity = true;
	spec.player_class = 9;
	CHECK(composer.compose(spec) == "Watch<ch><co> [04] []");
	// ... and the next non-spectator row — an ENEMY — carries it.
	CHECK(composer.compose(enemy) == "Foe<ch><co> [02] [] (Engineer)");
	CHECK(composer.compose(enemy) == "Foe<ch><co> [02] []");
	// Non-team boards never stash.
	ScoreboardRowContext free_ctx;
	free_ctx.non_team = true;
	free_ctx.local_team = 1;
	ScoreboardRowComposer free_composer(free_ctx, class_names());
	CHECK(free_composer.compose(medic) == "  0 Doc<ch><co> [01]");
	// No local entity never matches.
	ScoreboardRowContext orphan = ctx;
	orphan.local_team = -1;
	ScoreboardRowComposer orphan_composer(orphan, class_names());
	CHECK(orphan_composer.compose(medic) == "Doc<ch><co> [01] []");
}

// The page fold: pages = max(1, ceil(rows / ((490 - base) * 1/18f))); the
// stored page wraps below zero to the last page and past the last to the
// first; the row base scrolls one page height per page
// [orig: @0x423bb4..0x423c1c].
void test_page_fold() {
	// A base of 310 leaves 180 units = 10 rows per page.
	ScoreboardColumnCounts c;
	c.column_a = 10;
	CHECK(scoreboard_page_count(c, 310) == 1);
	c.column_a = 11;
	CHECK(scoreboard_page_count(c, 310) == 2);
	c.column_a = 0;
	CHECK(scoreboard_page_count(c, 310) == 1); // never below one page
	// Spectators and the spacer rows add to the longer column.
	c.column_a = 6;
	c.column_b = 8;
	c.spectators = 3;
	c.header_rows = 2;
	CHECK(scoreboard_page_count(c, 310) == 2); // 8 + 2 + 3 = 13 rows
	CHECK(scoreboard_page_fold(-1, 2) == 1);
	CHECK(scoreboard_page_fold(2, 2) == 0);
	CHECK(scoreboard_page_fold(1, 2) == 1);
	CHECK(scoreboard_page_fold(5, 1) == 0);
	CHECK(scoreboard_row_base(310, 0) == 310);
	CHECK(scoreboard_row_base(310, 1) == 130);
	// The key gate: session AND the board up.
	CHECK(scoreboard_takes_page_keys(true, true));
	CHECK(!scoreboard_takes_page_keys(false, true));
	CHECK(!scoreboard_takes_page_keys(true, false));
}

// The pre-pass counts teams 1|3 into A and 2|4 into B whatever the page, only
// rows with a live entity; non-team rows alternate; the spacer rows need both
// players and spectators [orig: @0x423af1..0x423ba1].
void test_column_counts() {
	std::vector<ScoreboardEntry> rows;
	for (uint8_t team : {1, 3, 2, 4, 4}) {
		ScoreboardEntry e = player("x", 0, 0, team);
		e.has_entity = true;
		rows.push_back(e);
	}
	ScoreboardEntry gone = player("gone", 0, 0, 1);
	rows.push_back(gone); // no entity: counted nowhere in team mode
	ScoreboardColumnCounts c = scoreboard_column_counts(rows, /*non_team=*/false);
	CHECK(c.column_a == 2 && c.column_b == 3 && c.spectators == 0 && c.header_rows == 0);
	ScoreboardEntry spec = player("s", 0, 0, 0);
	spec.spectator = true;
	rows.push_back(spec);
	c = scoreboard_column_counts(rows, false);
	CHECK(c.spectators == 1 && c.header_rows == 2);
	c = scoreboard_column_counts(rows, /*non_team=*/true);
	CHECK(c.column_a == 3 && c.column_b == 3 && c.spectators == 1);
	std::vector<ScoreboardEntry> only_spec = { spec };
	c = scoreboard_column_counts(only_spec, true);
	CHECK(c.header_rows == 0);
}

// The team-score block per game type [orig: @0x4232bf..0x423925].
void test_team_score_lines() {
	ScoreboardHeaderText text;
	text.team_names = { "Joint Ops Team", "Rebel Team", "Yellow Team", "Violet Team" };
	text.of_team_a = "Joint Ops Team (of";
	text.of_team_b = "Rebel Team (of";
	ScoreboardHeaderInput in;
	in.teams[1].score1 = 12;
	in.teams[2].score1 = -3;
	in.teams[3].score1 = 7;
	in.teams[4].score1 = 100;
	// TDM: "%3i %s" for teams 1/2, signed scores.
	in.game_type = 0x10000;
	in.team_count = 2;
	std::vector<std::string> lines = scoreboard_team_score_lines(in, text);
	CHECK(lines.size() == 2);
	CHECK(lines[0] == " 12 Joint Ops Team");
	CHECK(lines[1] == " -3 Rebel Team");
	// Four teams add the 3/4 pair (the test is == 4).
	in.team_count = 4;
	lines = scoreboard_team_score_lines(in, text);
	CHECK(lines.size() == 4 && lines[2] == "  7 Yellow Team" && lines[3] == "100 Violet Team");
	in.team_count = 3;
	CHECK(scoreboard_team_score_lines(in, text).size() == 2);
	// FlagBall draws the TDM form.
	in.game_type = 0x10008;
	in.team_count = 2;
	CHECK(scoreboard_team_score_lines(in, text)[0] == " 12 Joint Ops Team");
	// TKOTH: the remaining hold time per team, the hold count when nonzero.
	in.game_type = 0x10001;
	in.time_limit = 10;
	in.teams[1].score1 = 90;
	in.teams[1].koth_hold = 2;
	in.teams[2].score1 = 0;
	lines = scoreboard_team_score_lines(in, text);
	CHECK(lines.size() == 2);
	CHECK(lines[0] == " 8:30 Joint Ops Team (2)");
	CHECK(lines[1] == "10:00 Rebel Team");
	// CTF / S&D: each score against the OTHER team's ctf byte.
	in.game_type = 0x10004;
	in.teams[1].score1 = 3;
	in.teams[1].ctf_flag = 1;
	in.teams[2].score1 = 4;
	in.teams[2].ctf_flag = 5;
	lines = scoreboard_team_score_lines(in, text);
	CHECK(lines.size() == 2);
	CHECK(lines[0] == "3 Joint Ops Team (of 5)");
	CHECK(lines[1] == "4 Rebel Team (of 1)");
	in.game_type = 0x90002;
	CHECK(scoreboard_team_score_lines(in, text) == lines);
	// A&D: one line, picked by team 2's ctf byte.
	in.game_type = 0x10002;
	lines = scoreboard_team_score_lines(in, text);
	CHECK(lines.size() == 1 && lines[0] == "3 Joint Ops Team (of 5)");
	in.teams[2].ctf_flag = 0;
	lines = scoreboard_team_score_lines(in, text);
	CHECK(lines.size() == 1 && lines[0] == "4 Rebel Team (of 1)");
	// Skipped: the non-team types, the co-op family, and other team types.
	for (uint32_t g : { 0u, 1u, 8u, 0x10020u, 0x30020u, 0x10010u, 0x50010u }) {
		in.game_type = g;
		CHECK(scoreboard_team_score_lines(in, text).empty());
	}
}

// The flag carrier line: FlagBall and type 8 only; label, two spaces, name;
// the carrier's team picks the color [orig: @0x423932..0x42398e].
void test_flag_carrier_line() {
	CHECK(scoreboard_has_flag_carrier_line(0x10008));
	CHECK(scoreboard_has_flag_carrier_line(8));
	CHECK(!scoreboard_has_flag_carrier_line(0x10004));
	CHECK(scoreboard_flag_carrier_text("Flag Carrier: ", "Rock") == "Flag Carrier:   Rock");
	CHECK(scoreboard_flag_carrier_color(1, 0xFF00FF00u) == kTeamAColor);
	CHECK(scoreboard_flag_carrier_color(2, 0xFF00FF00u) == kTeamBColor);
	CHECK(scoreboard_flag_carrier_color(3, 0xFF00FF00u) == 0xFF00FF00u);
	// The label's literal fallback [orig: "!FlagCarrier:" @0x423959].
	const ScoreboardText none = scoreboard_text(GameTextLookup{});
	CHECK(none.flag_carrier_label == "!FlagCarrier:");
	CHECK(none.class_names[0].empty() && none.header.team_names[0].empty());
	CHECK(std::string(scoreboard_class_name_key(0)) == "STROVR_MEDIC");
	CHECK(std::string(scoreboard_class_name_key(5)) == "STROVR_UNKNOWN");
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
	test_status_suffix_gate();
	test_glyph_append_order();
	test_trailing_s_is_outside_the_bracket();
	test_row_formats();
	test_koth_countdown_rows();
	test_class_suffix_and_leak();
	test_page_fold();
	test_column_counts();
	test_team_score_lines();
	test_flag_carrier_line();
	test_columns();
	test_row_colors();
	test_team_page();
	test_layout_constants();
	if (failures == 0) std::printf("scoreboard_format_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
