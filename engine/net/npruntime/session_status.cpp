#include "npruntime/session_status.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include <io/le.h>
#include <npwire/game_type.h>
#include <world/match.h>
#include <world/world.h>

namespace opennova::np {
namespace gtype = opennova::game_type;
namespace {

// [orig: ScoreConfig_LoadFile @0x52D8A0; Server_BuildStatusReport @0x530A60
// -> SessionStatus_SerializeToBuffer @0x5310C0]
constexpr std::array<const char *, 38> kScoreVarNames = {
	"FIRE", "HIT", "FRIENDLYKILL", "ENEMYKILL", "SUICIDE",
	"DEATH", "MEDICHEAL", "MEDICSAVE", "RESPAWN", "FLAGSAVE",
	"FLAGCAPTURE", "FLAGPICKUP", "ZONEQUANTUM", "DESTROYTARGET",
	"PSPATTEMPT", "PSPTAKEOVER", "MULTIPLEKILL", "HEADSHOTKILL",
	"KNIFEKILL", "SKILLKILL", "FLAGCARRIERKILL", "THEMINZONEKILL",
	"MEINZONEKILL", "THEMINMYZONEKILL", "MEINMYZONEKILL",
	"THEMINTHEIRZONEKILL", "MEINTHEIRZONEKILL", "ASSISTS",
	"ENEMYSNIPERKILL", "SNIPERSKILLKILLDISTANCEMIN",
	"SNIPERSKILLKILLDISTANCEMAX", "INAZONE", "INDZONE", "INZONE",
	"LFPTAKEOVER", "ALIVE", "ALIVEQUANTUM", "VATTACHKILL",
};

// IDs are one-based and positional in retail's lookup table.
// [orig: ScoreConfig_LoadFile @0x52D8A0]
constexpr std::array<const char *, 32> kScoreFieldNames = {
	"NUMSUICIDES", "NUMFRIENDLYKILLS", "NUMENEMYKILLS", "NUMDEATHS",
	"NUMSECONDSINZONE", "NUMFLAGSCAPTURED", "NUMFLAGSSAVED",
	"NUMTARGETSDESTROYED", "NUMSHOTSFIRED", "NUMMEDICSAVES",
	"NUMREVIVES", "NUMPSPATTEMPTS", "NUMPSPTAKEOVERS",
	"NUMFLAGCARRIERKILLS", "NUMMULTIPLEKILLS", "NUMHEADSHOTKILLS",
	"NUMKNIFEKILLS", "NUMTHEMINZONEKILLS", "EXPERIENCEPOINTS",
	"ITEMPOINTS", "NUMSHOTSPERKILL", "NUMMEINZONEKILLS",
	"NUMTHEMINMYZONEKILLS", "NUMMEINMYZONEKILLS",
	"NUMTHEMINTHEIRZONEKILLS", "NUMMEINTHEIRZONEKILLS",
	"NUMSKILLKILL", "NUMTHEMINFLAGZONEKILLS", "NUMMEINFLAGZONEKILLS",
	"NUMASSISTS", "NUMENEMYSNIPERKILLS", "NUMLFPTAKEOVERS",
};

bool ascii_iequals(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) !=
		    std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
}

const char *score_game_type_name(uint32_t game_type) {
	if (game_type == gtype::kDeathmatch) return "DM";
	if (game_type == gtype::kTeamDeathmatch) return "TDM";
	if (gtype::is_waypoint_family(game_type)) return "COOP";
	switch (game_type) {
	case gtype::kTeamKingOfTheHill: return "TKOTH";
	case gtype::kKingOfTheHill: return "KOTH";
	case gtype::kSearchAndDestroy: return "SD";
	case gtype::kAttackDefend: return "AD";
	case gtype::kCaptureTheFlag: return "CTF";
	case gtype::kFlagBall: return "FB";
	case gtype::kFlagMe: return nullptr;
	case gtype::kAdvanceAndSecure: return "AAS";
	case gtype::kConquerAndControl: return "CAC";
	default:
		// Retail normalizes its unknown/nonzero row 0 to the Co-op score row.
		return "COOP";
	}
}

void append_cstr_limited(
		std::vector<uint8_t> &out, const std::string &value,
		std::size_t kept_limit) {
	const std::size_t n = std::min(value.size(), kept_limit);
	out.insert(out.end(), value.begin(), value.begin() +
			static_cast<std::ptrdiff_t>(n));
	out.push_back(0);
}

void append_u32(std::vector<uint8_t> &out, uint32_t value) {
	opennova::io::append_u32_le(out, value);
}

} // namespace

bool load_session_score_config(GameConfig &config, std::string_view score_ini) {
	const char *target_name = score_game_type_name(config.game_type);
	// Flag Me's selector is the intentionally invalid row 12. Retail loads no
	// FIELD/VAR row for it; in particular it never falls through to Co-op.
	if (target_name == nullptr) return false;
	std::array<int32_t, 39> parsed =
			world::default_match_score_values(config.game_type);
	std::vector<std::pair<uint8_t, uint8_t>> parsed_fields;
	const std::string target = target_name;
	bool selected = false;
	bool found_target = false;
	bool selected_section_has_fields = false;
	bool saw_score_fields = false;
	bool saw_version = false;
	int version = 0;

	std::istringstream input{std::string(score_ini)};
	std::string line;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		std::istringstream row(line);
		std::string directive;
		if (!(row >> directive) || directive.rfind("//", 0) == 0) continue;

		if (ascii_iequals(directive, "VERSION")) {
			if (row >> version) saw_version = true;
			continue;
		}
		if (ascii_iequals(directive, "GAMETYPE")) {
			std::string name;
			if (row >> std::quoted(name)) {
				selected = ascii_iequals(name, target);
				found_target = found_target || selected;
			} else {
				selected = false;
			}
			selected_section_has_fields = false;
			continue;
		}
		if (!selected) continue;

		std::string name;
		int64_t value = 0;
		if (!(row >> std::quoted(name) >> value)) continue;
		if (ascii_iequals(directive, "FIELD")) {
			for (std::size_t i = 0; i < kScoreFieldNames.size(); ++i) {
				if (!ascii_iequals(name, kScoreFieldNames[i])) continue;
				// The first recognized FIELD after every matching GAMETYPE
				// replaces the prior/default schema; duplicate matching sections
				// therefore follow the same reset-and-replace behavior as retail.
				if (!selected_section_has_fields) {
					parsed_fields.clear();
					selected_section_has_fields = true;
					saw_score_fields = true;
				}
				if (parsed_fields.size() < 34) {
					parsed_fields.emplace_back(
							static_cast<uint8_t>(i + 1),
							static_cast<uint8_t>(value));
				}
				break;
			}
			continue;
		}
		if (!ascii_iequals(directive, "VAR")) continue;
		for (std::size_t i = 0; i < kScoreVarNames.size(); ++i) {
			if (!ascii_iequals(name, kScoreVarNames[i])) continue;
			parsed[i] = static_cast<int32_t>(static_cast<uint32_t>(value));
			break;
		}
	}

	if (!saw_version || version != 40 || !found_target) return false;
	config.session_status_stat_values = parsed;
	if (saw_score_fields)
		config.scoreboard_fields = std::move(parsed_fields);
	else
		config.scoreboard_fields.clear();
	return true;
}

std::vector<uint8_t> serialize_session_status(
		const GameConfig &config, uint32_t uptime_ms,
		uint32_t active_players, world::World *match_world) {
	std::vector<uint8_t> out;
	// Napi_CopyString stores at most 31/63 characters in the 32/64-byte report
	// fields before the serializer walks the resulting C strings.
	append_cstr_limited(out, config.server_name, 31);
	append_cstr_limited(out, config.mission_name, 63);
	out.push_back(static_cast<uint8_t>(config.game_type));
	out.push_back(gtype::score_table_index(config.game_type));
	out.push_back(static_cast<uint8_t>(config.max_players));
	append_u32(out, uptime_ms);
	const std::array<int32_t, 39> default_values =
			world::default_match_score_values(config.game_type);
	const std::array<int32_t, 39> &score_values =
			config.session_status_stat_values.has_value()
			? *config.session_status_stat_values
			: default_values;
	for (int32_t value : score_values)
		append_u32(out, static_cast<uint32_t>(value));

	std::vector<std::pair<uint8_t, uint32_t>> options;
	const uint32_t game_type = config.game_type;
	if (gtype::is_waypoint_family(game_type) && active_players > 0) {
		options.emplace_back(
				9, std::min<uint32_t>(active_players, 8));
	}
	if (game_type == gtype::kDeathmatch || game_type == gtype::kTeamDeathmatch) {
		options.emplace_back(1, config.score_limit);
	}
	if (game_type == gtype::kKingOfTheHill || game_type == gtype::kTeamKingOfTheHill) {
		options.emplace_back(2, config.time_limit_minutes);
	}
	// The four authored-target globals are populated by the round-start entity
	// census. Their option order and asymmetric keys are the literal status
	// writer order: S&D/A&D 3 then 4; CTF 7 then 6; zero targets are omitted.
	// [orig: reset_round_counters @0x516C50; Server_BuildStatusReport
	// @0x530A60, target rows @0x530B7C..0x530C04]
	if (match_world != nullptr &&
			(game_type == gtype::kSearchAndDestroy ||
			 game_type == gtype::kAttackDefend)) {
		const int32_t team2_target =
				match_world->match.demolition_target(*match_world, 2);
		const int32_t team1_target =
				match_world->match.demolition_target(*match_world, 1);
		if (team2_target > 0)
			options.emplace_back(3, static_cast<uint32_t>(team2_target));
		if (team1_target > 0)
			options.emplace_back(4, static_cast<uint32_t>(team1_target));
	}
	if (match_world != nullptr && game_type == gtype::kCaptureTheFlag) {
		const int32_t team1_target =
				match_world->match.flag_capture_target(*match_world, 1);
		const int32_t team2_target =
				match_world->match.flag_capture_target(*match_world, 2);
		if (team1_target > 0)
			options.emplace_back(7, static_cast<uint32_t>(team1_target));
		if (team2_target > 0)
			options.emplace_back(6, static_cast<uint32_t>(team2_target));
	}
	if (game_type == gtype::kFlagBall || game_type == gtype::kFlagMe)
		options.emplace_back(5, config.max_score);
	// Every live non-objective session with a nonzero respawn time appends key
	// 8. Objective Co-op (0x30020) suppresses it; training Co-op (0x10020)
	// therefore carries both key 9 and key 8 in the retail oracle.
	if (!gtype::is_objective(game_type) && config.respawn_time != 0) {
		options.emplace_back(8, config.respawn_time);
	}
	if (options.size() > 8) options.resize(8);

	out.push_back(static_cast<uint8_t>(options.size()));
	for (const auto &[key, value] : options) {
		out.push_back(key);
		append_u32(out, value);
	}
	// Retail loops while pair_index <= pair_count. The zero-initialized report
	// therefore contributes one unadvertised five-byte sentinel on every send.
	out.push_back(0);
	append_u32(out, 0);
	return out;
}

} // namespace opennova::np
