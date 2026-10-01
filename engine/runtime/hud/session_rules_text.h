#pragma once

// The CMAP RULES tab's text: the session status block, the win conditions,
// the game type's rules and the mission briefing, the scoring rules and each
// scored stat's explanation, as the CMAP show hands it to RULELIST. Every
// string is the game's own (the "Overlays" gametext table through
// hud::GameTextLookup); these port only the composition.
// Witness record: docs/interface/hud-re.md "The windowed map views" (D-HUD-19).

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
	// SessionStatus_GetElapsedMS at the build: the record's uptime plus the
	// milliseconds since its stamp [orig: @0x52d5f0].
	uint32_t elapsed_ms = 0;
	std::array<int32_t, 39> stats{};
	std::vector<std::pair<uint32_t, uint32_t>> options; // (key, value), at most 8
};

// The inputs HUD_BuildRulesAndBriefingText reads besides the game type.
struct RulesBriefingInputs {
	uint32_t game_type = 0;       // g_GameType
	int local_team = 0;           // g_LocalPlayerEntity->Team, 0 without one
	bool spawn_zones = false;     // sub_43B910: g_SpawnZoneCount > 0
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

} // namespace opennova::hud
