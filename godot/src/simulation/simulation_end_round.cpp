// Simulation — the end-of-round presentation feed (net-re §5.68): the 0x1D
// header edge and the reassembled 0x56 board read by BOTH roles through the
// one ClientEndRoundStats the replica pipeline folds (the listen host's own
// loopback view folds the same messages), turned into the overlay text ladder
// (hud/end_round_overlay.h) and the stat.mnu RESULTLIST feed
// (npruntime/stat_screen_feed.h).
#include "simulation/simulation_internal.h"
#include "simulation/hud_view_records.h"
#include "simulation/end_round_state.h"

#include "rtxt/rtxt_string_file.h"

#include <runtime/hud/end_round_overlay.h>
#include <runtime/hud/end_round_statistics.h>
#include <runtime/hud/feed_format.h>
#include <runtime/inmatch/stat_screen_feed.h>
#include <base/gameprofile/game_type.h>

#include <algorithm>
#include <functional>
#include <string>

using namespace godot;

namespace {

// GameText_GetString over the Overlays section: present -> true + value (may
// be empty), missing -> false. A null table resolves nothing.
opennova::hud::EndRoundTextLookup overlays_lookup(const Ref<RtxtStringFile> &gametext) {
	return [gametext](const std::string &key, std::string &value) {
		if (gametext.is_null()) return false;
		const String k = String::utf8(key.c_str());
		if (!gametext->has_string_in_section("Overlays", StringName(k))) return false;
		value = gametext->get_string_in_section("Overlays", StringName(k)).utf8().get_data();
		return true;
	};
}

// The stat.mnu column header: a keyed header resolves like the ladder
// (present + non-empty, else the "!..." fallback stripped); the key-less NAME
// / Squad columns show their fallback stripped.
String resolve_column_header(const opennova::hud::EndRoundTextLookup &lookup,
		const opennova::inmatch::StatScreenColumn &c) {
	std::string text = c.header_fallback;
	if (!c.header_key.empty()) {
		std::string value;
		if (lookup(c.header_key, value) && !value.empty()) return String::utf8(value.c_str());
	}
	if (!text.empty() && text[0] == '!') text.erase(0, 1);
	return String::utf8(text.c_str());
}

} // namespace

Ref<EndRoundState> Simulation::get_end_round_state() const {
	// [orig: the S2C 0x1D landing NapiNPClientMsg_0x01D @0x430840 —
	// g_spawn_success_gate, g_endround_winner_team, g_scoreTeamScore0/1,
	// g_endround_draw_flag, dword_A81B2C = GetTickCount; the 0x56 board
	// completion.] Role-agnostic: every role's view folds both lanes.
	Ref<EndRoundState> record;
	record.instantiate();
	opennova::inmatch::EndRoundSessionState v;
	if (runtime_) {
		const opennova::replication::ClientEndRoundStats &er = runtime_->state().end_round;
		v.header_known = er.header_known;
		v.board_known = er.known;
		v.game_type = runtime_->game_type();
		v.winner = static_cast<int>(er.header.winner_team);
		v.team_score_0 = static_cast<int>(er.header.team_score_0);
		v.team_score_1 = static_cast<int>(er.header.team_score_1);
		v.draw = er.header.draw != 0;
		v.my_index = static_cast<int>(er.header.player_index);
		// The authority reads its own Match clock; a joiner reads the folded
		// 0x0A sub-block-1 copy. [orig: g_round_time_remaining @0x24C1958,
		// the joiner store @0x430219..0x430235]
		const int32_t remaining = joiner_
				? runtime_->state().round_time_remaining_ticks
				: (kernel_ ? kernel_->world.match.remaining_ticks() : -1);
		v.round_ticks = std::max(0, remaining);
		v.death_screen = local_death_screen_active();
		v.local_team = static_cast<int>(runtime_->assigned_team());
		// The team-mode arm stat.mnu's RADIO_TAB_* trio rides (the g_GameType
		// 0x10000 bit, base/gameprofile/game_type.h; the show callback's witness is
		// stat_screen_feed.h's).
		v.team_mode = opennova::game_type::is_team(runtime_->game_type());
		// The round-cycle handoff's session half: the host's post-round linger
		// expiry closes the session [orig: Server_TickUpdate's drain sets
		// g_mission_exit_reason = 3 @0x51db63 — the map cycle]; a joiner's
		// session dies with the host's exit.
		v.session_open = joiner_ ? !runtime_->session_lost() : ctx_.is_in_session != 0;
	}
	record->assign(v);
	return record;
}

bool Simulation::is_mp_session() const {
	return kernel_ != nullptr && kernel_->world.rules.mp_session;
}

opennova::hud::EndRoundOverlayInput Simulation::end_round_overlay_input() const {
	opennova::hud::EndRoundOverlayInput in;
	if (!runtime_) return in;
	const opennova::replication::ClientEndRoundStats &er = runtime_->state().end_round;
	in.game_type = runtime_->game_type();
	in.draw = er.header.draw != 0;
	in.winner_team = er.header.winner_team;
	in.local_team = runtime_->assigned_team();
	in.death_screen = local_death_screen_active();
	in.team_scores[0] = er.header.team_score_0;
	in.team_scores[1] = er.header.team_score_1;
	// The non-team 0x1D form's three named players + primary scores; empty
	// names take the ladder's name-less arms. [orig: the 0x1D commit
	// @0x430a70..0x430abb into byte_24C1A98/B7C/C60 + dword_24C1AD4/BB8/C9C]
	for (int i = 0; i < 3; ++i) {
		in.player_names[i] = er.header.player_names[i];
		in.player_scores[i] = er.header.player_scores[i];
	}
	// Authority: the Match clock; joiner: the folded 0x0A sub-block-1 copy
	// [orig: g_round_time_remaining @0x24C1958 — the game-time line and the
	// timed/untimed arm picks read it on every role].
	in.round_time_remaining_ticks = std::max(0, joiner_
			? runtime_->state().round_time_remaining_ticks
			: (kernel_ ? kernel_->world.match.remaining_ticks() : -1));
	return in;
}

Ref<EndRoundOverlay> Simulation::get_end_round_overlay(const Ref<RtxtStringFile> &p_gametext) const {
	// The resolved ladder: keys through the gametext Overlays table, the
	// empty-resolve folds and the printf forms applied by the engine
	// (hud::end_round_overlay_resolve), plus the design-space safe area.
	opennova::hud::EndRoundOverlayLadder ladder;
	if (runtime_ && runtime_->state().end_round.header_known) {
		ladder.lines = opennova::hud::end_round_overlay_resolve(
				opennova::hud::end_round_overlay_lines(end_round_overlay_input()),
				overlays_lookup(p_gametext));
	}
	Ref<EndRoundOverlay> out;
	out.instantiate();
	out->assign(ladder);
	return out;
}

int Simulation::end_round_stat_screen_delay_msec() {
	return opennova::hud::kEndRoundStatScreenDelayMsec;
}

String Simulation::strip_inline_tags(const String &p_text) {
	return String::utf8(opennova::hud::strip_inline_tags(p_text.utf8().get_data()).c_str());
}

TypedArray<EndRoundColumn> Simulation::get_end_round_columns(int p_table_width,
		const Ref<RtxtStringFile> &p_gametext) const {
	TypedArray<EndRoundColumn> out;
	if (!runtime_) return out;
	const opennova::replication::ClientEndRoundStats &er = runtime_->state().end_round;
	if (!er.known) return out;
	const opennova::hud::EndRoundTextLookup lookup = overlays_lookup(p_gametext);
	for (opennova::inmatch::StatScreenColumn c :
			opennova::inmatch::stat_screen_columns(er.board, false, p_table_width)) {
		c.header = resolve_column_header(lookup, c).utf8().get_data();
		Ref<EndRoundColumn> column;
		column.instantiate();
		column->assign(c);
		out.push_back(column);
	}
	return out;
}

TypedArray<EndRoundRow> Simulation::get_end_round_rows(int p_tab) const {
	// The PLAYER SLOT table retail walks is the roster every role's view
	// folds from 0x46 (name / clan / team). The local row comes from the 0x1D
	// header's board index, resolved to a connection slot below.
	TypedArray<EndRoundRow> out;
	if (!runtime_) return out;
	const opennova::replication::ClientState &cs = runtime_->state();
	if (!cs.end_round.known) return out;
	std::vector<opennova::inmatch::StatScreenPlayer> players;
	for (size_t i = 0; i < cs.roster.size(); ++i) {
		const opennova::replication::ClientRosterSlot &slot = cs.roster[i];
		if (!slot.bound) continue;
		opennova::inmatch::StatScreenPlayer p;
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
	// [orig: populate_stat_results_list @0x562240 — row slot store @0x562576,
	//  board join @0x5624F3, selection compare @0x56272E]
	int local_slot = -1;
	const int8_t header_index = cs.end_round.header.player_index;
	if (header_index >= 0 &&
			static_cast<size_t>(header_index) <
					cs.end_round.board.players.size()) {
		local_slot = cs.end_round.board.players[header_index].slot;
	}
	for (const opennova::inmatch::StatScreenRow &r :
			opennova::inmatch::stat_screen_rows(cs.end_round.board, players, false, local_slot)) {
		// The tab filter (the engine's stat_screen_row_visible; its witness is
		// stat_screen_feed.h's).
		if (!opennova::inmatch::stat_screen_row_visible(p_tab, r.team)) continue;
		Ref<EndRoundRow> row;
		row.instantiate();
		row->assign(r);
		out.push_back(row);
	}
	return out;
}


Ref<EndRoundStatistics> Simulation::get_end_round_statistics() const {
	// The SP Show Score panel's counters — the 0xC846xx stat block
	// (hud/end_round_statistics.h documents the rows). Host-world data only:
	// the panel's toggle is settable only outside a session, and a joiner has
	// no tally world. [orig: HUD_DrawEndRoundStatistics @0x5b7600 reads the
	// block; the toggle gate @0x49bd29 — see net-re §5.68]
	if (kernel_ == nullptr) return Ref<EndRoundStatistics>();
	const opennova::world::World &w = kernel_->world;
	opennova::hud::EndRoundStatisticsInput in;
	int32_t won = 0;
	for (uint32_t mask = w.script.subgoals.won; mask != 0; mask &= mask - 1) ++won;
	in.subgoals_won = won; // [orig: 0xC846D0 — one per first SubGoalWon @0x4fd117]
	in.subgoals_defined = opennova::world::count_defined_subgoals(w);
	in.enemy_kills = w.kill_stats.enemy_kills_by_player +
			w.kill_stats.enemy_kills_by_others; // the six buckets folded @0x5b771b
	in.enemy_unit_total = w.kill_stats.enemy_unit_total;
	in.team_unit_kills = w.kill_stats.bluekills_by_player +
			w.kill_stats.team_kills_by_others; // @0x5b77bd
	in.friendly_unit_kills = w.kill_stats.greenkills_by_player +
			w.kill_stats.friendly_kills_by_others; // @0x5b783c
	// The raised box: the between-rounds gate with a team-1 win
	// [orig: g_spawn_success_gate && g_endround_winner_team == 1 @0x5b763b].
	in.raised = w.match.outcome().ended && w.match.outcome().winner_team == 1;
	opennova::hud::EndRoundStatisticsPanel panel;
	panel.raised = in.raised;
	for (const opennova::hud::EndRoundStatisticsRow &row :
			opennova::hud::end_round_statistics_rows(in)) {
		panel.rows.push_back(row);
	}
	Ref<EndRoundStatistics> out;
	out.instantiate();
	out->assign(panel);
	return out;
}
