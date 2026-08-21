// The AAS objective ("LFP") panel: zone state, frame selection, blink phase
// and marker stepping.
// [orig: HUD_DrawZoneStatusPanel @0x5A2480; HUD_DrawZoneMarker @0x5986F0;
//  the state inputs @0x598825..0x598934]

#include <hud/hud_lfp_panel.h>

#include <cstdio>
#include <string>

using namespace opennova::hud;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// READY is someone ELSE's point at zero control; UNDER ATTACK is YOUR point
// with a draining rate. The team test is what separates them, and swapping it
// would show "ready for takeover" on a point you are losing.
void test_zone_states() {
	// Enemy point, control exhausted -> ready for takeover.
	CHECK(lfp_zone_ready(2, 1, 0), "an enemy point at zero control is ready");
	// Your own point at zero control is NOT "ready" — you already hold it.
	CHECK(!lfp_zone_ready(1, 1, 0), "your own point is never 'ready'");
	// Enemy point still holding control is not ready yet.
	CHECK(!lfp_zone_ready(2, 1, 5), "control remaining means not ready");

	// Your point, control draining -> under attack.
	CHECK(lfp_zone_under_attack(1, 1, -3), "your point draining is under attack");
	// A zero or positive rate is NOT an attack — the sign is the whole test.
	CHECK(!lfp_zone_under_attack(1, 1, 0), "a still rate is not an attack");
	CHECK(!lfp_zone_under_attack(1, 1, 4), "a rising rate is not an attack");
	// An enemy point draining is not YOUR point under attack.
	CHECK(!lfp_zone_under_attack(2, 1, -3), "an enemy point draining is not yours");
}

// The mask is NOT a 50/50 blink: phase A is 128 counts of every 512, phase B
// the other 384. A half-and-half blink would be visibly wrong.
void test_blink_duty_cycle() {
	int a = 0, b = 0;
	for (int ms = 0; ms < 512; ++ms) {
		if (lfp_blink_phase_a(ms)) ++a; else ++b;
	}
	CHECK(a == 128, "phase A is 128 of every 512");
	CHECK(b == 384, "phase B is the other 384");
	CHECK(b == a * 3, "so B runs three times as long as A");
}

// Frame 2 is UNDER-ATTACK and frame 3 is READY, not the reverse.
void test_frame_numbering() {
	CHECK(static_cast<int>(LfpFrame::UnderAttack) == 2, "under-attack is frame 2");
	CHECK(static_cast<int>(LfpFrame::Ready) == 3, "ready is frame 3");
	CHECK(static_cast<int>(LfpFrame::Default) == 0, "default is frame 0");
	CHECK(static_cast<int>(LfpFrame::InZone) == 1, "in-zone is frame 1");
}

void test_frame_selection() {
	// Nothing special, outside the cylinder -> default.
	CHECK(lfp_frame(0, 1, 5, 0, false, true) == LfpFrame::Default,
			"a quiet neutral point draws the default frame");
	// Inside the cylinder wins over default.
	CHECK(lfp_frame(0, 1, 5, 0, true, true) == LfpFrame::InZone,
			"standing in the cylinder shows the in-zone frame");

	// Your point draining, on phase A -> under attack.
	CHECK(lfp_frame(1, 1, 5, -2, false, true) == LfpFrame::UnderAttack,
			"an attacked point blinks its frame on phase A");
	// ...and off phase A it falls back rather than sticking.
	CHECK(lfp_frame(1, 1, 5, -2, false, false) != LfpFrame::UnderAttack,
			"the attack frame is a BLINK, not a steady state");

	// An enemy point at zero control shows ready on phase B.
	CHECK(lfp_frame(2, 1, 0, 0, false, false) == LfpFrame::Ready,
			"a takeable point blinks ready on phase B");
	CHECK(lfp_frame(2, 1, 0, 0, false, true) != LfpFrame::Ready,
			"and not on phase A — the two states alternate");
}

// Markers step ACROSS by 98 within a group, groups step DOWN by 86, and a
// group is RIGHT-ANCHORED to the panel X: its first marker sits
// 98 * zonesInGroup left of the anchor and the last ends one pitch short of it.
void test_stepping() {
	int x = 0, y = 0;
	// A three-zone group: k = 0 at panelX - 294.
	lfp_marker_origin(1020, 250, 3, 0, 0, x, y);
	CHECK(x == 1020 - 3 * 0x62, "the first marker sits 98 * count left of the anchor");
	CHECK(y == 250 + kLfpRowDy, "on the first group's row");

	lfp_marker_origin(1020, 250, 3, 2, 0, x, y);
	CHECK(x == 1020 - 0x62, "the last marker ends one pitch short of the anchor");
	CHECK(y == 250 + kLfpRowDy, "and stays on its group's row");

	// A one-zone group three groups down: no horizontal drift.
	lfp_marker_origin(1020, 250, 1, 0, 3, x, y);
	CHECK(y == 250 + kLfpRowDy + 3 * 0x56, "groups step DOWN by 86");
	CHECK(x == 1020 - 0x62, "a single-zone group sits one pitch left of the anchor");

	// Right-anchoring: a bigger group grows LEFTWARD, the right edge stays put.
	int x1 = 0, y1 = 0, x4 = 0, y4 = 0;
	lfp_marker_origin(1020, 250, 1, 0, 0, x1, y1);
	lfp_marker_origin(1020, 250, 4, 3, 0, x4, y4);
	CHECK(x1 == x4, "the last marker of any group ends at the same x");

	// The status text sits 4 px left of the group's first marker, on the row.
	lfp_status_text_origin(1020, 250, 3, 1, x, y);
	CHECK(x == 1020 - 3 * 0x62 - 4, "status text x is the group start minus 4");
	CHECK(y == 250 + 0x56 + 12, "status text y is the group row plus 12");

	CHECK(kLfpStepX != kLfpStepY, "the two pitches are NOT the same value");
}

// Neutral is GREEN — an un-owned objective is not "no colour". The three
// values are the g_hudColorTable immediates HUD_InitTeamColorTable writes.
void test_colors() {
	CHECK(kLfpColorTeam1 == 0xFF80A0FFu, "team 1 = table[3] @0x51f26d");
	CHECK(kLfpColorTeam2 == 0xFFFF5050u, "team 2 = table[5] @0x51f259");
	CHECK(kLfpColorNeutral == 0xFF00FF00u, "neutral = table[1] @0x51f263");
	CHECK(lfp_team_color(0) == kLfpColorNeutral, "neutral takes the neutral colour");
	CHECK(lfp_team_color(1) == kLfpColorTeam1, "team 1 takes its colour");
	CHECK(lfp_team_color(2) == kLfpColorTeam2, "team 2 takes its colour");
	CHECK(lfp_team_color(7) == kLfpColorNeutral, "an unknown team falls to neutral");
	// The icon modulate is a flat brightness, NOT a team tint.
	CHECK(kLfpIconModulate == 0xFF7F7F7Fu, "the icon modulate is grey");
}

// The letter tile is CENTRE-anchored while the distance is left-aligned —
// mixing those up shifts the letter by half a tile.
void test_offsets() {
	CHECK(kLfpTileOffX == 0x22 && kLfpTileOffY == 0x34, "tile centre offset");
	CHECK(kLfpTileSize == 0x24, "the tile is 36x36");
	CHECK(kLfpIconSize == 0x66, "the icon quad is 102x102");
	CHECK(kLfpCountOwnOffY == kLfpCountEnemyOffY,
			"both contest counts share a baseline");
	CHECK(kLfpCountEnemyOffX > kLfpCountOwnOffX,
			"the enemy count sits right of the own count");
}

// The row cursor starts at the anchor + 12, and the MARKERS share that row
// with the status text [orig: x_position = x_base + 12 @0x5a25bd, pushed as
// the marker's y @0x5a2799 and the text's y @0x5a2652].
void test_row_offset() {
	int mx = 0, my = 0, tx = 0, ty = 0;
	lfp_marker_origin(1020, 27, 2, 0, 0, mx, my);
	lfp_status_text_origin(1020, 27, 2, 0, tx, ty);
	CHECK(my == 27 + 12, "the first marker row is the anchor plus 12");
	CHECK(ty == my, "the status text sits on the marker row");
	CHECK(tx == mx - 4, "and 4 px left of the group's first marker");
}

// In the cylinder: 2D distance within the radius AND |dz| within half of it
// [orig: @0x5987f4..0x598810].
void test_in_cylinder() {
	CHECK(lfp_in_cylinder(50 << 16, 0, 70), "inside the radius, level");
	CHECK(!lfp_in_cylinder(71 << 16, 0, 70), "outside the radius");
	CHECK(lfp_in_cylinder(50 << 16, 35 << 16, 70), "|dz| at half the radius still counts");
	CHECK(!lfp_in_cylinder(50 << 16, 36 << 16, 70), "|dz| past half the radius does not");
	CHECK(!lfp_in_cylinder(50 << 16, -(36 << 16), 70), "the height test is absolute");
}

// The point colour ladder [orig: @0x5988b7..0x59890d].
void test_point_color_ladder() {
	const uint32_t c = kLfpColorTeam1; // FF80A0FF
	// Quiet and outside the cylinder -> dimmed: (c>>1 & 7F7F7F) + (c>>3 & 1F1F1F).
	const uint32_t dim = ((c >> 1) & 0x7F7F7Fu) + ((c >> 3) & 0x1F1F1Fu) + 0xFF000000u;
	CHECK(lfp_point_color(c, false, false, false, false, true) == dim,
			"a quiet, distant point is dimmed");
	CHECK(lfp_point_color(c, true, false, false, false, true) == c,
			"standing in the cylinder keeps the full colour");
	CHECK(lfp_point_color(c, false, true, false, false, true) == c,
			"a contested point (unequal counts) keeps the full colour");
	CHECK(lfp_point_color(c, true, false, true, false, true) == 0xFFFFFF00u,
			"under attack on phase A is pure yellow");
	CHECK(lfp_point_color(c, true, false, true, false, false) == c,
			"and the team colour again on phase B");
	CHECK(lfp_point_color(c, true, false, false, true, true) ==
					(((c >> 1) & 0x7F7F7Fu) | 0xFF000000u),
			"ready on phase A halves the colour");
}

// The counts swap sides for an enemy zone; the label format switches to
// kilometres past 1000 m [orig: @0x598ff5..0x59902b; @0x599114..0x59914b].
void test_counts_and_distance() {
	int own = 0, other = 0;
	lfp_contest_counts(1, 1, 5, 2, own, other);
	CHECK(own == 5 && other == 2, "on your own zone +0x220 is the own count");
	lfp_contest_counts(2, 1, 5, 2, own, other);
	CHECK(own == 2 && other == 5, "on an enemy zone the sides swap");
	char buf[32];
	lfp_format_distance(buf, sizeof(buf), 760);
	CHECK(std::string(buf) == "760m", "metres up to 1000");
	lfp_format_distance(buf, sizeof(buf), 1000);
	CHECK(std::string(buf) == "1000m", "1000 is still metres (the test is <=)");
	lfp_format_distance(buf, sizeof(buf), 1234);
	CHECK(std::string(buf) == "1.23k", "past 1000 it reads kilometres to two places");
	CHECK(lfp_bar_fraction_q16(30, 60, true) == 0x8000, "the bar is value / limit");
	CHECK(lfp_bar_fraction_q16(30, 60, false) == 0, "an inactive entry draws an empty bar");
	CHECK(lfp_opposing_team_color(1) == kLfpColorTeam2 &&
					lfp_opposing_team_color(2) == kLfpColorTeam1,
			"the other-side count takes the opposing colour");
}

} // namespace

int main() {
	test_zone_states();
	test_blink_duty_cycle();
	test_frame_numbering();
	test_frame_selection();
	test_stepping();
	test_colors();
	test_offsets();
	test_row_offset();
	test_in_cylinder();
	test_point_color_ladder();
	test_counts_and_distance();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_lfp_panel_test OK\n");
	return 0;
}
