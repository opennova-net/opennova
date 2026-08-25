// The DEATH deploy screen's content feed (world/deploy_screen_feed.h): the
// two row loops of UI_UpdateDeathScreenContent @0x5536a0 — the secured zone
// rows, the whole-list case-insensitive text sort, the occupant rows landing
// AFTER the zone row (or right after the Default row when the zone has no
// row: the witnessed insert_pos = 0 quirk), the self marker, the blank
// separator — plus the STATIC message rules (penalty wins over wave; numbered
// vs lettered; the psp/medic statics' show gates).
#include <cstdio>
#include <string>
#include <vector>

#include <world/deploy_screen_feed.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

DeployZoneRow zone(int index, bool secured) {
	DeployZoneRow z;
	z.index = index;
	z.letter = static_cast<char>('A' + index);
	char key[32];
	std::snprintf(key, sizeof(key), "STRWPNAME%03d", index + 1);
	z.name_key = key;
	z.secured = secured;
	return z;
}

DeployListInput base_input() {
	DeployListInput in;
	in.team_color_tag = "<c4040FF>";
	in.default_key = "D";
	in.default_home = "Home Base";
	in.zone_name = [](const std::string &key) { return "Zone " + key.substr(9); };
	return in;
}

void test_rows_sort_and_occupants() {
	DeployListInput in = base_input();
	// Authored order C, A, B: the list sort puts them back in letter order
	// (the shared color prefix makes the letter the sort key).
	in.zones.push_back(zone(2, true));
	in.zones.push_back(zone(0, true));
	in.zones.push_back(zone(1, true));
	std::vector<DeployListRow> rows = build_deploy_rows(in);
	CHECK(rows.size() == 4);
	CHECK(rows[0].value == 1 && rows[0].text == "<c4040FF>'A' Zone 001");
	CHECK(rows[1].value == 2);
	CHECK(rows[2].value == 3);
	// 'D' sorts after 'C': the Default row lands LAST here — the witnessed
	// whole-list sort includes it.
	CHECK(rows[3].value == 0 && rows[3].text == "<c4040FF>'D' Home Base");

	// Occupants land after their zone row, self marked, then one blank row.
	DeployOccupant ace;
	ace.handle = 0x0002;
	ace.name = "Ace";
	DeployOccupant me;
	me.handle = 0x0001;
	me.name = "Me";
	me.self = true;
	in.zones[2].occupants = {ace, me}; // zone B (index 1)
	rows = build_deploy_rows(in);
	CHECK(rows.size() == 7);
	CHECK(rows[1].value == 2);
	CHECK(rows[2].value == -1 && rows[2].text == "Ace");
	CHECK(rows[3].value == -1 && rows[3].text == "<b><cFF4040>** Me **");
	CHECK(rows[4].value == -1 && rows[4].text.empty());
	CHECK(rows[5].value == 3);
	CHECK(rows[6].value == 0);
}

// A zone with occupants but no list row (not secured: the second loop has no
// secured gate) inserts at position 0 -> right after whatever row 0 is.
void test_unsecured_zone_occupants_land_after_row_zero() {
	DeployListInput in = base_input();
	in.zones.push_back(zone(0, true));
	in.zones.push_back(zone(1, false));
	DeployOccupant bee;
	bee.name = "Bee";
	in.zones[1].occupants = {bee};
	const std::vector<DeployListRow> rows = build_deploy_rows(in);
	// Sorted: 'A' zone, then 'D' default; the quirk inserts after row 0.
	CHECK(rows.size() == 4);
	CHECK(rows[0].value == 1);
	CHECK(rows[1].value == -1 && rows[1].text == "Bee");
	CHECK(rows[2].value == -1 && rows[2].text.empty());
	CHECK(rows[3].value == 0);
}

void test_status_line() {
	DeployStatusInput in;
	CHECK(build_deploy_status(in).kind == DeployStatusLine::Kind::None);
	in.self_zone_index = 2;
	in.self_zone_countdown = 14;
	DeployStatusLine wave = build_deploy_status(in);
	CHECK(wave.kind == DeployStatusLine::Kind::Wave && wave.seconds == 14 &&
			!wave.numbered && wave.zone_index == 2);
	in.self_zone_numbered = true;
	CHECK(build_deploy_status(in).numbered);
	// The penalty timer wins over the wave arm [orig: @0x553928].
	in.penalty_seconds = 9;
	DeployStatusLine penalty = build_deploy_status(in);
	CHECK(penalty.kind == DeployStatusLine::Kind::Penalty && penalty.seconds == 9);
	// A wave zone outside the list (IndexOf < 0) shows nothing.
	in.penalty_seconds = 0;
	in.self_zone_index = -1;
	CHECK(build_deploy_status(in).kind == DeployStatusLine::Kind::None);
}

void test_statics() {
	DeployStaticsInput in;
	CHECK(!deploy_statics_visibility(in).psp_respawn);
	CHECK(!deploy_statics_visibility(in).medic);
	in.hold_seconds = 3;
	CHECK(deploy_statics_visibility(in).psp_respawn);
	in.revive_seconds = 100;
	CHECK(deploy_statics_visibility(in).medic);
	// entity+0x1E0 nonzero hides the medic pair [orig: @0x553ec5].
	in.local_mounted = true;
	CHECK(!deploy_statics_visibility(in).medic);
}

// The STATIC_RESPAWN_MSG1 text arms [orig: @0x5538e7..0x553a7b].
void test_status_text() {
	DeployStatusLine line;
	CHECK(deploy_status_text(line, "Respawn penalty", "") == "");
	line.kind = DeployStatusLine::Kind::Penalty;
	line.seconds = 7;
	CHECK(deploy_status_text(line, "Respawn penalty", "") == "Respawn penalty  <cFF4040>7");
	line.kind = DeployStatusLine::Kind::Wave;
	line.seconds = 12;
	line.numbered = true;
	line.zone_index = 4;
	CHECK(deploy_status_text(line, "", "Alpha") == "'Alpha':  <cFF4040>12");
	line.numbered = false;
	line.zone_index = 2;
	line.seconds = 30;
	CHECK(deploy_status_text(line, "", "") == "C:  <cFF4040>30");
}

} // namespace

int main() {
	test_status_text();
	test_rows_sort_and_occupants();
	test_unsecured_zone_occupants_land_after_row_zero();
	test_status_line();
	test_statics();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("deploy_screen_feed_test OK\n");
	return 0;
}
