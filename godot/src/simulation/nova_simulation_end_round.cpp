// Simulation — the end-of-round presentation feed (net-re §5.68): the 0x1D
// header edge and the reassembled 0x56 board read by BOTH roles through the
// one ClientEndRoundStats the replica pipeline folds (the listen host's own
// loopback view folds the same messages), turned into the overlay text ladder
// (hud/end_round_overlay.h) and the stat.mnu RESULTLIST feed
// (npruntime/stat_screen_feed.h).
#include "simulation/nova_simulation_internal.h"

#include <hud/end_round_overlay.h>
#include <hud/end_round_statistics.h>
#include <npruntime/stat_screen_feed.h>

#include <algorithm>

using namespace godot;

namespace {

Dictionary arg_to_dict(const opennova::hud::EndRoundArg &a) {
	Dictionary d;
	d["key"] = String::utf8(a.key.c_str());
	d["fallback"] = String::utf8(a.fallback.c_str());
	d["literal"] = String::utf8(a.literal.c_str());
	d["number"] = a.number;
	d["is_number"] = a.is_number;
	return d;
}

} // namespace

Dictionary Simulation::get_end_round_state() const {
	// (retail: the S2C 0x1D landing NapiNPClientMsg_0x01D @0x430840 —
	// g_spawn_success_gate, g_endround_winner_team, g_scoreTeamScore0/1,
	// g_endround_draw_flag, dword_A81B2C = GetTickCount; the 0x56 board
	// completion.) Role-agnostic: every role's view folds both lanes.
	Dictionary out;
	if (!runtime_) return out;
	const opennova::netsim::ClientEndRoundStats &er = runtime_->state().end_round;
	out["header_known"] = er.header_known;
	out["board_known"] = er.known;
	out["game_type"] = static_cast<int64_t>(runtime_->game_type());
	out["winner"] = static_cast<int>(er.header.winner_team);
	Array scores;
	scores.push_back(static_cast<int>(er.header.team_score_0));
	scores.push_back(static_cast<int>(er.header.team_score_1));
	out["team_scores"] = scores;
	out["draw"] = er.header.draw != 0;
	out["my_index"] = static_cast<int>(er.header.player_index);
	// The authority keeps its own round clock; the joiner's copy of
	// g_round_time_remaining (the 0x0A sub-block-1 timer) is not folded yet.
	int32_t remaining = 0;
	if (world_ && !joiner_) remaining = world_->match.remaining_ticks();
	out["round_ticks"] = std::max(0, remaining);
	out["death_screen"] = local_death_screen_active();
	out["local_team"] = static_cast<int>(runtime_->assigned_team());
	return out;
}

TypedArray<Dictionary> Simulation::get_end_round_lines() const {
	// The overlay text ladder (retail: draw_endround_stats_overlay @0x5b7cd0, see hud/end_round_overlay.h):
	// {key, fallback, literal, args[{key, fallback, literal, number,
	// is_number}], y} per line; the presenter resolves Overlays/<key>.
	TypedArray<Dictionary> out;
	if (!runtime_) return out;
	const opennova::netsim::ClientEndRoundStats &er = runtime_->state().end_round;
	if (!er.header_known) return out;
	opennova::hud::EndRoundOverlayInput in;
	in.game_type = runtime_->game_type();
	in.draw = er.header.draw != 0;
	in.winner_team = er.header.winner_team;
	in.local_team = runtime_->assigned_team();
	in.death_screen = local_death_screen_active();
	in.team_scores[0] = er.header.team_score_0;
	in.team_scores[1] = er.header.team_score_1;
	// The non-team 0x1D form's three named players + primary scores; empty
	// names take the ladder's name-less arms. (retail: the 0x1D commit
	// @0x430a70..0x430abb into byte_24C1A98/B7C/C60 + dword_24C1AD4/BB8/C9C)
	for (int i = 0; i < 3; ++i) {
		in.player_names[i] = er.header.player_names[i];
		in.player_scores[i] = er.header.player_scores[i];
	}
	if (world_ && !joiner_) in.round_time_remaining_ticks = std::max(0, world_->match.remaining_ticks());
	for (const opennova::hud::EndRoundLine &line : opennova::hud::end_round_overlay_lines(in)) {
		Dictionary d;
		d["key"] = String::utf8(line.key.c_str());
		d["fallback"] = String::utf8(line.fallback.c_str());
		d["literal"] = String::utf8(line.literal.c_str());
		Array args;
		for (const opennova::hud::EndRoundArg &a : line.args) args.push_back(arg_to_dict(a));
		d["args"] = args;
		d["y"] = line.y;
		// The empty-resolve fold the presenter applies (see
		// hud::EndRoundEmptyFold): 1 = headline STROVER1 re-lookup,
		// 2 = collapse the line and shift the ladder below it up 32 px.
		d["fold"] = static_cast<int>(line.fold);
		out.push_back(d);
	}
	return out;
}

TypedArray<Dictionary> Simulation::get_end_round_columns(int p_table_width) const {
	TypedArray<Dictionary> out;
	if (!runtime_) return out;
	const opennova::netsim::ClientEndRoundStats &er = runtime_->state().end_round;
	if (!er.known) return out;
	for (const opennova::np::StatScreenColumn &c :
			opennova::np::stat_screen_columns(er.board, false, p_table_width)) {
		Dictionary d;
		d["header_key"] = String::utf8(c.header_key.c_str());
		d["header_fallback"] = String::utf8(c.header_fallback.c_str());
		d["literal"] = String::utf8(c.literal.c_str());
		d["width"] = c.width;
		d["field_id"] = c.field_id;
		out.push_back(d);
	}
	return out;
}

TypedArray<Dictionary> Simulation::get_end_round_rows() const {
	// The PLAYER SLOT table retail walks is the roster every role's view
	// folds from 0x46 (name / clan / team). The local row comes from the 0x1D
	// header's board index, resolved to a connection slot below.
	TypedArray<Dictionary> out;
	if (!runtime_) return out;
	const opennova::netsim::ClientState &cs = runtime_->state();
	if (!cs.end_round.known) return out;
	std::vector<opennova::np::StatScreenPlayer> players;
	for (size_t i = 0; i < cs.roster.size(); ++i) {
		const opennova::netsim::ClientRosterSlot &slot = cs.roster[i];
		if (!slot.bound) continue;
		opennova::np::StatScreenPlayer p;
		p.slot = static_cast<uint8_t>(i);
		p.team = slot.team;
		p.name = slot.name;
		p.squad = slot.clan;
		players.push_back(p);
	}
	// The header's player_index is the recipient's index into the FROZEN
	// (points-descending) board array, not a connection slot. Retail joins the
	// board by the row's stored slot id and highlights the row whose slot
	// matches board[player_index].slot; the same fold works for both roles
	// because the listen host consumes its own loopback 0x1D.
	// (retail: populate_stat_results_list @0x562240 — row slot store @0x562576,
	//  board join @0x5624F3, selection compare @0x56272E)
	int local_slot = -1;
	const int8_t header_index = cs.end_round.header.player_index;
	if (header_index >= 0 &&
			static_cast<size_t>(header_index) <
					cs.end_round.board.players.size()) {
		local_slot = cs.end_round.board.players[header_index].slot;
	}
	for (const opennova::np::StatScreenRow &r :
			opennova::np::stat_screen_rows(cs.end_round.board, players, false, local_slot)) {
		Dictionary d;
		d["slot"] = static_cast<int>(r.slot);
		d["team"] = static_cast<int>(r.team);
		d["name"] = String::utf8(r.name.c_str());
		d["squad"] = String::utf8(r.squad.c_str());
		PackedStringArray cells;
		for (const std::string &c : r.cells) cells.push_back(String::utf8(c.c_str()));
		d["cells"] = cells;
		d["color"] = static_cast<int64_t>(r.color_argb);
		d["selected"] = r.selected;
		out.push_back(d);
	}
	return out;
}


Dictionary Simulation::get_end_round_statistics() const {
	// The SP Show Score panel's counters — the 0xC846xx stat block
	// (hud/end_round_statistics.h documents the rows). Host-world data only:
	// the panel's toggle is settable only outside a session, and a joiner has
	// no tally world. (retail: HUD_DrawEndRoundStatistics @0x5b7600 reads the
	// block; the toggle gate @0x49bd29 — see net-re §5.68)
	Dictionary out;
	if (world_ == nullptr) return out;
	const opennova::world::World &w = *world_;
	opennova::hud::EndRoundStatisticsInput in;
	int32_t won = 0;
	for (uint32_t mask = w.subgoals.won; mask != 0; mask &= mask - 1) ++won;
	in.subgoals_won = won; // (retail: 0xC846D0 — one per first SubGoalWon @0x4fd117)
	in.subgoals_defined = opennova::world::count_defined_subgoals(w);
	in.enemy_kills = w.kill_stats.enemy_kills_by_player +
			w.kill_stats.enemy_kills_by_others; // the six buckets folded @0x5b771b
	in.enemy_unit_total = w.kill_stats.enemy_unit_total;
	in.team_unit_kills = w.kill_stats.bluekills_by_player +
			w.kill_stats.team_kills_by_others; // @0x5b77bd
	in.friendly_unit_kills = w.kill_stats.greenkills_by_player +
			w.kill_stats.friendly_kills_by_others; // @0x5b783c
	// The raised box: the between-rounds gate with a team-1 win
	// (retail: g_spawn_success_gate && g_endround_winner_team == 1 @0x5b763b).
	in.raised = w.match.outcome().ended && w.match.outcome().winner_team == 1;
	out["raised"] = in.raised;
	TypedArray<Dictionary> rows;
	for (const opennova::hud::EndRoundStatisticsRow &row :
			opennova::hud::end_round_statistics_rows(in)) {
		Dictionary d;
		d["label_key"] = String::utf8(row.label_key);
		d["value"] = String::utf8(row.value.c_str());
		rows.push_back(d);
	}
	out["rows"] = rows;
	return out;
}
