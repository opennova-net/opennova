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

#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_runtime.h>
#include <runtime/world/deploy_screen_feed.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

using namespace opennova;
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

// Rows whose texts tie stand where the list sort's quicksort leaves them, not
// in their order: the Default row and two secured zones 32 indices apart
// ('D' and 'd' fold together) under its name. Three rows equal on the
// compare come out third, first, second (2 and 0 swap, then 2 and 1), and the
// second loop then puts each zone's blank after its row.
// [orig: ListWidget_SortRows @0x644990 -> Utility_SortDualArray (the call
//  @0x6449c0) -> Utility_QuickSortDualArray @0x651AB0; cmp @0x6448a0]
void test_tied_rows_stand_as_the_quicksort_leaves_them() {
	DeployListInput in = base_input();
	in.zone_name = [](const std::string &) { return std::string("Home Base"); };
	in.zones.push_back(zone(3, true));
	in.zones.push_back(zone(35, true));
	CHECK(in.zones[1].letter == 'd');
	const std::vector<DeployListRow> rows = build_deploy_rows(in);
	CHECK(rows.size() == 5);
	CHECK(rows[0].value == 36 && rows[0].text == "<c4040FF>'d' Home Base");
	CHECK(rows[1].value == -1 && rows[1].text.empty());
	CHECK(rows[2].value == 0 && rows[2].text == "<c4040FF>'D' Home Base");
	CHECK(rows[3].value == 4 && rows[3].text == "<c4040FF>'D' Home Base");
	CHECK(rows[4].value == -1 && rows[4].text.empty());
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

// The DEATH_SHROUD reveal: at once under the deploy overlay, else only past
// 240 ticks since the death stamp (a signed difference); the refresh runs on
// each tick whose low four bits are zero.
void test_shroud_reveal_and_refresh() {
	CHECK(death_shroud_revealed(true, 0));
	CHECK(!death_shroud_revealed(false, 240));
	CHECK(death_shroud_revealed(false, 241));
	CHECK(!death_shroud_revealed(false, -5));
	CHECK(deploy_refresh_due(15, 16));
	CHECK(!deploy_refresh_due(16, 16));
	CHECK(!deploy_refresh_due(16, 31));
	CHECK(deploy_refresh_due(16, 32));
	CHECK(deploy_refresh_due(-1, 0));
	CHECK(deploy_refresh_due(3, 40));
	CHECK(!deploy_refresh_due(40, 3));
}

} // namespace

// The list's items as the death screen's code-page menu holds them (D-MNU-46):
// the tags stripped, a member's roster name as its bytes ("J\xF6rg", where the
// binding's UTF-8 round trip left "J\xEF\xBF\xBDrg"), the embedder's UTF-8 zone
// name in the page's bytes, and every item the list then holds those bytes.
// [orig: UI_UpdateDeathScreenContent @0x5536a0 - the member's sprintf @0x553d8e,
//  the UIList_AddRow call @0x553da5]
void test_list_items_are_the_page_bytes() {
	DeployListInput in;
	in.team_color_tag = "<c4040FF>";
	in.default_key = "D";
	in.default_home = "Home Base";
	DeployZoneRow z = zone(0, true);
	DeployOccupant self;
	self.name = "J\xF6rg";
	self.self = true;
	DeployOccupant other;
	other.name = "\xC9mile";
	z.occupants = { self, other };
	in.zones = { z };
	in.zone_name = [](const std::string &) { return std::string("\xC3\x89" "cole"); }; // UTF-8 "Ecole"
	const std::vector<DeployListRow> rows = build_deploy_rows(in);
	const std::vector<std::string> items = deploy_list_items(rows);
	CHECK(items.size() == rows.size() && items.size() == 5);
	if (items.size() == 5) {
		// The sort puts zone A before the Default 'D' row; the members follow A.
		CHECK(items[0] == "'A' \xC9" "cole");
		CHECK(items[1] == "** J\xF6rg **");
		CHECK(items[2] == "\xC9mile");
		CHECK(items[3].empty());
		CHECK(items[4] == "'D' Home Base");
	}
}

// A zone past 'Z' (list index 63) letters its row with the byte 0x80, the
// game's "%c" of index + 0x41, beside the gametext name's bytes. The builder
// encodes the embedder's UTF-8 zone name, and the Default row's key and home,
// before it joins them, so each row is the page's bytes throughout and the
// list holds "\xC9cole" (one glyph for the E-acute), where joining the UTF-8
// name to the raw letter left a text that is not UTF-8, which the items took
// as it was: the name's two UTF-8 bytes.
// [orig: UI_UpdateDeathScreenContent @0x5536a0 - GameText_GetString("WPNames")
//  @0x553c10, the letter's add @0x553c16, sprintf("%s'%c' %s")
//  @0x553c1f..0x553c25; the Default row's sprintf("%s'%s' %s") @0x553b30..0x553b36]
void test_rows_are_the_page_bytes_past_z() {
	DeployListInput in = base_input();
	in.default_home = "Base \xC3\xA9t\xC3\xA9"; // UTF-8 "Base ete", both e acute
	in.zones = { zone(63, true) };
	in.zone_name = [](const std::string &) { return std::string("\xC3\x89" "cole"); }; // UTF-8 "Ecole"
	const std::vector<DeployListRow> rows = build_deploy_rows(in);
	// The sort: 'D' folds to 0x64, below the letter 0x80, so the Default row
	// leads; the zone's blank follows its row.
	CHECK(rows.size() == 3);
	if (rows.size() == 3) {
		CHECK(rows[0].value == 0 && rows[0].text == "<c4040FF>'D' Base \xE9t\xE9");
		CHECK(rows[1].value == 64 && rows[1].text == "<c4040FF>'\x80' \xC9" "cole");
		CHECK(rows[2].value == -1 && rows[2].text.empty());
	}
	const std::vector<std::string> items = deploy_list_items(rows);
	CHECK(items.size() == 3);
	if (items.size() == 3) {
		CHECK(items[0] == "'D' Base \xE9t\xE9");
		CHECK(items[1] == "'\x80' \xC9" "cole");
	}
}

// DFX2-41: the DEATH screen's loadout picker shows and fills for a player on a team, not for a
// team-0 player, nor for one lying dead under permanent death (alive under it, it shows).
// [orig: sub_550690 @0x550690 - the gate @0x550a3d..0x550a53 (dfx2.exe)]
void test_loadout_picker_gate() {
	CHECK(deploy_loadout_shown(1, false, true));
	CHECK(deploy_loadout_shown(2, true, false));
	CHECK(!deploy_loadout_shown(0, false, false));
	CHECK(!deploy_loadout_shown(2, true, true));
}

// DFX2-41: the SPAWNPOINTS_TABLE rows: the Default row (its team colour overwritten by
// 0xFFDDDDDD), a zone's letter, icon (the category's, the class id's over it) and its text
// column, a blank head then its lines, its health colour or the greyed pair.
// [orig: sub_551450 @0x551450 - @0x5514de..0x551568, @0x55170a, @0x551771, @0x5518a4,
//  @0x551cbf..0x551cfc (dfx2.exe)]
void test_spawn_table_rows() {
	CHECK(deploy_table_icon(0, 0) == 1);
	CHECK(deploy_table_icon(3, 0) == 4 && deploy_table_icon(7, 0) == 3 &&
			deploy_table_icon(0xC, 0) == 2);
	CHECK(deploy_table_icon(3, 0x210) == 13 && deploy_table_icon(1, 0x228) == 12);
	CHECK(deploy_table_icon(2, 0x221) == 2); // a class no case names keeps the category's
	CHECK(deploy_table_seat_line("Ace", 8, false) == "Ace  <i>(Driver)<-i>\r\n");
	CHECK(deploy_table_seat_line("Bo", 9, true) == "Bo  <i>(Driver)<-i>  <i>(Gunner)<-i>\r\n");
	CHECK(deploy_table_seat_line("Cy", 0, false) == "Cy\r\n");
	const std::string m2 = "M2";
	const std::string missed;
	CHECK(deploy_table_gunner_line("Di", &m2) == "Di  <i>(M2)<-i>\r\n");
	CHECK(deploy_table_gunner_line("Di", nullptr) == "Di  <i>(Gunner)<-i>\r\n");
	CHECK(deploy_table_gunner_line("Di", &missed) == "Di  <i>()<-i>\r\n");
	CHECK(deploy_table_waiting_line("Me", true) == "<b><cC0FFFF>** Me **<-b> <i>waiting</i>\r\n");
	CHECK(deploy_table_waiting_line("You", false) == "<cC0F0F0>You <i>waiting</i>\r\n");
	DeployTableZone heli;
	heli.index = 2;
	heli.vehicle_category = 3;
	heli.class_id = 0x219;
	heli.health_color = 0xFF20FF20u;
	heli.lines = { deploy_table_seat_line("Ace", 8, true) };
	DeployTableZone empty;
	empty.index = 0;
	empty.greyed = true;
	const std::vector<DeployTableRow> rows = build_deploy_table_rows(2, { heli, empty });
	CHECK(rows.size() == 3);
	CHECK(rows[0].cells[0] == "' '" && rows[0].cells[1] == "0" &&
			rows[0].cells[2] == "\r\nDefault");
	CHECK(rows[0].value == 0 && rows[0].cell1_value == 0 && rows[0].color == 0xFFDDDDDDu);
	CHECK(rows[1].cells[0] == "'C'" && rows[1].cells[1] == "11" && rows[1].value == 3);
	CHECK(rows[1].cell1_value == 3 && rows[1].color == 0xFF20FF20u && !rows[1].state1_color);
	CHECK(rows[1].cells[2] == "\r\nAce  <i>(Driver)<-i>\r\n");
	CHECK(rows[2].cells[0] == "'A'" && rows[2].cells[1] == "1" && rows[2].cells[2] == "\r\n");
	CHECK(rows[2].color == 0xFF404040u && rows[2].state1_color);
	CHECK(!rows[0].zone && rows[1].zone && rows[2].zone);
	// The health colour: the band rule, the fill's own colour standing for the good band
	// [orig: sub_5A19D0 @0x5a19d0 (dfx2.exe)].
	CHECK(deploy_table_health_color(100, 100, 1, 2) == 0xFFDDDDDDu);
	CHECK(deploy_table_health_color(50, 100, 1, 2) == 1u);
	CHECK(deploy_table_health_color(10, 100, 1, 2) == 2u);
	CHECK(deploy_table_health_color(-5, 100, 1, 2) == 0xFFDDDDDDu); // the unsigned first test
	CHECK(deploy_table_health_color(50, 0, 1, 2) == 0xFFDDDDDDu); // a zero ceiling taken as 1
}

// DFX2-41: the table's refill over the menu: the rows sorted by column 0 (the Default row's
// "' '" first), each with its value, column 1's value, its cells, its colour and a zone's state-1
// bit; the table and STATIC_LIST_TITLE shown; the selected row kept BY INDEX, whatever row now
// stands there, and the scroll top put back while it is under the row count. A screen without
// the table is not filled.
// [orig: sub_551450 @0x551450 - @0x55149d..0x5514d9, @0x551d28..0x551d53 (dfx2.exe)]
mnu::Document death_document() {
	mnu::Document doc;
	mnu::Screen death;
	death.name = "DEATH";
	mnu::Window root;
	root.name = "MAIN";
	root.type = mnu::WindowType::Window;
	mnu::Window table;
	table.name = "SPAWNPOINTS_TABLE";
	table.type = mnu::WindowType::Table;
	table.hidden = true;
	table.table_data.column.has_count = true;
	table.table_data.column.count = 3;
	mnu::Window title;
	title.name = "STATIC_LIST_TITLE";
	title.type = mnu::WindowType::Static;
	title.hidden = true;
	root.children.push_back(table);
	root.children.push_back(title);
	death.roots.push_back(root);
	doc.screens.push_back(death);
	return doc;
}

void test_spawn_table_fill() {
	const mnu::Document doc = death_document();
	menu::MenuRuntime rt;
	rt.open_document(&doc, "death.mnu", "");
	const int table = rt.widget_id("SPAWNPOINTS_TABLE");
	const int title = rt.widget_id("STATIC_LIST_TITLE");
	CHECK(table >= 0 && title >= 0);
	DeployTableZone c;
	c.index = 2;
	c.health_color = 0xFF123456u;
	DeployTableZone a;
	a.index = 0;
	a.greyed = true;
	const std::vector<DeployTableRow> rows = build_deploy_table_rows(1, { c, a });
	CHECK(fill_deploy_spawn_table(rt, rows));
	CHECK(rt.is_widget_shown(table) && rt.is_widget_shown(title));
	CHECK(rt.table_row_count(table) == 3);
	CHECK(rt.table_cell_text(table, 0, 0) == "' '" && rt.table_cell_text(table, 1, 0) == "'A'" &&
			rt.table_cell_text(table, 2, 0) == "'C'");
	CHECK(rt.table_row_value(table, 0) == 0 && rt.table_row_value(table, 1) == 1 &&
			rt.table_row_value(table, 2) == 3);
	CHECK(rt.table_cell_value(table, 2, 1) == 3 && rt.table_cell_text(table, 2, 1) == "1");
	uint32_t color = 0;
	CHECK(rt.table_row_colored(table, 2, &color) && color == 0xFF123456u);
	CHECK(rt.table_row_colored(table, 0, &color) && color == 0xFFDDDDDDu);
	CHECK(rt.table_row_colored(table, 1, &color) && color == 0xFF404040u);
	// The pick and the scroll ride the refill by index: the second row selected, the first
	// visible row 1.
	rt.table_set_row_selected(table, 1, true);
	rt.set_scroll_row(table, 1);
	DeployTableZone b;
	b.index = 1;
	CHECK(fill_deploy_spawn_table(rt, build_deploy_table_rows(1, { c, a, b })));
	CHECK(rt.table_row_count(table) == 4 && rt.table_cell_text(table, 2, 0) == "'B'");
	CHECK(rt.table_selected_rows(table) == std::vector<int>{ 1 } && rt.scroll_row(table) == 1);
	// A scroll top past the new count is not put back (the clear left 0).
	rt.set_scroll_row(table, 9);
	CHECK(fill_deploy_spawn_table(rt, rows) && rt.scroll_row(table) == 0);
	mnu::Document none = death_document();
	none.screens[0].roots[0].children.erase(none.screens[0].roots[0].children.begin());
	menu::MenuRuntime other;
	other.open_document(&none, "death.mnu", "");
	CHECK(!fill_deploy_spawn_table(other, rows));
}

int main() {
	test_loadout_picker_gate();
	test_spawn_table_rows();
	test_spawn_table_fill();
	test_list_items_are_the_page_bytes();
	test_rows_are_the_page_bytes_past_z();
	test_shroud_reveal_and_refresh();
	test_instruction_branches();
	test_status_text();
	test_rows_sort_and_occupants();
	test_unsecured_zone_occupants_land_after_row_zero();
	test_tied_rows_stand_as_the_quicksort_leaves_them();
	test_status_line();
	test_statics();
	test_statics_text();
	// The registry cache: built once, kept while the world sync serial stands,
	// rebuilt when it moves or when invalidated.
	{
		World w;
		SpawnZoneRegistryCache cache;
		CHECK(!cache.built);
		cache.get(w, 0);
		CHECK(cache.built);
		cache.registry.min_x = 99; // a marker a rebuild clears
		CHECK(cache.get(w, 0).min_x == 99);
		CHECK(cache.get(w, 1).min_x != 99 && cache.sync_serial == 1);
		cache.registry.min_x = 99;
		cache.invalidate();
		CHECK(cache.get(w, 1).min_x != 99);
	}
	// The rows' team tag [orig: @0x553adb..0x553af1].
	CHECK(std::string(deploy_list_team_color_tag(2)) == "<cFF2020>");
	CHECK(std::string(deploy_list_team_color_tag(1)) == "<c4040FF>");
	CHECK(std::string(deploy_list_team_color_tag(0)) == "<c4040FF>");
	// The deploy pick's wire value [orig: Input_HandleActionBinding case 12
	// @0x49b0c5..0x49b17b].
	{
		SpawnZoneRegistry reg;
		EntityHandle zone;
		zone.packed = 0x1234;
		reg.entries.push_back(zone);
		CHECK(deploy_pick_wire_value(0, reg) == kDeployPickNone);
		CHECK(deploy_pick_wire_value(65534, reg) == kDeployPickAutoTeam);
		CHECK(deploy_pick_wire_value(1, reg) == 0x1234);
		CHECK(deploy_pick_wire_value(2, reg) == kDeployPickNone);
		CHECK(deploy_pick_wire_value(-3, reg) == kDeployPickNone);
	}
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("deploy_screen_feed_test OK\n");
	return 0;
}
