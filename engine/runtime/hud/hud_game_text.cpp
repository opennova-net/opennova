// The HUD's game-text compositions — see hud_game_text.h for the witnesses.
// [orig: get_waypoint_name @0x594630; HUD_DisplayTriggeredText @0x51f190]

#include <runtime/hud/hud_game_text.h>

#include <base/io/strutil.h>

#include <cstdio>

namespace opennova::hud {

std::string waypoint_display_name(int name_id, const GameTextLookup &mission,
		const GameTextLookup &gametext) {
	char key[32];
	std::snprintf(key, sizeof(key), "STRWPNAME%03d", name_id);
	const std::string name = mission("WPNames", key, "");
	// Empty or the literal "null" falls back to the gametext default
	// [orig: @0x59477b].
	if (name.empty() || strutil::iequals(name.c_str(), "null"))
		return gametext("WPNames", "STRWPNAMEDEFAULT", "");
	return name;
}

std::string subgoal_message(bool lost, int header_id, const GameTextLookup &mission) {
	char key[32];
	std::snprintf(key, sizeof(key), lost ? "STRLOSEMSG%03d" : "STRWINMSG%03d", header_id);
	return mission(lost ? "LoseConditions" : "WinConditions", key, "");
}

std::string triggered_text(int text_id, const GameTextLookup &mission) {
	char key[32];
	std::snprintf(key, sizeof(key), "ID%03d", text_id);
	return mission("Triggered Text", key, "");
}

std::string weapon_display_name(const std::string &weapon_id, const GameTextLookup &gametext) {
	if (weapon_id.empty()) return std::string();
	return gametext("WepDes", weapon_id.c_str(), "");
}

} // namespace opennova::hud
