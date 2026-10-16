#include <runtime/inmatch/session_status.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <base/io/le.h>
#include <base/gameprofile/game_type.h>
#include <formats/score/score.h>
#include <runtime/world/match.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {
namespace gtype = opennova::game_type;
namespace {

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
	// The file as ScoreConfig_LoadFile reads it: the lines and tokens, the VERSION 40 gate,
	// the rows by their GAMETYPE names (formats/score, score::parse). A file of another
	// version is read for nothing, the defaults standing.
	// [orig: ScoreConfig_LoadFile @0x52D8A0, `version == 40` @0x52DA8A]
	score::File file;
	std::string error;
	if (!score::parse(reinterpret_cast<const uint8_t *>(score_ini.data()), score_ini.size(), file,
				error) ||
			file.version != score::kVersion)
		return false;
	// EXP_FANFARE is the file's whatever the session's game type: its arm sits beside the
	// row tests, inside the version-40 branch alone, and the file is read into every row at
	// once, so a VERSION 40 file's pair is stored with or without a block for the session's
	// row, Flag Me's included. The title's pair stands where no line passes the gate.
	// [orig: GameType_CreateDefaultSettings @0x52DD00 (the title's pair); ScoreConfig_LoadFile
	//  — the EXP_FANFARE arm @0x52DC59..0x52DC9F beside `if (currentGameType)` @0x52DB7C /
	//  @0x52DC05; the byte gate @0x52DC84..0x52DC8E, LOBYTE / HIBYTE stores @0x52DC96 /
	//  @0x52DC9F]
	config.exp_fanfare = score::exp_fanfare_kept(file)
			? static_cast<uint16_t>((static_cast<uint32_t>(file.exp_fanfare[0]) & 0xFFu) |
					((static_cast<uint32_t>(file.exp_fanfare[1]) & 0xFFu) << 8))
			: world::default_score_exp_fanfare(config.title);
	// Flag Me's selector is the intentionally invalid row 12. Retail loads no
	// FIELD/VAR row for it; in particular it never falls through to Co-op.
	// [orig: ScoreConfig_LoadScoringTableForGameType @0x52D300, the `<= 11` gate @0x52d3db]
	const int row = score::row_for_game_type(config.game_type);
	if (row < 0) return false;
	const score::Row loaded = score::row_of(file, row);
	if (!loaded.found) return false;
	std::array<int32_t, 39> values =
			world::default_match_score_values(config.game_type, config.title);
	for (const score::Entry &var : loaded.vars)
		if (var.id >= 0 && static_cast<size_t>(var.id) < values.size())
			values[static_cast<size_t>(var.id)] = var.value;
	config.session_status_stat_values = values;
	config.scoreboard_fields.clear();
	for (const score::Entry &field : loaded.fields)
		config.scoreboard_fields.emplace_back(
				static_cast<uint8_t>(field.id), static_cast<uint8_t>(field.value));
	return true;
}

std::vector<uint8_t> serialize_session_status(
		const GameConfig &config, uint32_t uptime_ms, bool in_session,
		world::World *match_world) {
	std::vector<uint8_t> out;
	// Napi_CopyString stores at most 31/63 characters in the 32/64-byte report
	// fields before the serializer walks the resulting C strings.
	append_cstr_limited(out, config.server_name, 31);
	append_cstr_limited(out, config.mission_name, 63);
	out.push_back(static_cast<uint8_t>(config.game_type));
	out.push_back(gtype::score_table_index(config.game_type));
	// The cfg's cap, never the slot limit [orig: Server_BuildStatusReport
	// @0x530A60, maxPlayers_3F4 @0x530B75..0x530B81].
	out.push_back(static_cast<uint8_t>(config.max_players));
	append_u32(out, uptime_ms);
	const std::array<int32_t, 39> default_values =
			world::default_match_score_values(config.game_type, config.title);
	const std::array<int32_t, 39> &score_values =
			config.session_status_stat_values.has_value()
			? *config.session_status_stat_values
			: default_values;
	for (int32_t value : score_values)
		append_u32(out, static_cast<uint32_t>(value));

	std::vector<std::pair<uint8_t, uint32_t>> options;
	const uint32_t game_type = config.game_type;
	// Co-op's key 9 is the mission's defined-subgoal count (the leading run of
	// authored win conditions, at most 8), sent only when nonzero.
	// [orig: Server_BuildStatusReport @0x530cb0..0x530ce1 — the scan of
	//  g_MissionWinConditionIds @0xA7628C, the same run
	//  Score_CountMissionSubgoalsAndUnits @0x509dc4 counts]
	if (gtype::is_waypoint_family(game_type) && match_world != nullptr) {
		const int32_t subgoals = world::count_defined_subgoals(*match_world);
		if (subgoals > 0) options.emplace_back(9, static_cast<uint32_t>(subgoals));
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
	// [orig: Server_ResetRoundCounters @0x516C50; Server_BuildStatusReport
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
	// [orig: Server_BuildStatusReport @0x530e71..0x530eac — is_in_session,
	//  !(type & 0x20000), g_RespawnTime != 0]
	if (in_session && !gtype::is_objective(game_type) && config.respawn_time != 0) {
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

} // namespace opennova::inmatch
