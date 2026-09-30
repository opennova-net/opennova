#pragma once

// The key-toggled overlay windows HUD_DrawGameplayOverlays draws after the
// objectives panel: the F1 key-binding help screen, the F12 map legend and
// the I briefing panel [orig: HUD_DrawGameplayOverlays @0x5BDE60 — the
// briefing @0x5be133..0x5be145, HelpScreen_Draw @0x5be179 (the map legend
// wins over the help list inside it @0x497800)]. The shell resolves the text
// tables; the compiler owns the witnessed layout and the briefing's page
// state (its drawer writes the next page's start line).

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {

// The F1 help screen's resolved text (controls/help_screen.h builds the page).
struct HudHelpScreenState {
	bool shown = false;
	std::string title;     // "Help - <class>"
	std::string page_line; // "Page n of m"
	std::string footer;    // "PgUp and PgDn to change pages"
	std::vector<std::string> keys;  // row + 0, right-aligned at x 472
	std::vector<std::string> texts; // row + 128, left at x 512
};

// One map-legend entry: its gametext Hud key, TSDicon strip cell, size
// factor and ARGB [orig: the 44-byte records @0x8154F8 — name +0x00, cell
// +0x20, size +0x24 (float), colour +0x28; the walk stops on an empty name].
struct HudMapLegendIcon {
	const char *name;
	uint8_t cell;
	float size;
	uint32_t color;
};
inline constexpr HudMapLegendIcon kHudMapLegendIcons[] = {
	{"hud_icon_person", 3, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_medic", 0, 3.0f, 0xFFFFFFFFu},
	{"hud_icon_persondead", 8, 1.0f, 0x7F2F3F7Fu},
	{"hud_icon_personmedicable", 14, 1.0f, 0x7F2F3F7Fu},
	{"hud_icon_waypoint", 1, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_psp", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_lfp_attack", 26, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_lfp_defend", 27, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_emplaced", 4, 0.8f, 0x7F7F6200u},
	{"hud_icon_emplaced2", 12, 0.8f, 0x7F7F6200u},
	{"hud_icon_vehicle", 10, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_watervehicle", 15, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_helo", 11, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_motorcycle", 25, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_farp", 5, 1.0f, 0x7F7F6200u},
	{"hud_icon_armoury", 13, 0.8f, 0x7F207F20u},
	{"hud_icon_bldg", 0, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_bridge", 9, 1.0f, 0x7F207F20u},
	{"hud_icon_ridewanted", 23, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_enemyloc", 24, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_recentspeaker", 28, 0.8f, 0x7F7F7F7Fu},
	{" ", 0, 0.0f, 0x00000000u},
	{"hud_icon_flag", 2, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_flagbay", 6, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrmed", 16, 1.0f, 0x7F7F7F7Fu},
	{"hud_icon_pwrammo", 17, 1.0f, 0x7F7F7F7Fu},
};
inline constexpr int kHudMapLegendIconCount =
		static_cast<int>(sizeof(kHudMapLegendIcons) / sizeof(kHudMapLegendIcons[0]));

// The F12 map legend's resolved text: the title and one label per icon
// entry, each a gametext Hud lookup [orig: GameText_GetString("Hud",
// "hud_map_legend") @0x49749e; GameText_GetString("Hud", name) @0x4977a7].
struct HudMapLegendState {
	bool shown = false;
	std::string title;
	std::vector<std::string> labels; // kHudMapLegendIconCount entries
	int frame_counter = 0;           // the pulses' clock [orig: g_HUDFrameCounter @0xA87064]
};

// The briefing panel's text [orig: sub_5BAD00 — the authority reads
// MissionText info/briefing2, falling back to info/briefing @0x5bad96..
// 0x5badb9; a joiner draws the text its world state delivered
// (byte_A86120)]. Out of a session only: in a session the same window flag
// draws HUD_DrawEndGameScreen @0x5be13e instead.
struct HudBriefingState {
	bool shown = false;
	std::string text;
};

// The briefing pages: the current page and each page's first wrapped line;
// the drawer writes the next page's start when the page overflows, and the
// page keys walk them [orig: dword_28E3D6C (page), g_BriefingPageLineOffsets
// @0x28E3D70 (ten starts); sub_5B9150 @0x5B9150 (the reset and the cycle)].
struct HudBriefingPages {
	static constexpr int kPages = 10;
	int page = 0;
	int starts[kPages] = {};
	// Direction 0: every start and the page clear [orig: @0x5b9158..0x5b918a].
	void reset();
	// PgDn (+1) / PgUp (-1). In a session the four-page end-game screen wraps
	// modulo 4; out of one, forward needs the next page's start (page <= 8)
	// and back stops at page 0 [orig: @0x5b9197..0x5b9207].
	void cycle(int direction, bool in_session);
};

// One wrapped line of HUD_DrawWrappedText: a byte range of the text and the
// y it draws at (design-space pen, before scaling is the caller's).
struct HudWrappedLine {
	size_t begin = 0;
	size_t end = 0;
	int y = 0;
};

} // namespace opennova::hud
