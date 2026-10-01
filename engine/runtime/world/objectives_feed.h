#pragma once

// The SP objectives panel's row feed: the shown win-condition slots with
// their mission-text lines and completed state, built from the world's
// subgoal state (world.h SubgoalState) through the game-text seam. An empty
// row set hides the panel — the retail toggle's off state. Beside it, the SP
// score block's feed for the two screens that draw the objectives' tally.

#include <runtime/hud/end_round_statistics.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_frame.h>

#include <vector>

namespace opennova::world {

class World;

// The row walk: slots 1..8 until a 0/255 win id, a row per slot whose
// show-win bit is set, text = mission WinConditions/STRWINCOND%03i (empty
// when the key is absent), done = the won bit.
// [orig: HUD_DrawWinConditions @0x5ba940 / @0x5ba9e0 — byte_A7628B[slot]
//  0/255 break; row gate = show-win bit @0x5ba9ff; checkmark = won bit
//  @0x5bab35]
void fill_objective_rows(const World &world, const hud::GameTextLookup &mission_text,
		std::vector<hud::HudObjectiveRow> &r_rows);

// The SP score block's counters as the Show Score panel and the win epilog
// read them: the won tally over the defined subgoals, the six enemy kill
// buckets folded over the census total, and the two unit sums; raised = the
// between-rounds gate with a team-1 win (the panel's box only).
// [orig: HUD_DrawEndRoundStatistics @0x5b7676..0x5b783c;
//  Cine_EpilogStateMachineUpdate @0x5765d0..0x57672d; the raised gate
//  g_SpawnSuccessGate && g_EndRoundWinnerTeam == 1 @0x5b763b]
hud::EndRoundStatisticsInput end_round_statistics_input(const World &world);

} // namespace opennova::world
