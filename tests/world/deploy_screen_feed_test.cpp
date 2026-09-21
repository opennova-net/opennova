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

#include <runtime/world/deploy_screen_feed.h>

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
	// Every team zone appends one blank spacer after its row even with nobody
	// queued [orig: the unconditional UIList_AddRow @0x553dbf], so the sorted
	// list reads A, blank, B, blank, C, blank, D.
	CHECK(rows.size() == 7);
	CHECK(rows[0].value == 1 && rows[0].text == "<c4040FF>'A' Zone 001");
	CHECK(rows[1].value == -1 && rows[1].text.empty());
	CHECK(rows[2].value == 2);
	CHECK(rows[3].value == -1 && rows[3].text.empty());
	CHECK(rows[4].value == 3);
	CHECK(rows[5].value == -1 && rows[5].text.empty());
	// 'D' sorts after 'C': the Default row lands LAST here — the witnessed
	// whole-list sort includes it.
	CHECK(rows[6].value == 0 && rows[6].text == "<c4040FF>'D' Home Base");

	// Occupants land after their zone row, self marked, then the blank row.
	DeployOccupant ace;
	ace.handle = 0x0002;
	ace.name = "Ace";
	DeployOccupant me;
	me.handle = 0x0001;
	me.name = "Me";
	me.self = true;
	in.zones[2].occupants = {ace, me}; // zone B (index 1)
	rows = build_deploy_rows(in);
	CHECK(rows.size() == 9);
	CHECK(rows[0].value == 1);
	CHECK(rows[1].value == -1 && rows[1].text.empty());
	CHECK(rows[2].value == 2);
	CHECK(rows[3].value == -1 && rows[3].text == "Ace");
	CHECK(rows[4].value == -1 && rows[4].text == "<b><cFF4040>** Me **");
	CHECK(rows[5].value == -1 && rows[5].text.empty());
	CHECK(rows[6].value == 3);
	CHECK(rows[7].value == -1 && rows[7].text.empty());
	CHECK(rows[8].value == 0);
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
	// Sorted: 'A' zone, then 'D' default. Zone A (listed, nobody queued) still
	// appends its blank after row 0; the unlisted zone B's quirk then inserts
	// Bee after row 0 and its own blank after Bee.
	CHECK(rows.size() == 5);
	CHECK(rows[0].value == 1);
	CHECK(rows[1].value == -1 && rows[1].text == "Bee");
	CHECK(rows[2].value == -1 && rows[2].text.empty());
	CHECK(rows[3].value == -1 && rows[3].text.empty());
	CHECK(rows[4].value == 0);
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
	// entity+0x1E0 nonzero (a medic already reviving) hides the medic pair
	// [orig: @0x553ec5].
	in.local_medic_reviving = true;
	CHECK(!deploy_statics_visibility(in).medic);
}

// The three statics' texts [orig: @0x553e10..0x553f60]: the Overlays labels
// with the retail fallbacks, the key slot of STROVER_CALLMEDIC.
void test_statics_text() {
	DeployStaticsInput in;
	in.hold_seconds = 8;
	in.revive_seconds = 42;
	const opennova::hud::GameTextLookup none;
	DeployStaticsText t = deploy_statics_text(in, "M", none);
	CHECK(t.psp_respawn == "Spawn point available in  <cFF4040>8");
	CHECK(t.medic_timer == "Medic time remaining  <cFF4040>42");
	CHECK(t.call_medic == "Press M to call a medic");
	const opennova::hud::GameTextLookup table = [](const char *section, const char *key,
														const char *fallback) {
		if (std::string(section) != "Overlays") return std::string(fallback);
		const std::string k = key;
		if (k == "STROVER_PSPRESPAWN") return std::string("Spawnpunkt in");
		if (k == "STROVER_MEDICTIMER") return std::string("Sanitaeter");
		if (k == "STROVER_CALLMEDIC") return std::string("Sanitaeter rufen");
		return std::string(fallback);
	};
	t = deploy_statics_text(in, "M", table);
	CHECK(t.psp_respawn == "Spawnpunkt in  <cFF4040>8");
	CHECK(t.medic_timer == "Sanitaeter  <cFF4040>42");
	// A format without the key slot draws as authored.
	CHECK(t.call_medic == "Sanitaeter rufen");
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


void test_instruction_branches() {
    const opennova::hud::GameTextLookup keys = [](const char *, const char *key, const char *fallback) {
        return *fallback ? std::string(fallback) : std::string(key);
    };
    DeployInstructionsInput in;
    in.player_name = "Ace";
    in.clan = "TAG";
    auto v = build_deploy_instructions(in, keys);
    CHECK(v.show_first && v.show_second && v.replace_first);
    CHECK(v.first_text == "Welcome to the game, Ace<ch>TAG<co>!");
    CHECK(v.second_text == "STROVER_SPECTATORSPAWN");
    in.team = 1;
    in.game_type = 0x10000;
    v = build_deploy_instructions(in, keys);
    CHECK(v.first_text == "Ace<ch>TAG<co>, you have joined the !Joint Ops Team.");
    CHECK(v.second_text == "STROVER_RESPAWN3");
    in.team = 2;
    CHECK(build_deploy_instructions(in, keys).first_text ==
            "Ace<ch>TAG<co>, you have joined the !Rebel Team.");
    in.team = 3;
    CHECK(build_deploy_instructions(in, keys).first_text ==
            "Ace<ch>TAG<co>, you have joined the Unknown.");
    in.dead = true;
    in.kill_announcement = "Ace was killed.";
    v = build_deploy_instructions(in, keys);
    CHECK(v.first_text == in.kill_announcement);
    in.has_spawn_zones = true;
    in.has_full_team_spawn = true;
    v = build_deploy_instructions(in, keys);
    CHECK(v.show_second && v.replace_first); // full entry alone is insufficient
    CHECK(v.second_text == "STROVER_RESPAWN1");
    in.check_secured_spawn = true;
    v = build_deploy_instructions(in, keys);
    CHECK(v.show_first && !v.show_second && !v.replace_first);
    CHECK(v.second_text == "STROVER_RESPAWN1"); // RESPawn2 is overwritten
    in.game_type = 0x10020;
    v = build_deploy_instructions(in, keys);
    CHECK(!v.show_first && !v.show_second && !v.replace_first);
    in.has_full_team_spawn = false;
    in.has_spawn_zones = false;
    v = build_deploy_instructions(in, keys);
    CHECK(!v.show_first && v.show_second && v.first_text == in.kill_announcement);
    CHECK(v.second_text == "STROVER_RESPAWN5");
    in.dead = false;
    CHECK(build_deploy_instructions(in, keys).second_text == "STROVER_RESPAWN4");
    in.dead = true;
    in.game_type = 0x30020;
    CHECK(build_deploy_instructions(in, keys).second_text == "STROVER_RESPAWN4");
    in.team = 0;
    CHECK(build_deploy_instructions(in, keys).second_text == "STROVER_SPECTATORSPAWN");

    in.permanent_death = true;
    in.round_ticks = (3600 + 2 * 60 + 3) * 62 + 61;
    in.alive_players = 7;
    v = build_deploy_instructions(in, keys);
    CHECK(v.permanent_death && v.show_first && v.show_second && v.replace_first);
    CHECK(v.first_text == "STROVER_PERMANENTDEATH");
    CHECK(v.second_text == "STROVER_NORESPAWN");
    CHECK(v.show_round_status && v.round_text == "STROVER50 <cFF4040>1:02:03");
    CHECK(v.remaining_players_text == "STRCLI25 <cFF4040>7");
    in.spectators_allowed = true;
    in.round_ticks = -1;
    v = build_deploy_instructions(in, keys);
    CHECK(v.second_text == "STROVER_SPECTATORSPAWN" && !v.show_round_status);
    in.round_ticks = 0;
    CHECK(build_deploy_instructions(in, keys).round_text == "STROVER50 <cFF4040>0:00:00");
    in.dead = false;
    CHECK(!build_deploy_instructions(in, keys).permanent_death);
    in.player_name = std::string(254, 'X');
    in.clan = "long";
    in.game_type = 0;
    CHECK(build_deploy_instructions(in, keys).first_text == "Welcome to the game, " +
            std::string(254, 'X') + "<!");
}

} // namespace

int main() {
	test_instruction_branches();
	test_status_text();
	test_rows_sort_and_occupants();
	test_unsecured_zone_occupants_land_after_row_zero();
	test_status_line();
	test_statics();
	test_statics_text();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("deploy_screen_feed_test OK\n");
	return 0;
}
