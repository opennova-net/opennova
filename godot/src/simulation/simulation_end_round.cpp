// Simulation — the end-of-round presentation feed (net-re §5.68): the 0x1D
// header edge and the reassembled 0x56 board read by BOTH roles through the
// one ClientEndRoundStats the replica pipeline folds (the listen host's own
// loopback view folds the same messages), turned into the overlay text ladder
// (hud/end_round_overlay.h) and the stat.mnu RESULTLIST feed
// (runtime/inmatch/stat_screen_feed.h).
#include "simulation/simulation_internal.h"
#include "simulation/hud_view_records.h"
#include "simulation/end_round_state.h"

#include "rtxt/rtxt_string_file.h"
#include "util/string_convert.h"

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
		const String k = opennova::to_gd(key);
		if (!gametext->has_string_in_section("Overlays", StringName(k))) return false;
		value = opennova::to_std(gametext->get_string_in_section("Overlays", StringName(k)));
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
		if (lookup(c.header_key, value) && !value.empty()) return opennova::to_gd(value);
	}
	if (!text.empty() && text[0] == '!') text.erase(0, 1);
	return opennova::to_gd(text);
}

} // namespace

Ref<EndRoundState> Simulation::get_end_round_state() const {
	// The folded 0x1D / 0x56 lanes every role's view carries (inmatch/role_feeds.h).
	Ref<EndRoundState> record;
	record.instantiate();
	record->assign(opennova::inmatch::end_round_session_state(role_view()));
	return record;
}

bool Simulation::is_mp_session() const {
	return kernel_ != nullptr && kernel_->world.rules.mp_session;
}

opennova::hud::EndRoundOverlayInput Simulation::end_round_overlay_input() const {
	return opennova::inmatch::end_round_overlay_input(role_view());
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

String Simulation::strip_inline_tags(const String &p_text) {
	return opennova::to_gd(opennova::hud::strip_inline_tags(p_text.utf8().get_data()));
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
		c.header = opennova::to_std(resolve_column_header(lookup, c));
		Ref<EndRoundColumn> column;
		column.instantiate();
		column->assign(c);
		out.push_back(column);
	}
	return out;
}

TypedArray<EndRoundRow> Simulation::get_end_round_rows(int p_tab) const {
	// The roster joined to the frozen board, the tab filter applied
	// (inmatch/role_feeds.h end_round_rows carries the witnesses).
	TypedArray<EndRoundRow> out;
	for (const opennova::inmatch::StatScreenRow &r :
			opennova::inmatch::end_round_rows(role_view(), p_tab)) {
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
	// [orig: g_SpawnSuccessGate && g_EndRoundWinnerTeam == 1 @0x5b763b].
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
