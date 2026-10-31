#pragma once

// The key-toggled overlay windows HUD_DrawGameplayOverlays draws after the
// objectives panel: the F1 key-binding help screen, the F12 map legend and
// the I briefing panel [orig: HUD_DrawGameplayOverlays @0x5BDE60 — the
// briefing @0x5be133..0x5be145, HelpScreen_Draw @0x5be179 (the map legend
// wins over the help list inside it @0x497800)]. The shell resolves the text
// tables; the compiler owns the witnessed layout and the briefing's page
// state (its drawer writes the next page's start line).

#include <array>
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

// The F12 map legend's resolved text: the title and one label per icon
// entry, each a gametext Hud lookup [orig: GameText_GetString("Hud",
// "hud_map_legend") @0x49749e; GameText_GetString("Hud", name) @0x4977a7].
struct HudMapLegendState {
	bool shown = false;
	std::string title;
	std::vector<std::string> labels; // one per entry of the title's legend (Title::hud_map_legend)
};

// The briefing panel's text [orig: HUD_DrawBriefingPanel @0x5bad00 — the authority reads
// MissionText info/briefing2, falling back to info/briefing @0x5bad96..
// 0x5badb9; a joiner draws the text its world state delivered
// (byte_A86120)]. Out of a session only: in a session the same window flag
// draws HUD_DrawEndGameScreen @0x5be13e instead.
// The F9 AudioEmote and F10 RadioMacro menus, drawn by the overlay-panel
// pass while the death screen is down: a stdbox (128, 180)..(896, 550)
// titled from vmacros macrotext EMOTES_TITLE / RADIO_TITLE, then ten rows
// "%i - %s" at x 200 from y 225 in steps of 30 (the tenth numbered 0), each
// the macrotext of the row's voice-macro key (the key itself on a miss) in
// the bold slot, the context rows from kHudEmotesMenuContextRow /
// kHudRadioMenuContextRow on in palette entry 4. The embedder resolves the
// title and the ten texts (inmatch hud_role_facts).
// [orig: HUD_DrawOverlayPanels @0x5c00cf..0x5c00f9 (both behind
//  !g_DeathScreenActive); HUD_DrawEmotesMenu @0x5bff00 (the box @0x5bff5c,
//  the rows @0x5bff81..0x5c002a); HUD_DrawRadioTitleMenu @0x5bfb90 (the box
//  @0x5bfbec, the rows @0x5bfc11..0x5bfcb4)]
struct HudVoiceMacroMenuState {
	bool shown = false;
	std::string title;
	std::array<std::string, 10> texts;
};
inline constexpr int kHudEmotesMenuContextRow = 8; // [orig: `cmp esi, 8` @0x5c0002]
inline constexpr int kHudRadioMenuContextRow = 9;  // [orig: `cmp esi, 9` @0x5bfc8f]

struct HudBriefingState {
	bool shown = false;
	std::string text;
};

// The I window in a session, the end-game screen: the embedder builds the
// page the briefing pages stand on (hud/session_rules_text.h
// build_end_game_screen_page, end_game_screen_footer; inmatch
// end_game_screen_text) and the compiler draws it: its box, then while the
// session status record is valid the wrapped page and the two footer lines
// [orig: HUD_DrawEndGameScreen @0x5bd070, drawn on the briefing's window flag
// in a session @0x5be135..0x5be13e].
struct HudEndGameScreenState {
	bool shown = false;
	bool valid = false;  // g_SessionStatus' valid word (@0x5bd10f)
	int page = 0;        // the page the text was built for [orig: dword_28E3D6C]
	std::string text;
	bool tags_live = false; // EndGameScreenPage::tags_live
	std::string page_line;  // KeyHelp Text/PAGE over (page + 1, 4)
	std::string footer;     // KeyHelp Text/CHANGE_SCREEN
};

// The briefing pages: the current page and each page's first wrapped line;
// the drawer writes the next page's start when the page overflows, and the
// page keys walk them [orig: dword_28E3D6C (page), g_BriefingPageLineOffsets
// @0x28E3D70 (ten starts); HUD_CycleBriefingPage @0x5B9150 (the reset and the cycle)].
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
