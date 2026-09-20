#pragma once

// The HUD's game-text compositions (ADR 0040 ladder E3b): the mission-table
// and gametext keys the HUD builds from an id, and each one's witnessed
// fallback. Every string is the game's own — these only choose the section,
// compose the key and apply the miss rule; the embedder supplies the tables
// through the hud::GameTextLookup seam.

#include <runtime/hud/game_text_lookup.h>

#include <string>

namespace opennova::hud {

// The waypoint label's display name. Our SP runtime is the co-op session
// shape (gametype 0x30020), whose `& 0x20000` branch keys STRWPNAME by the
// RAW authored id — the +1 remap belongs to the non-co-op MP gametypes,
// unported with them. The armory/target/flag specials key off MP POI entity
// types, not SP route markers.
// [orig: get_waypoint_name @0x594630 — index remap @0x594678; mission-table
//  fallback @0x59473d ("STRWPNAME%03i" in WPNames); empty or "null" ->
//  gametext WPNames/STRWPNAMEDEFAULT @0x59477b]
std::string waypoint_display_name(int name_id, const GameTextLookup &mission,
		const GameTextLookup &gametext);

// A resolved subgoal's chat-feed announcement: the mission table's
// WinConditions/STRWINMSG%03d or LoseConditions/STRLOSEMSG%03d line for the
// header text id, "" when the table lacks it (nothing posts). The LOST line
// also stamps the persistent banner — the caller's leg.
// [orig: case 14 @0x454543 STRWINMSG chat; case 15 @0x454612 STRLOSEMSG chat
//  + GameMsg_SetBannerText @0x454647]
std::string subgoal_message(bool lost, int header_id, const GameTextLookup &mission);

// The mission table's "Triggered Text" line for a text id, read directly (no
// override-table consult); a miss shows nothing ("").
// [orig: HUD_DisplayTriggeredText @0x51f190 — key ID%03i]
std::string triggered_text(int text_id, const GameTextLookup &mission);

// The weapon's HUD display name: the raw weapon id resolved in gametext's
// WepDes section; a miss (or an empty id) is "" and the element draws nothing.
// [orig: GameText_GetString("WepDes", weapondef+20) @0x593b7f; miss "" @0x51ec00]
std::string weapon_display_name(const std::string &weapon_id, const GameTextLookup &gametext);

} // namespace opennova::hud
