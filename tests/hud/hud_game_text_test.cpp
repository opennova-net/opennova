// The HUD's game-text compositions (hud/hud_game_text.h) and the feed row's
// gametext resolve (hud/feed_format.h feed_row_line), pinned where they used
// to live in the GDScript HUD presenter (ADR 0040 ladder E3b): the waypoint
// name's "null" fallback, the subgoal announcement sections, the triggered
// text key, the WepDes miss, and the feed's template / camp / bonus / unknown
// lookups.
// [orig: HUD_GetWaypointName @0x594630; HUD_DisplayTriggeredText @0x51f190;
//  HUD_FormatKillEventMessage @0x422DA0]
#include <runtime/hud/feed_format.h>
#include <runtime/hud/hud_frame.h> // HudSessionText
#include <runtime/hud/hud_game_text.h>

#include <cstdio>
#include <map>
#include <string>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

// A table as "section/key" -> text.
static GameTextLookup table_of(std::map<std::string, std::string> rows) {
	return [rows](const char *section, const char *key, const char *fallback) {
		const auto it = rows.find(std::string(section) + "/" + key);
		return it != rows.end() ? it->second : std::string(fallback);
	};
}

int main() {
	const GameTextLookup mission = table_of({
			{ "WPNames/STRWPNAME001", "Alpha" },
			{ "WPNames/STRWPNAME002", "null" },
			{ "WPNames/STRWPNAME003", "NULL" },
			{ "WPNames/STRWPNAME004", "" },
			{ "WinConditions/STRWINMSG007", "Objective secured" },
			{ "LoseConditions/STRLOSEMSG007", "Objective lost" },
			{ "Triggered Text/ID012", "Proceed to the beach" },
			{ "WinConditions/STRWINDIRECTIVE003", "Take the bridge" },
			{ "LoseConditions/STRLOSEDIRECTIVE003", "Do not lose the convoy" },
			{ "WinConditions/STRWINDIRECTIVE004", "x" },
	});
	const GameTextLookup gametext = table_of({
			{ "WPNames/STRWPNAMEDEFAULT", "Waypoint" },
			{ "WepDes/M4A1", "M4A1 Carbine" },
			{ "Canned Msg/STRCND04", "$A killed $B." },
			{ "Canned Msg/STRCND48", "%s - Bonus for %s" },
			{ "Canned Msg/STRCND_FULLYCAMPED_BLUE", "Blue holds %s" },
			{ "WPNames/STRWPNAME005", "Hilltop" },
			{ "Client/STRCLI01", "Unknown" },
			{ "Misc/STRMISC_NEWOBJECTIVE", "New Objective" },
	});
	const GameTextLookup empty = table_of({});

	// The waypoint name outside a session: the RAW id keys the mission table;
	// empty or "null" (any case) falls back to the gametext default; no
	// default reads "" [orig: HUD_GetWaypointName @0x594668 / @0x59476F].
	const auto wp = [](int32_t id) {
		WaypointNameKey k;
		k.name_id = id;
		return k;
	};
	CHECK(waypoint_display_name(wp(1), false, 0x10020u, mission, gametext) == "Alpha");
	CHECK(waypoint_display_name(wp(2), false, 0x10020u, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(wp(3), false, 0x10020u, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(wp(4), false, 0x10020u, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(wp(9), false, 0x10020u, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(wp(9), false, 0x10020u, mission, empty).empty());
	// Out of a session the def specials are never consulted.
	{
		WaypointNameKey k = wp(1);
		k.has_def = true;
		k.def_attrib = 0x80000u;
		CHECK(waypoint_display_name(k, false, 0x10004u, mission, gametext) == "Alpha");
	}
	// In a session the id is remapped +1 unless the game type carries 0x20000
	// [orig: @0x594678]: retail's 00TRa "Alley Corner" is STRWPNAME002 = raw id
	// 1 + 1; the co-op shape 0x30020 keys the raw id.
	{
		const GameTextLookup trg = table_of({
				{ "WPNames/STRWPNAME001", "Front Gate" },
				{ "WPNames/STRWPNAME002", "Alley Corner" },
		});
		CHECK(waypoint_display_name(wp(1), true, 0x10020u, trg, gametext) == "Alley Corner");
		CHECK(waypoint_display_name(wp(1), true, 0x30020u, trg, gametext) == "Front Gate");
		CHECK(waypoint_display_name(wp(1), false, 0x10020u, trg, gametext) == "Front Gate");
	}
	// The in-session def specials [orig: @0x594688..0x59470D]: ARMORY beats
	// TARGET beats the type lists; a special's miss (and no special at all)
	// falls back to the mission name of the REMAPPED id.
	{
		const GameTextLookup specials = table_of({
				{ "WPNames/STRWPNAMEARMORY", "Armory" },
				{ "WPNames/STRWPNAMETARGET", "Target" },
				{ "WPNames/STRWPNAMEFLAG", "Flag" },
				{ "WPNames/STRWPNAMEFLAGBAY", "Flag Bay" },
				{ "WPNames/STRWPNAMEDEFAULT", "Waypoint" },
		});
		WaypointNameKey k = wp(0);
		k.has_def = true;
		k.def_attrib = 0x80000u | 0x8000u;
		CHECK(waypoint_display_name(k, true, 0x10004u, mission, specials) == "Armory");
		k.def_attrib = 0x8000u;
		CHECK(waypoint_display_name(k, true, 0x10004u, mission, specials) == "Target");
		k.def_attrib = 0;
		for (const int32_t t : {4091, 4093, 4095, 4096, 4097}) {
			k.def_type = t;
			CHECK(waypoint_display_name(k, true, 0x10004u, mission, specials) == "Flag");
		}
		for (const int32_t t : {4098, 4100, 4101, 4102, 4103}) {
			k.def_type = t;
			CHECK(waypoint_display_name(k, true, 0x10004u, mission, specials) == "Flag Bay");
		}
		// 4099 and 4094 are in neither list: the remapped mission id 0 + 1.
		k.def_type = 4099;
		CHECK(waypoint_display_name(k, true, 0x10004u, mission, specials) == "Alpha");
		k.def_type = 4094;
		CHECK(waypoint_display_name(k, true, 0x10004u, mission, specials) == "Alpha");
		// A special key the gametext lacks misses to the mission name.
		k.def_type = 4091;
		CHECK(waypoint_display_name(k, true, 0x10004u, mission, gametext) == "Alpha");
	}
	// The label: gametext hud/mto and the name joined "%s %s"; CTF colour runs
	// by the def type; the LFP override replaces the whole label
	// [orig: HUD_DrawWaypointNameAndDistance @0x5948D3..0x5949C0].
	{
		const GameTextLookup labels = table_of({
				{ "hud/mto", "m to" },
				{ "Overlays/LFP", "Objective" },
		});
		WaypointNameKey k;
		CHECK(waypoint_label_text("Alpha", k, 0x10020u, labels) == "m to Alpha");
		CHECK(waypoint_label_text("", k, 0x10020u, labels) == "m to ");
		// No "mto" row: GameText_GetString's "" still joins.
		CHECK(waypoint_label_text("Alpha", k, 0x10020u, empty) == " Alpha");
		// A '%' in the name is data, not a conversion.
		CHECK(waypoint_label_text("100%s", k, 0x10020u, labels) == "m to 100%s");
		k.has_def = true;
		k.def_type = 4091;
		CHECK(waypoint_label_text("Flag", k, 0x10004u, labels) == "m to <c4050FF>Flag<co>");
		k.def_type = 4098;
		CHECK(waypoint_label_text("Bay", k, 0x10004u, labels) == "m to <c4050FF>Bay<co>");
		k.def_type = 4093;
		CHECK(waypoint_label_text("Flag", k, 0x10004u, labels) == "m to <cFF3535>Flag<co>");
		k.def_type = 4100;
		CHECK(waypoint_label_text("Bay", k, 0x10004u, labels) == "m to <cFF3535>Bay<co>");
		// Outside CTF the runs never apply.
		k.def_type = 4091;
		CHECK(waypoint_label_text("Flag", k, 0x10008u, labels) == "m to Flag");
		// The LFP override: attrib 0x40000 and a nonzero zone byte; the letter
		// is (b & 0x1F) + 64, the number b >> 5.
		k.def_type = 0;
		k.def_attrib = 0x40000u;
		k.zone_number = 0x22; // 'B', 1
		CHECK(waypoint_label_text("Zone", k, 0x10020u, labels) == "m to Objective B-1");
		k.zone_number = 0;
		CHECK(waypoint_label_text("Zone", k, 0x10020u, labels) == "m to Zone");
	}

	// The session lines' strings, keyed as retail looks them up; a miss is "".
	{
		const GameTextLookup session = table_of({
				{ "Overlays/STROVER50", "Timer" },
				{ "Client/STRCLI25", "Players remaining:" },
				{ "Client/STRCLI04", "Number of players:" },
				{ "Client/STRCLI23", "Number of Spectators:" },
				{ "Overlays/STROVER53", "In the Zone" },
				{ "client/strcli19", "Green Team" },
				{ "client/strcli05", "Joint Ops Team" },
				{ "client/strcli06", "Rebel Team" },
				{ "client/strcli17", "Yellow Team" },
				{ "client/strcli18", "Violet Team" },
				{ "client/strcli20", " - Attacking" },
		});
		HudSessionText t;
		hud_session_text(session, t);
		CHECK(t.timer == "Timer" && t.players_remaining == "Players remaining:" &&
				t.players == "Number of players:" && t.spectators == "Number of Spectators:" &&
				t.in_the_zone == "In the Zone");
		CHECK(t.team_names[0] == "Green Team" && t.team_names[1] == "Joint Ops Team" &&
				t.team_names[2] == "Rebel Team" && t.team_names[3] == "Yellow Team" &&
				t.team_names[4] == "Violet Team" && t.team_names[5].empty());
		CHECK(t.attacking == " - Attacking" && t.defending.empty());
	}

	// The subgoal announcements by section; a miss posts nothing.
	CHECK(subgoal_message(false, 7, mission) == "Objective secured");
	CHECK(subgoal_message(true, 7, mission) == "Objective lost");
	CHECK(subgoal_message(false, 8, mission).empty());

	// A shown objective: the gametext header and the directive by section; a
	// directive shorter than two characters (or missing) posts nothing.
	// [orig: HUD_ShowObjectiveNotification @0x5ba2e0 — @0x5ba37b the drop]
	CHECK(objective_header(gametext) == "New Objective");
	CHECK(objective_header(empty).empty());
	CHECK(objective_directive(true, 3, mission) == "Take the bridge");
	CHECK(objective_directive(false, 3, mission) == "Do not lose the convoy");
	CHECK(objective_directive(true, 4, mission).empty());
	CHECK(objective_directive(true, 5, mission).empty());

	// Triggered text keys ID%03d; a miss shows nothing.
	CHECK(triggered_text(12, mission) == "Proceed to the beach");
	CHECK(triggered_text(13, mission).empty());

	// The WepDes name; an empty id or a miss is "".
	CHECK(weapon_display_name("M4A1", gametext) == "M4A1 Carbine");
	CHECK(weapon_display_name("AK47", gametext).empty());
	CHECK(weapon_display_name("", gametext).empty());

	// The feed row resolve: the template, the unknown-actor fallback, the
	// bonus re-compose, the camp form, and the no-template drop.
	FeedRow kill;
	kill.event_type = 4;
	kill.key = "STRCND04";
	kill.attacker = "Alice";
	kill.victim = "Bob";
	CHECK(feed_row_line(kill, gametext) == "Alice killed Bob.");
	kill.victim.clear();
	CHECK(feed_row_line(kill, gametext) == "Alice killed Unknown.");
	kill.victim = "Bob";
	kill.extra = "Carol";
	CHECK(feed_row_line(kill, gametext) == "Alice killed Bob. - Bonus for Carol");
	FeedRow camp;
	camp.camp = true;
	camp.key = "STRCND_FULLYCAMPED_BLUE";
	camp.wpname_key = "STRWPNAME005";
	CHECK(feed_row_line(camp, gametext) == "Blue holds Hilltop");
	FeedRow unknown_key;
	unknown_key.key = "STRCND99";
	unknown_key.attacker = "Alice";
	CHECK(feed_row_line(unknown_key, gametext).empty());

	// The HUD sprintf: the template IS the CRT format and the call site's own
	// argument list feeds it. Integer conversions take the whole CRT spec
	// (flags, width, precision) with the size prefix reduced to retail's
	// 32-bit int; `h` narrows first. [orig: sprintf @0x76A9E4 call sites
	// @0x59E530 / @0x59E8F6 / @0x59E9BD / @0x5A8972 (one int)]
	CHECK(hud_sprintf("Distance: %ldm", 127) == "Distance: 127m");
	CHECK(hud_sprintf("%li / %lu / %I32d", -3) == "-3 / %lu / %I32d");
	CHECK(hud_sprintf("%lu", -1) == "4294967295");
	CHECK(hud_sprintf("%04d|%-4d|%+d|% d|%.3d", 7) == "0007|%-4d|%+d|% d|%.3d");
	CHECK(hud_sprintf("%hd %hu", 65535) == "-1 %hu");
	CHECK(hud_sprintf("%#x %X %o %c", 255) == "0xff %X %o %c");
	CHECK(hud_sprintf("%c", 'A') == "A");
	CHECK(hud_sprintf("100%% %d%%", 5) == "100% 5%");
	// Forms the call never passed stay literal; a mismatched kind uses up its
	// slot; a printed NUL ends the string.
	CHECK(hud_sprintf("%n %p %f %S %*d %lld", 1) == "%n %p %f %S %*d %lld");
	CHECK(hud_sprintf("%s m", 12) == "%s m");
	CHECK(hud_sprintf("x%cy", 0) == "x");
	CHECK(hud_sprintf("50%") == "50%");
	// No argument: %% collapses and every conversion stays literal
	// [orig: @0x59E4D5 STROVER_DIST1KM, @0x59E938 auto/none, @0x5BE0E9].
	CHECK(hud_sprintf("> 1km %d%%") == "> 1km %d%");
	// The mixed end-round lists and a string argument.
	HudTextArg name;
	name.text = "Blue";
	HudTextArg twelve;
	twelve.number = 12;
	twelve.is_number = true;
	CHECK(hud_sprintf("%s : %ld", {name, twelve}) == "Blue : 12");
	CHECK(hud_sprintf("%-6s|%.2s|%hs|%ls", {name, name, name, name}) == "Blue  |Bl|Blue|%ls");
	CHECK(hud_sprintf("%d %s", {name, twelve}) == "%d %s");

	// The service line [orig: HUD_DrawGameplayOverlays @0x5BDE60]: the armory
	// and FARP templates miss to "" (GameText_GetString @0x51EC08), the bay
	// keeps its compiled-in fallback; the key name, the seconds, or nothing
	// formats each.
	const GameTextLookup overlays = table_of({
			{ "Overlays/STROVER_ARMORY_INFO", "Press '%s' for gear" },
			{ "Overlays/STROVER_FARP_WAIT", "Rearm in %d" },
			{ "Overlays/STROVER_FARP_RELOADING", "Rearming 100%%" },
	});
	CHECK(service_prompt_text(1, "E", 0, overlays) == "Press 'E' for gear");
	CHECK(service_prompt_text(2, "E", 0, overlays) == "!Press 'E' to activate vehicle bay menu");
	CHECK(service_prompt_text(3, "E", 7, overlays) == "Rearm in 7");
	CHECK(service_prompt_text(4, "E", 7, overlays) == "Rearming 100%");
	CHECK(service_prompt_text(1, "E", 0, empty).empty());
	CHECK(service_prompt_text(3, "E", 7, empty).empty());
	CHECK(service_prompt_text(4, "E", 7, empty).empty());
	CHECK(service_prompt_text(0, "E", 7, overlays).empty());

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_game_text_test OK\n");
	return 0;
}
