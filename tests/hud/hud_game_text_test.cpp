// The HUD's game-text compositions (hud/hud_game_text.h) and the feed row's
// gametext resolve (hud/feed_format.h feed_row_line), pinned where they used
// to live in the GDScript HUD presenter (ADR 0040 ladder E3b): the waypoint
// name's "null" fallback, the subgoal announcement sections, the triggered
// text key, the WepDes miss, and the feed's template / camp / bonus / unknown
// lookups.
// [orig: get_waypoint_name @0x594630; HUD_DisplayTriggeredText @0x51f190;
//  HUD_FormatKillEventMessage @0x422DA0]
#include <runtime/hud/feed_format.h>
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

	// The waypoint name: the raw id keys the mission table; empty or "null"
	// (any case) falls back to the gametext default; no default reads "".
	CHECK(waypoint_display_name(1, mission, gametext) == "Alpha");
	CHECK(waypoint_display_name(2, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(3, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(4, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(9, mission, gametext) == "Waypoint");
	CHECK(waypoint_display_name(9, mission, empty).empty());

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
