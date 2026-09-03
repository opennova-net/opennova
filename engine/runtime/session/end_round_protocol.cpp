#include <runtime/session/end_round_protocol.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::np {
namespace {

int16_t wire_i16(int32_t value) {
	return static_cast<int16_t>(static_cast<uint16_t>(value));
}

std::string capped(const std::string &value, size_t kept) {
	return value.size() <= kept ? value : value.substr(0, kept);
}

int16_t scoreboard_ratio(const world::MatchStats &stats) {
	const int32_t denominator = stats[world::MatchStats::kEnemyKills];
	if (denominator == 0) return -1;
	// 32-bit `raw[2] << 16` exactly as the board builder computes it.
	const int32_t numerator =
			static_cast<int32_t>(static_cast<uint32_t>(stats[2]) << 16);
	return wire_i16(numerator / denominator);
}

} // namespace

EndRoundStats build_end_round_stats(const world::MatchResult &result) {
	EndRoundStats out;
	out.winner_team = static_cast<int8_t>(result.winner_team);
	out.team_score_0 = wire_i16(result.team_scores[0]);
	out.team_score_1 = wire_i16(result.team_scores[1]);
	// The board builder computes every configured column, then declares only
	// columns with a nonzero value in at least one player row. Disabled columns
	// still participate in this scan; their byte is presentation metadata.
	// [orig: Server_BuildEndOfRoundScoreboard @0x508F30]
	std::vector<size_t> active_fields;
	active_fields.reserve(result.score_fields.size());
	for (size_t i = 0; i < result.score_fields.size(); ++i) {
		const world::MatchScoreField &field = result.score_fields[i];
		const bool active = std::any_of(
				result.players.begin(), result.players.end(),
				[&](const world::MatchResultPlayer &player) {
					return world::match_score_field_value(
							player.stats, field.field, result.game_type) != 0;
				});
		if (!active) continue;
		active_fields.push_back(i);
		out.team_fields.emplace_back(field.field, field.enabled);
	}
	out.players.reserve(result.players.size());
	for (const world::MatchResultPlayer &player : result.players) {
		EndRoundPlayerRow row;
		row.slot = player.identity.slot;
		// The producer first stores these in 32/16/16-byte fixed buffers. The
		// second string is ALWAYS g_empty_str (@0x5090E3); the third is the
		// NovaWorld clan-list node tag when the slot's account netId finds one
		// (CLinkedList_FindByTag @0x509100), else g_empty_str.
		// [orig: Server_BuildEndOfRoundScoreboard @0x5090D3..@0x509116]
		row.name = capped(player.identity.name, 31);
		row.clan.clear();
		row.tag = capped(player.identity.tag, 15);
		row.team = player.team;
		row.player_class = player.player_class;
		// Exact producer wire order. The decode POD's names predate the write-side
		// recovery and are intentionally not a second stats schema:
		// primary, raw29, raw30, raw5, raw7, raw11, (raw2<<16)/raw5.
		// [orig: Server_BuildEndOfRoundScoreboard @0x508F30]
		row.kills = wire_i16(player.primary_score);
		row.deaths = wire_i16(player.stats[world::MatchStats::kPoints]);
		row.assists = wire_i16(player.stats[30]);
		row.score = wire_i16(player.stats[world::MatchStats::kEnemyKills]);
		row.captures = wire_i16(player.stats[world::MatchStats::kDeaths]);
		row.flags = wire_i16(player.stats[world::MatchStats::kFlagSaves]); // raw11 = FLAGSAVE
		row.special = scoreboard_ratio(player.stats);
		row.per_team.reserve(active_fields.size());
		for (const size_t index : active_fields) {
			const world::MatchScoreField &field = result.score_fields[index];
			row.per_team.push_back(wire_i16(world::match_score_field_value(
					player.stats, field.field, result.game_type)));
		}
		out.players.push_back(std::move(row));
	}
	const size_t team_row_count = std::min<size_t>(
			result.team_row_count, result.team_stats.size());
	out.team_rows.resize(team_row_count);
	for (size_t team = 0; team < team_row_count; ++team) {
		out.team_rows[team].reserve(active_fields.size());
		for (const size_t index : active_fields) {
			const world::MatchScoreField &field = result.score_fields[index];
			out.team_rows[team].push_back(wire_i16(
					world::match_score_field_value(
							result.team_stats[team], field.field,
							result.game_type)));
		}
	}
	return out;
}

EndRoundHeader build_end_round_header(const world::MatchResult &result,
		uint8_t recipient_slot, bool non_team_form) {
	EndRoundHeader out;
	out.winner_team = static_cast<int8_t>(result.winner_team);
	out.team_score_0 = wire_i16(result.team_scores[0]);
	out.team_score_1 = wire_i16(result.team_scores[1]);
	out.draw = result.draw ? 1u : 0u;
	if (non_team_form) {
		// The non-team form serializes the top three rows of the frozen
		// (points-descending) board: the 32-byte entry name and the
		// game-type primary score the board builder selected into
		// entry+0x40 (the ScoreRules_GetPrimaryScoreField (ex sub_52C850)(g_GameType, ...) store @0x509152).
		// [orig: EndRoundScoreboard_SerializeHeader @0x5052bf..0x505381,
		// reading entry+4 / entry+0x40 of rows 0..2 @0x24C1A94]
		for (size_t i = 0; i < 3 && i < result.players.size(); ++i) {
			out.player_names[i] = capped(result.players[i].identity.name, 31);
			out.player_scores[i] = wire_i16(result.players[i].primary_score);
		}
	}
	for (size_t i = 0; i < result.players.size(); ++i) {
		if (result.players[i].identity.slot != recipient_slot) continue;
		out.player_index = i <= size_t(std::numeric_limits<int8_t>::max())
				? static_cast<int8_t>(i)
				: int8_t{-1};
		break;
	}
	return out;
}

} // namespace opennova::np
