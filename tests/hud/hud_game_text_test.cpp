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
	});
	const GameTextLookup gametext = table_of({
			{ "WPNames/STRWPNAMEDEFAULT", "Waypoint" },
			{ "WepDes/M4A1", "M4A1 Carbine" },
			{ "Canned Msg/STRCND04", "$A killed $B." },
			{ "Canned Msg/STRCND48", "%s - Bonus for %s" },
			{ "Canned Msg/STRCND_FULLYCAMPED_BLUE", "Blue holds %s" },
			{ "WPNames/STRWPNAME005", "Hilltop" },
			{ "Client/STRCLI01", "Unknown" },
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

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_game_text_test OK\n");
	return 0;
}
