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

// The waypoint marker facts the name and label read off the current entry's
// entity: the raw name id (entity+672, the BMS record's +0x60), its item
// def's type id (def+80) and attrib dword (def+84) when it has a def
// (entity+32), and the authored zone byte (entity+538, .mis "lfp_group").
struct WaypointNameKey {
	int32_t name_id = 0;
	bool has_def = false;
	int32_t def_type = 0;
	uint32_t def_attrib = 0;
	uint8_t zone_number = 0;
};

// The waypoint label's display name [orig: HUD_GetWaypointName @0x594630].
// Outside a session: mission WPNames/STRWPNAME%03i of the RAW id
// (@0x594668). In a session the id is first remapped +1 unless the game type
// carries the co-op bit 0x20000 (@0x594678 — retail single player boots as
// 0x10020 and is never in session, its launch setting network type 0
// [orig: CNapiNetwork_SetNetworkType(0) @0x561bce], so it always keys the
// raw id); then a def
// picks a gametext WPNames special key — attrib 0x80000 ARMORY, else 0x8000
// TARGET, else type 4091/4093/4095/4096/4097 FLAG, 4098/4100..4103 FLAGBAY
// (@0x594688..0x59470D) — and an empty result (no key, a miss) falls back to
// the mission STRWPNAME%03i of the remapped id (@0x59472D), read as the spawn
// left it: its first 15 characters (game_text_lookup.h kWaypointNameChars; the
// spawn cuts the key of each type-6005 record's own id, the one read out of a
// session, and the port cuts every read). The mission
// fallback's empty or "null" (any case) takes gametext
// WPNames/STRWPNAMEDEFAULT (@0x59476F..0x59477B); a special key's text is
// never "null"-tested.
std::string waypoint_display_name(const WaypointNameKey &key, bool in_session,
		uint32_t game_type, const GameTextLookup &mission, const GameTextLookup &gametext);

// The drawn waypoint label [orig: HUD_DrawWaypointNameAndDistance
// @0x5948d3..0x5949c0]: gametext hud/mto ("m to") and the name joined
// "%s %s" — under CTF (0x10004) a def of type 4091/4098 wraps the name in the
// blue run "%s <c4050FF>%s<co>", 4093/4100 in the red "%s <cFF3535>%s<co>";
// then a def with attrib 0x40000 and a nonzero zone byte b REPLACES it with
// "%s %s %c-%d" of mto, Overlays/LFP, the letter (b & 0x1F) + 64 and b >> 5.
std::string waypoint_label_text(const std::string &name, const WaypointNameKey &key,
		uint32_t game_type, const GameTextLookup &gametext);

// The MP session lines' gametext strings (hud_frame.h HudSessionText), each
// through GameText_GetString, whose miss is "" [orig: @0x51EC00]: the timer
// label Overlays/STROVER50 [orig: HUD_DrawGameTimer @0x593E00], the counts'
// Client/STRCLI25 / STRCLI04 / STRCLI23 [orig: HUD_DrawScoreOverlay
// @0x593ED9 / @0x593F04 / @0x593F28], "In the Zone" Overlays/STROVER53
// [orig: HUD_DrawGameTimerOverlay @0x59CE2D], the team names client/strcli19,
// 05, 06, 17, 18, 01 and the A&D suffixes strcli20 / strcli21 [orig:
// HUD_DrawTeamIdLine @0x59AAED..0x59AC96].
struct HudSessionText;
void hud_session_text(const GameTextLookup &gametext, HudSessionText &out);

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
