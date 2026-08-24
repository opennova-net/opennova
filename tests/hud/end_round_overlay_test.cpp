// The end-of-round overlay ladder (hud/end_round_overlay.h): the headline /
// second-line key selection per game type x draw x winner, the y ladder, the
// score lines, the game-time quirk, and the stat-field column arithmetic of
// draw_endround_stats_overlay @0x5b7cd0 / Overlay_ComputeStatFieldColumnLayout
// @0x5b7a10.
#include <cstdio>
#include <string>
#include <vector>

#include <hud/end_round_overlay.h>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

void test_team_mode_ladder() {
	EndRoundOverlayInput in;
	in.game_type = 0x10000; // TDM
	in.winner_team = 2;
	in.team_scores[0] = 12;
	in.team_scores[1] = 30;
	in.round_time_remaining_ticks = 62 * 65; // 1:05
	const std::vector<EndRoundLine> lines = end_round_overlay_lines(in);
	CHECK(lines.size() == 5);
	CHECK(lines[0].key == "STROVER33" && lines[0].y == 300);
	// The TDM second line: winner 2 with time remaining -> STROVER106 (REDTEAM).
	CHECK(lines[1].key == "STROVER106" && lines[1].y == 350);
	CHECK(lines[1].args.size() == 1 && lines[1].args[0].key == "STROVER_REDTEAM");
	// Score lines from 382 + 32 = 414, step 40.
	CHECK(lines[2].literal == "%s : %ld" && lines[2].y == 414);
	CHECK(lines[2].args[0].key == "STROVER_BLUETEAM" && lines[2].args[0].fallback == "!Joint Ops Team");
	CHECK(lines[2].args[1].is_number && lines[2].args[1].number == 12);
	CHECK(lines[3].y == 454 && lines[3].args[1].number == 30);
	// +40 then +24 -> 518: the game time with the un-modded minutes quirk.
	CHECK(lines[4].y == 518 && lines[4].literal == "%s : %d:%02d:%02d");
	CHECK(lines[4].args.size() == 4 && lines[4].args[0].key == "STROVER_GAMETIME");
	CHECK(lines[4].args[1].number == 0 && lines[4].args[2].number == 1 &&
			lines[4].args[3].number == 5);
	// No time remaining swaps the second-line key; a draw beats the winner.
	in.round_time_remaining_ticks = 0;
	CHECK(end_round_overlay_lines(in)[1].key == "STROVER107");
	in.draw = true;
	const std::vector<EndRoundLine> drawn = end_round_overlay_lines(in);
	CHECK(drawn[0].key == "STROVER34" && drawn[1].key == "STROVER108");
	// The AAS family and the S&D family.
	in.draw = false;
	in.game_type = 0x50010;
	in.round_time_remaining_ticks = 1;
	CHECK(end_round_overlay_lines(in)[1].key == "STROVER100");
	in.game_type = 0x90002;
	CHECK(end_round_overlay_lines(in)[1].key == "STROVER112");
	in.winner_team = 3; // no team name -> no second line; the headline is STROVER61
	const std::vector<EndRoundLine> three = end_round_overlay_lines(in);
	CHECK(three[0].key == "STROVER61");
	CHECK(three[1].literal == "%s : %ld" && three[1].y == 382); // 350 + 32
}

void test_objective_and_non_team() {
	EndRoundOverlayInput in;
	in.game_type = 0x30020; // co-op: the objective family, no score lines
	in.winner_team = 1;
	in.local_team = 1;
	std::vector<EndRoundLine> lines = end_round_overlay_lines(in);
	CHECK(lines.size() == 3);
	// 0x30020 carries the team bit: the headline is the team-mode winner key.
	CHECK(lines[0].key == "STROVER32" && lines[0].fallback == "!Mission Completed");
	CHECK(lines[1].key == "STROVER1" && lines[1].y == 350);
	CHECK(lines[2].literal == "%s : %d:%02d:%02d" && lines[2].y == 414);
	in.local_team = 2;
	CHECK(end_round_overlay_lines(in)[1].key == "STROVER2");
	in.death_screen = true;
	lines = end_round_overlay_lines(in);
	CHECK(lines.size() == 2 && lines[1].y == 382); // no second line -> 350 + 32
	// DM with named players: the PLAYERWIN headline, the tie, the name rows.
	EndRoundOverlayInput dm;
	dm.game_type = 0;
	dm.player_names[0] = "Ace";
	dm.player_scores[0] = 9;
	dm.player_names[1] = "Bee";
	dm.player_scores[1] = 4;
	dm.round_time_remaining_ticks = 62;
	lines = end_round_overlay_lines(dm);
	CHECK(lines[0].key == "STROVER_PLAYERWIN" && lines[0].args[0].literal == "Ace");
	CHECK(lines[1].key == "STROVER106" && lines[1].args[0].literal == "Ace");
	CHECK(lines[2].args[0].literal == "Ace" && lines[2].args[1].number == 9 && lines[2].y == 414);
	CHECK(lines[3].args[0].literal == "Bee" && lines[3].y == 454);
	CHECK(lines[4].y == 518);
	dm.player_scores[1] = 9; // the two-name tie
	lines = end_round_overlay_lines(dm);
	CHECK(lines[0].key == "STROVER35" && lines[1].key == "STROVER116" &&
			lines[1].args.size() == 2);
}

void test_column_layout() {
	const std::vector<std::pair<uint8_t, uint8_t>> fields = {{1, 1}, {5, 0}, {30, 1}};
	const auto measure = [](const std::string &s) { return static_cast<int>(s.size()) * 10; };
	const auto resolve = [](const std::string &key, const std::string &fb) {
		return key == "STROVER_STATFIELD01" ? std::string("Kills") : fb;
	};
	EndRoundColumnLayout lay = end_round_column_layout(40, 984, fields, false,
			{"Ace", "Longername"}, measure, resolve);
	CHECK(lay.columns.size() == 2);
	CHECK(lay.columns[0].label_key == "STROVER_STATFIELD01");
	CHECK(lay.columns[1].label_key == "STROVER_STATFIELD30" && lay.columns[1].field_index == 2);
	// name: max(100, 100) + 10 = 110; col0: max(40, 50) + 10 = 60; col1:
	// "!STROVER_STATFIELD30" = 20 chars -> 200 + 10 = 210; remaining =
	// 944 - 270 - 110 = 564; left += 141; extra = 282 / 3 = 94.
	CHECK(lay.left == 40 + 141);
	CHECK(lay.name_width == 110 + 94);
	CHECK(lay.columns[0].width == 60 + 94 && lay.columns[1].width == 210 + 94);
	CHECK(lay.columns[0].x == lay.left + lay.name_width);
	CHECK(lay.columns[0].center_x == lay.columns[0].x + lay.columns[0].width / 2);
	CHECK(lay.columns[1].x == lay.columns[0].x + lay.columns[0].width);
	CHECK(lay.right == lay.columns[1].x + lay.columns[1].width);
	// The show-disabled toggle lists every field with the SMALL keys.
	lay = end_round_column_layout(40, 984, fields, true, {}, measure, resolve);
	CHECK(lay.columns.size() == 3 && lay.columns[1].label_key == "STROVER_STATFIELDSMALL05");
	// The field -> string map.
	CHECK(stat_field_string_index(30) == 33 && stat_field_string_index(31) == 27 &&
			stat_field_string_index(32) == 34 && stat_field_string_index(7) == 7 &&
			stat_field_string_index(40) == 0);
}

} // namespace

int main() {
	test_team_mode_ladder();
	test_objective_and_non_team();
	test_column_layout();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("end_round_overlay_test OK\n");
	return 0;
}
