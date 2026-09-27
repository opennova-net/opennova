#pragma once

// The HUD's game-text compositions (ADR 0040 ladder E3b): the mission-table
// and gametext keys the HUD builds from an id, and each one's witnessed
// fallback. Every string is the game's own — these only choose the section,
// compose the key and apply the miss rule; the embedder supplies the tables
// through the hud::GameTextLookup seam.

#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {

// One argument of a HUD template's sprintf: a 32-bit integer or a string.
struct HudTextArg {
	std::string text;
	int32_t number = 0;
	bool is_number = false;
};

// The CRT sprintf the HUD runs over a localized template: retail hands the
// authored text to sprintf AS THE FORMAT with the call site's own argument
// list -- none for the over-1km / auto / none scope labels and the FARP
// reloading line, one 32-bit int for the scope range / zero / magnification
// readouts, the mortar distance and the FARP wait, one key-name string for the
// armory and vehicle-bay prompts, and the end-round rows' mixed lists. A
// conversion `%[-+ #0]*[0-9]*(.[0-9]*)?(h|l|I32)?[diouxXc]` takes the next
// integer argument and `%[-+ #0]*[0-9]*(.[0-9]*)?h?s` the next string, printed
// through the host snprintf with retail's 32-bit int (`h` narrows to 16 bits
// first); `%%` prints '%'. Everything else stays literal text: a conversion
// whose argument is missing or of the other kind (its slot is still used up),
// and every form that would read memory the call never passed (%n, %p, %S,
// %e/%f/%g, `*` widths, 64-bit sizes). A printed NUL ends the string, as the
// drawers read a C string.
// [orig: sprintf @0x76A9E4 -- HUD_DrawScopeOverlayDetails @0x59E4D5
//  (STROVER_DIST1KM, no argument), @0x59E530 (STROVER_DIST), @0x59E8F6
//  (hud_scope_zero), @0x59E938 (hud_scope_zero_auto / _none, no argument),
//  @0x59E9BD (hud_scope_mag); HUD_RenderAllOverlays @0x5A8972 (STROVER_DIST);
//  HUD_DrawGameplayOverlays @0x5BDF45 / @0x5BDFC9 (key name), @0x5BE09C
//  (seconds), @0x5BE0E9 (no argument)]
std::string hud_sprintf(const std::string &format, const std::vector<HudTextArg> &args);
std::string hud_sprintf(const std::string &format);
std::string hud_sprintf(const std::string &format, int32_t value);

// The armory / vehicle-bay / FARP service line (prompt 1 armory, 2 vehicle bay,
// 3 FARP wait, 4 FARP reloading; anything else is ""). The armory and FARP
// templates come through GameText_GetString, whose miss is "" (@0x51EC08); the
// bay's through GameText_GetStringWithFallback with its compiled-in fallback.
// The key name formats the armory and bay lines, the seconds the wait line.
// [orig: HUD_DrawGameplayOverlays @0x5BDE60 -- armory @0x5BDF1B..0x5BDF45,
//  bay @0x5BDF9A..0x5BDFC9 (fallback @0x5BDFAC), FARP wait @0x5BE07A..0x5BE09C,
//  reloading @0x5BE0D4..0x5BE0E9]
std::string service_prompt_text(int prompt, const std::string &use_key, int32_t wait_seconds,
		const GameTextLookup &gametext);

// The waypoint label's display name. Our SP runtime is the co-op session
// shape (gametype 0x30020), whose `& 0x20000` branch keys STRWPNAME by the
// RAW authored id — the +1 remap belongs to the non-co-op MP gametypes,
// unported with them. The armory/target/flag specials key off MP POI entity
// types, not SP route markers.
// [orig: HUD_GetWaypointName @0x594630 — index remap @0x594678; mission-table
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

// A shown objective's two chat lines: the gametext Misc/STRMISC_NEWOBJECTIVE
// header ("" when absent) and the mission table's WinConditions/
// STRWINDIRECTIVE%03i or LoseConditions/STRLOSEDIRECTIVE%03i directive for the
// header text id, "" when that line is shorter than two characters (it then
// posts nothing).
// [orig: HUD_ShowObjectiveNotification @0x5ba2e0 — the directive keys
//  @0x5ba316/@0x5ba34b, the length drop @0x5ba37b, the header key @0x5ba39b]
std::string objective_header(const GameTextLookup &gametext);
std::string objective_directive(bool win, int header_id, const GameTextLookup &mission);

// The mission table's "Triggered Text" line for a text id, read directly (no
// override-table consult); a miss shows nothing ("").
// [orig: HUD_DisplayTriggeredText @0x51f190 — key ID%03i]
std::string triggered_text(int text_id, const GameTextLookup &mission);

// The weapon's HUD display name: the raw weapon id resolved in gametext's
// WepDes section; a miss (or an empty id) is "" and the element draws nothing.
// [orig: GameText_GetString("WepDes", weapondef+20) @0x593b7f; miss "" @0x51ec00]
std::string weapon_display_name(const std::string &weapon_id, const GameTextLookup &gametext);

} // namespace opennova::hud
