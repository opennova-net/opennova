#pragma once

// The command map's squad feed lines: what the squad S2C handlers post on
// the HUD's chat and system rings. The squad lines ride the CHAT ring in
// g_HUDColors.palette[4] for the 930-tick life; a received waypoint's line
// rides the SYSTEM ring in white.
// Witness record: docs/interface/hud-re.md "The command map" (D-HUD-19).

#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <string>

namespace opennova::hud {

struct SquadFeedLine {
	enum class Kind : uint8_t {
		Join,             // HUD_CMAP_JOIN(name)
		Recruit,          // HUD_CMAP_RECRUIT(name)
		Fireteam,         // HUD_CMAP_SETFIRETEAM(the fireteam label)
		GoCode,           // HUD_CMAP_GOCODE<UNIFORM..ZULU>
		WaypointReceived, // the literal "Waypoint \"%s\" received from %s."
	};
	Kind kind = Kind::Join;
	std::string text; // the subject's name, or the received line itself
	uint8_t value = 0; // the fireteam (0 none, 1..3) / the go code (0..5)
	uint32_t order = 0; // the carrying message's dispatch stamp (FeedPost::order)
};

struct SquadFeedPost {
	bool system_ring = false; // SYSTEM (Chat_AddMessageChannel2) or CHAT
	uint32_t argb = 0;
	std::string text;
	bool post = false; // a go code past 5 posts nothing
};

// The go-code suffixes 0..5 (UNIFORM, VICTOR, WHISKEY, XRAY, YANKEE, ZULU),
// "" past 5.
// [orig: Server_PlayGoCodeSoundAndChat @0x5522e0 — "_GC_UNIFORM" ...
//  "_GC_ZULU" @0x7D4AD0..0x7D4A20, the HUD keys HUD_CMAP_GOCODEUNIFORM ...]
const char *go_code_name(uint8_t code);

// Compose one line: the key from the "HUD" section (GameText_GetString's miss
// is ""), sprintf'd with its argument; the fireteam label from "MENU"
// (STR_CMAP_FIRETEAMA / B / C, else STR_CMAP_NOFIRETEAM).
// [orig: NapiNPClientMsg_HandleSquadJoin @0x42567b; NapiNPClientMsg_0x073
//  @0x425810..0x42586a; NapiNPClientMsg_PlayerRecruited @0x425915;
//  Server_PlayGoCodeSoundAndChat @0x552330..0x5524d8; Chat_AddMessageChannel1
//  (text, g_HUDColors.palette[4] = 0xFFF0F000, 930); Waypoint_CreateForPlayer
//  @0x4dfe6a — Chat_AddMessageChannel2(msg, -1, 930)]
SquadFeedPost squad_feed_post(const SquadFeedLine &line, const GameTextLookup &gametext);

} // namespace opennova::hud
