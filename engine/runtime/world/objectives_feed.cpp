#include <runtime/world/objectives_feed.h>

#include <runtime/world/world.h>

#include <cstdio>
#include <utility>

namespace opennova::world {

void fill_objective_rows(const World &world, const hud::GameTextLookup &mission_text,
		std::vector<hud::HudObjectiveRow> &r_rows) {
	// The SP objectives panel's row walk: slots 1..8 until a 0/255 win id.
	// [orig: HUD_DrawWinConditions @0x5ba9e0 — byte_A7628B[slot] 0/255 break;
	//  row gate = show-win bit @0x5ba9ff; checkmark = won bit @0x5bab35]
	// The panel's resolved rows: the shown win-condition slots with their
	// mission-text lines and completed state (an empty row set hides the
	// panel — the retail toggle's off state). [orig: HUD_DrawWinConditions
	// @0x5ba940 — rows from the header table walk, text = mission
	// WinConditions/STRWINCOND%03i]
	r_rows.clear();
	const SubgoalState &sg = world.script.subgoals;
	for (int slot = 1; slot <= 8; ++slot) {
		const uint8_t id = sg.win_text_ids[slot];
		if (id == 0 || id == 255) break;
		if ((sg.show_win & (1u << slot)) == 0) continue;
		hud::HudObjectiveRow row;
		char key[32];
		std::snprintf(key, sizeof(key), "STRWINCOND%03d", static_cast<int>(id));
		row.text = hud::game_text(mission_text, "WinConditions", key, "");
		row.done = (sg.won & (1u << slot)) != 0;
		r_rows.push_back(std::move(row));
	}
}

hud::EndRoundStatisticsInput end_round_statistics_input(const World &world) {
	const MissionKillStats &ks = world.kill_stats;
	hud::EndRoundStatisticsInput in;
	in.subgoals_won = ks.subgoals_won;                  // [orig: 0xC846D0]
	in.subgoals_defined = count_defined_subgoals(world); // [orig: 0xC8468C]
	// The six enemy buckets folded [orig: @0x5b771b / @0x576658].
	in.enemy_kills = ks.enemy_kills_by_player() + ks.enemy_kills_by_others;
	in.enemy_unit_total = ks.enemy_unit_total;           // [orig: 0xC84690]
	in.team_unit_kills = ks.bluekills_by_player + ks.team_kills_by_others;         // @0x5b77bd
	in.friendly_unit_kills = ks.greenkills_by_player + ks.friendly_kills_by_others; // @0x5b783c
	in.raised = world.match.outcome().ended && world.match.outcome().winner_team == 1;
	return in;
}

} // namespace opennova::world
