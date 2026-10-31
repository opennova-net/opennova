#pragma once

// The CMAP RULES tab's text: the session status block, the win conditions,
// the game type's rules and the mission briefing, the scoring rules and each
// scored stat's explanation, as the CMAP show hands it to RULELIST; and the
// in-session end-game (I) screen's four pages of the same matter. Every
// string is the game's own (the "Overlays" gametext table through
// hud::GameTextLookup, the KeyHelp table for the screen's footer); these port
// only the composition.
// Witness record: docs/interface/hud-re.md "The windowed map views" (D-HUD-19)
// and "The I briefing panel".

#include <runtime/hud/game_text_lookup.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::hud {

// What the text reads of the session status record (g_SessionStatus): a
// joiner's S2C 0x58 fold, the authority's own report.
// [orig: g_SessionStatus @0x24E3E88 — valid +0x00, server name +0x04,
//  mission name +0x24, max players +0x6C, the 39 stat values +0x78, the
//  option count +0x114 and pairs +0x118]
struct SessionStatusView {
	bool valid = false;
	std::string server_name;
	std::string mission_name;
	uint32_t max_players = 0;
	// CSessionTimer_GetElapsedMS at the build: the record's uptime plus the
	// milliseconds since its stamp [orig: @0x52d5f0].
	uint32_t elapsed_ms = 0;
	std::array<int32_t, 39> stats{};
	std::vector<std::pair<uint32_t, uint32_t>> options; // (key, value), at most 8
};

// The inputs HUD_BuildRulesAndBriefingText reads besides the game type.
struct RulesBriefingInputs {
	uint32_t game_type = 0;       // g_GameType
	int local_team = 0;           // g_LocalPlayerEntity->Team, 0 without one
	bool spawn_zones = false;     // SpawnZoneList_HasAny: g_SpawnZoneCount > 0
	bool authority = false;       // g_NapiNPCtx.is_authority
	// The authority's mission text: info/briefing3 and info/briefing2 (else
	// info/briefing), "" on a miss (MissionText_GetString @0x51eb50).
	std::string briefing3;
	std::string briefing2;
	// A client's S2C 0x7E strings as its reads see them: byte_A86520 (the
	// first) and byte_A86120 (the second).
	std::string config_first;
	std::string config_second;
};

// One win-condition line: the key's Overlays format over the value.
// [orig: HUD_FormatEndGameConditionText @0x5bce80]
std::string format_end_game_condition(uint32_t key, uint32_t value, const GameTextLookup &gametext);

// Append the game type's rules and the briefing to `out`; `mission_flag`
// picks briefing3 (the authority) or the first 0x7E string (a client) when
// set. [orig: HUD_BuildRulesAndBriefingText @0x5b92d0]
void append_rules_and_briefing_text(std::string &out, const RulesBriefingInputs &inputs,
		bool mission_flag, const GameTextLookup &gametext);

// The RULELIST text; false (nothing built, the widget keeps its old text)
// while the status record is not valid. `in_game_count` is the 0x16 trailer's
// in-game count (g_ScoreboardInGameCount).
// [orig: Overlay_BuildEndGameStatsText @0x54a240]
bool build_end_game_stats_text(const SessionStatusView &status, uint32_t in_game_count,
		const RulesBriefingInputs &inputs, const GameTextLookup &gametext, std::string &out);

// The in-session end-game (I) screen's page count: the page keys wrap modulo it
// (HudBriefingPages::cycle) and the footer names it [orig: HUD_DrawEndGameScreen
// @0x5bdd52 (`push 4`); HUD_CycleBriefingPage @0x5b9197..0x5b91d1].
inline constexpr int kEndGameScreenPages = 4;

// One page of the end-game screen, as its drawer rebuilds it each frame.
struct EndGameScreenPage {
	std::string text;
	// Whether the page's inline tags draw live. The wrapper's tags-inert byte
	// is the low byte of its state block's one unwritten dword, which holds
	// the residue of the screen's last call before the wrap (D-HUD-50's
	// mechanism): the screen's esp - 0x3C. Page 0's is the rules builder's
	// key buffer at byte 20: TKOTH's key reaches it (its team letter after
	// "STROVER_RULES_TKOTH"), and every other game type leaves there the
	// saved ebx of the session block's last sprintf, the uptime's seconds
	// field, so the rules' and briefing's tags draw live only in the second
	// each minute that field reads 0. Pages 2 and 3 leave the saved ebx of
	// the last explanation's sprintf, that stat's value, so their `<b>` names
	// draw bold only when its low byte is 0. Page 1 composes no tag; its
	// residue (the last stat line's sprintf) is not modelled and it draws
	// inert, as does an empty page.
	// [orig: HUD_DrawEndGameScreen @0x5bdd36 (the wrapper's block at its entry
	//  esp - 0x14); HUD_DrawWrappedText @0x580c00 (sub esp, 0x54; the block
	//  @0x580caa..0x580cce); HUD_BuildRulesAndBriefingText @0x5b92d0 (key at
	//  its entry esp - 0x44; the TKOTH key @0x5b94f0, its suffix
	//  @0x5b9577..0x5b9589); sprintf @0x76a9e4 (push ebx @0x76a9ea); ebx the
	//  seconds @0x5bd197 to the NUMPLAYERS sprintf @0x5bd434; ebx the value
	//  @0x5bda3b / @0x5bdbac to the explanations' sprintf @0x5bdb4a /
	//  @0x5bdcba]
	bool tags_live = false;
};

// Build `page` (0..3) of the end-game screen; false (nothing built: the
// screen draws its box alone) while the status record is not valid. Pages 0
// and 1 open on the session block, the mission line carrying the active
// expansion's name (g_ExpansionName) and the win conditions left out in a
// session; page 0 then carries the game type's rules and the briefing, page 1
// the points and penalties lines of the stats with a name, the penalties'
// header written before the first one; pages 2 and 3 explain up to eight
// scored stats each, the awards then the penalties, page 3 past the first
// eight. [orig: HUD_DrawEndGameScreen @0x5bd070]
bool build_end_game_screen_page(int page, const SessionStatusView &status,
		uint32_t in_game_count, const std::string &expansion, const RulesBriefingInputs &inputs,
		const GameTextLookup &gametext, EndGameScreenPage &out);

// The screen's footer lines: KeyHelp Text/PAGE as the format of (page + 1,
// 4) and Text/CHANGE_SCREEN as a format with no argument, each the table's
// entry, else its literal untouched; the getter answers only once gametext is
// loaded. [orig: HUD_DrawEndGameScreen @0x5bdd52..0x5bdd78, @0x5bddc9..0x5bdde9;
// KeyHelp_GetStringWithFallback @0x51ed40 (the gametext gate @0x51ed47)]
void end_game_screen_footer(int page, const GameTextLookup &keyhelp, bool gametext_loaded,
		std::string &page_line, std::string &footer);

} // namespace opennova::hud
