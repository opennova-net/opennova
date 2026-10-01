#include <runtime/hud/session_rules_text.h>

#include <base/gameprofile/game_type.h>
#include <runtime/hud/hud_game_text.h>

#include <cstdarg>
#include <cstdio>

namespace opennova::hud {

namespace {

// sprintf into a line, appended (the retail line buffer strcat'd onto the
// text) — every format here is the binary's own literal.
void appendf(std::string &out, const char *format, ...) {
	char stack[512];
	va_list args;
	va_start(args, format);
	va_list copy;
	va_copy(copy, args);
	const int n = std::vsnprintf(stack, sizeof(stack), format, args);
	va_end(args);
	if (n < 0) {
		va_end(copy);
		return;
	}
	if (static_cast<size_t>(n) < sizeof(stack)) {
		out.append(stack, static_cast<size_t>(n));
	} else {
		std::string big(static_cast<size_t>(n) + 1, '\0');
		std::vsnprintf(big.data(), big.size(), format, copy);
		big.resize(static_cast<size_t>(n));
		out += big;
	}
	va_end(copy);
}

std::string overlays(const GameTextLookup &gametext, const char *key, const char *fallback) {
	return game_text(gametext, "Overlays", key, fallback);
}

// SessionStatus_GetStatPointValue: 0 outside 0..38 [orig: @0x52d5d0].
int32_t stat(const SessionStatusView &status, int index) {
	return index >= 0 && index <= 38 ? status.stats[static_cast<size_t>(index)] : 0;
}

// The points / penalties gate: stat 12 (the zone quantum) shows under KOTH /
// TKOTH only when stat 33 is set, else when stat 31 or 32 is; stat 36 (the
// alive quantum) only when stat 35 is.
// [orig: Overlay_BuildEndGameStatsText @0x54a751..0x54a7a3, @0x54a950..0x54a9a2]
bool stat_gate(const SessionStatusView &status, uint32_t game_type, int index) {
	if (index == 12) {
		if (game_type == game_type::kTeamKingOfTheHill || game_type == game_type::kKingOfTheHill)
			return stat(status, 33) != 0;
		return stat(status, 31) != 0 || stat(status, 32) != 0;
	}
	if (index == 36) return stat(status, 35) != 0;
	return true;
}

// One points / penalties line: the distances in metres, the two quanta in
// seconds, every other value signed.
// [orig: Overlay_BuildEndGameStatsText @0x54a7e1..0x54a86f, @0x54a9e0..0x54aa6e]
void append_stat_line(std::string &out, int index, int32_t value, const GameTextLookup &gametext) {
	char key[64];
	char fallback[64];
	std::snprintf(key, sizeof(key), "STROVER_STATVAR%02ld", static_cast<long>(index));
	std::snprintf(fallback, sizeof(fallback), "!STROVER_STATVAR%02ld", static_cast<long>(index));
	if (index == 29 || index == 30 || index == 12 || index == 36) {
		const std::string unit = index == 29 || index == 30
				? overlays(gametext, "METERS", "!meters")
				: overlays(gametext, "SECONDS", "!seconds");
		appendf(out, "%s:\t%ld %s\r\n", overlays(gametext, key, fallback).c_str(),
				static_cast<long>(value), unit.c_str());
		return;
	}
	appendf(out, "%s:\t%+ld\r\n", overlays(gametext, key, fallback).c_str(), static_cast<long>(value));
}

// One stat's explanation: the bold name, then the STATVAREXP text. The
// negative walk's fallbacks carry no "!" (@0x54ac24).
void append_stat_explanation(std::string &out, int index, bool bang, const GameTextLookup &gametext) {
	char key[64];
	char fallback[64];
	std::snprintf(key, sizeof(key), "STROVER_STATVAR%02ld", static_cast<long>(index));
	std::snprintf(fallback, sizeof(fallback), bang ? "!STROVER_STATVAR%02ld" : "STROVER_STATVAR%02ld",
			static_cast<long>(index));
	appendf(out, "<b>%s<-b>\r\n", overlays(gametext, key, fallback).c_str());
	std::snprintf(key, sizeof(key), "STROVER_STATVAREXP%02ld", static_cast<long>(index));
	std::snprintf(fallback, sizeof(fallback),
			bang ? "!STROVER_STATVAREXP%02ld" : "STROVER_STATVAREXP%02ld", static_cast<long>(index));
	appendf(out, "%s\r\n\r\n", overlays(gametext, key, fallback).c_str());
}

// The rules key's team suffix: "_B" for team 1, "_R" for team 2, "_G" else
// [orig: the suffix words 0x425F @0x5b939a, 0x525F @0x5b93be, 0x475F @0x5b93da].
std::string team_suffixed(const char *key, int team) {
	return std::string(key) + (team == 1 ? "_B" : team == 2 ? "_R" : "_G");
}

} // namespace

std::string format_end_game_condition(uint32_t key, uint32_t value, const GameTextLookup &gametext) {
	// [orig: HUD_FormatEndGameConditionText @0x5bce80 — keys 0..9; the
	//  default arm (the pair itself as the format) is unreachable, the fold
	//  keeping keys up to 9 only (@0x53107f)]
	static const char *const kKeys[10] = {
		"STROVER_ENDGAMECOND_XP",
		"STROVER_ENDGAMECOND_ENEMYKILLS",
		"STROVER_ENDGAMECOND_ZONETIME",
		"STROVER_ENDGAMECOND_BLUETGTS",
		"STROVER_ENDGAMECOND_REDTGTS",
		"STROVER_ENDGAMECOND_FLAGS",
		"STROVER_ENDGAMECOND_BLUEFLAGS",
		"STROVER_ENDGAMECOND_REDFLAGS",
		"STROVER_ENDGAMECOND_TIMELIMIT",
		"STROVER_ENDGAMECOND_OBJECTIVES",
	};
	if (key > 9) return std::string();
	return hud_sprintf(overlays(gametext, kKeys[key], "!?? %ld"), static_cast<int32_t>(value));
}

void append_rules_and_briefing_text(std::string &out, const RulesBriefingInputs &inputs,
		bool mission_flag, const GameTextLookup &gametext) {
	// [orig: HUD_BuildRulesAndBriefingText @0x5b92d0]
	const uint32_t game_type = inputs.game_type;
	const int team = inputs.local_team;
	switch (game_type) {
	case game_type::kDeathmatch:
		out += overlays(gametext, "STROVER_RULES_DM", "!");
		break;
	case game_type::kKingOfTheHill:
		out += overlays(gametext, "STROVER_RULES_KOTH", "!");
		break;
	case game_type::kTeamDeathmatch:
	case game_type::kTeamKingOfTheHill:
	case game_type::kSearchAndDestroy:
	case game_type::kAttackDefend: {
		const char *key = game_type == game_type::kTeamDeathmatch ? "STROVER_RULES_TDM"
				: game_type == game_type::kTeamKingOfTheHill      ? "STROVER_RULES_TKOTH"
				: game_type == game_type::kSearchAndDestroy       ? "STROVER_RULES_SD"
																   : "STROVER_RULES_AD";
		out += overlays(gametext, team_suffixed(key, team).c_str(), "!");
		// The protected-spawn-point games add their PSP paragraph while the
		// mission has a spawn zone [orig: sub_43B910 — g_SpawnZoneCount > 0;
		// @0x5b995d..0x5b99a5].
		if (inputs.spawn_zones) {
			out += "\r\n\r\n";
			out += overlays(gametext, "STROVER_RULES_PSPGAMES", "!");
		}
		break;
	}
	case game_type::kCaptureTheFlag:
	case game_type::kFlagBall:
	case game_type::kAdvanceAndSecure:
	case game_type::kConquerAndControl: {
		const char *key = game_type == game_type::kCaptureTheFlag ? "STROVER_RULES_CTF"
				: game_type == game_type::kFlagBall               ? "STROVER_RULES_FB"
				: game_type == game_type::kAdvanceAndSecure       ? "STROVER_RULES_AAS"
																   : "STROVER_RULES_CAC";
		out += overlays(gametext, team_suffixed(key, team).c_str(), "!");
		break;
	}
	default:
		break; // Co-op, type 8 and the rest carry no rules text (LABEL_114)
	}
	if (!game_type::is_waypoint_family(game_type)) out += "\r\n"; // @0x5b9c82
	// The briefing: a client reads the 0x7E strings (the first when the flag
	// asks for it and it is not empty, else the second); the authority its
	// mission text, briefing3 under the flag, else briefing2, else briefing.
	if (!inputs.authority) {
		out += mission_flag && !inputs.config_first.empty() ? inputs.config_first
															: inputs.config_second;
		return;
	}
	if (mission_flag && !inputs.briefing3.empty()) {
		out += inputs.briefing3;
		return;
	}
	out += inputs.briefing2;
}

bool build_end_game_stats_text(const SessionStatusView &status, uint32_t in_game_count,
		const RulesBriefingInputs &inputs, const GameTextLookup &gametext, std::string &out) {
	// [orig: Overlay_BuildEndGameStatsText @0x54a240 — a no-op while the
	//  record is not valid (@0x54a272)]
	if (!status.valid) return false;
	out.clear();
	const uint32_t ms = status.elapsed_ms;
	const uint32_t seconds = ms / 1000 % 60;
	const uint32_t total_minutes = ms / 1000 / 60;
	const uint32_t hours = total_minutes / 60;
	const uint32_t minutes = total_minutes % 60;
	appendf(out, "%s\t%s\r\n", overlays(gametext, "STROVER_SERVERNAME", "!Servername:").c_str(),
			status.server_name.c_str());
	appendf(out, "%s\t%s\r\n", overlays(gametext, "STROVER_MISSIONNAME", "!Missionname:").c_str(),
			status.mission_name.c_str());
	appendf(out, "%s\t%ld %2.2ld:%2.2ld:%2.2ld\r\n",
			overlays(gametext, "STROVER_UPTIME", "!Uptime:").c_str(), static_cast<long>(hours / 24),
			static_cast<long>(hours % 24), static_cast<long>(minutes), static_cast<long>(seconds));
	// The game type's label: GameText_GetString, whose miss is "", and ""
	// for an unlisted type [orig: HUD_GetGameTypeOverlayLabel @0x5b8680].
	const char *label_key = game_type::overlay_label_key(inputs.game_type);
	const std::string label = label_key[0] != '\0' ? game_text(gametext, "Overlays", label_key, "")
												   : std::string();
	appendf(out, "%s\t%s\r\n", overlays(gametext, "STROVER_GAMETYPE", "!Gametype:").c_str(),
			label.c_str());
	appendf(out, "%s\t%ld\r\n",
			overlays(gametext, "STROVER_MAXPLAYERSALLOWED", "!Max Players:").c_str(),
			static_cast<long>(status.max_players));
	appendf(out, "%s\t%ld\r\n", overlays(gametext, "STROVER_NUMPLAYERS", "!Num Players:").c_str(),
			static_cast<long>(in_game_count));
	out += "\r\n"; // @0x54a5b3
	for (size_t i = 0; i < status.options.size(); ++i) {
		const std::string condition =
				format_end_game_condition(status.options[i].first, status.options[i].second, gametext);
		if (i == 0) {
			appendf(out, "%s\t%s\r\n",
					overlays(gametext, "STROVER_ENDGAMECOND_TITLEWIN", "!Win Condition:").c_str(),
					condition.c_str());
		} else {
			appendf(out, "\t%s\r\n", condition.c_str());
		}
	}
	out += "\r\n\r\n\r\n"; // @0x54a69b
	append_rules_and_briefing_text(out, inputs, false, gametext); // @0x54a6b4
	out += "\r\n\r\n\r\n\r\n"; // @0x54a6dc
	appendf(out, "%s\r\n", overlays(gametext, "STROVER_RULEPOINTS", "!Points:").c_str());
	for (int index = 0; index <= 38; ++index) { // @0x54a74f..0x54a8b5
		if (!stat_gate(status, inputs.game_type, index)) continue;
		const int32_t value = stat(status, index);
		if (value > 0) append_stat_line(out, index, value, gametext);
	}
	out += "\r\n"; // @0x54a8e6
	appendf(out, "%s\r\n", overlays(gametext, "STROVER_RULEPENALTIES", "!Penalties:").c_str());
	for (int index = 0; index <= 38; ++index) { // @0x54a948..0x54aab5
		if (!stat_gate(status, inputs.game_type, index)) continue;
		const int32_t value = stat(status, index);
		if (value < 0) append_stat_line(out, index, value, gametext);
	}
	out += "\r\n\r\n\r\n"; // @0x54aadc
	// Every scored stat's explanation, the awards then the penalties, with
	// no quantum gate (@0x54aaeb, @0x54ac24).
	for (int index = 0; index <= 38; ++index)
		if (stat(status, index) > 0) append_stat_explanation(out, index, true, gametext);
	for (int index = 0; index <= 38; ++index)
		if (stat(status, index) < 0) append_stat_explanation(out, index, false, gametext);
	return true;
}

} // namespace opennova::hud
