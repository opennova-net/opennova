#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace opennova::hud {

// THE TOGGLED END-ROUND STATISTICS PANEL — retail's SP "Show Score" screen.
//
// One titled label box with four label/value rows: subgoals won over subgoals
// defined, enemy units killed over the mission's enemy-unit total, and the
// two friendly-fire unit counts. Drawn every HUD frame while the ShowScore
// toggle is set; the toggle is settable only OUTSIDE a session (SP), flips
// exclusively (the respawn-init wrapper clears every overlay toggle, then
// restores this one), and clears with the respawn init.
// [orig: HUD_DrawEndRoundStatistics @0x5b7600, called from the frame drawer
//  HUD_DrawOverlayPanels @0x5c0092 while dword_24C18AC is set; the toggle is
//  Input_HandleActionBinding jumptable case 422 @0x49bd29 (gated
//  !is_in_session) -> Game_InitRespawnStateKeepingToggle @0x4993c0 (&toggle) = Game_InitRespawnState keeping
//  *ptr @0x4993c0..0x4993db; cleared by Game_InitRespawnState @0x499381 and
//  Game_DestroyAllEntitiesAndReset @0x5235c5. Controls catalog row 99
//  "ShowScore" ("!Show Score"), default VK 0x74 = F5, handler id 422 at
//  record +40.]
//
// Retail also sprintf's a SECOND value per row (the subgoal bonus score, the
// damage-received sum, and the two by-others point sums) into the same stack
// buffer WITHOUT a draw call between format and the next row — dead stores,
// witnessed at @0x5b76ea/@0x5b77a0/@0x5b7823/@0x5b78a6 (no HUD_DrawTextLeftScaled (ex sub_580B40)/BC0
// between them and the next label). They are not modeled.

// The label box, design space (x1, y1, x2, y2 like every HUD_DrawLabelBox
// call): x 128..896, 340 tall from the panel top [orig: the call @0x5b7671 —
// (128, top, 896, bottom - 140) with top/bottom 280/760, or 140/620 when
// g_spawn_success_gate && g_endround_winner_team == 1 @0x5b763b..0x5b7644].
inline constexpr int kEndRoundStatsBoxX1 = 128;
inline constexpr int kEndRoundStatsBoxX2 = 896;
inline constexpr int kEndRoundStatsBoxHeight = 340;
inline constexpr int kEndRoundStatsTop = 280;
inline constexpr int kEndRoundStatsTopRaised = 140;
// Rows start 48 below the panel top and step 48 [orig: @0x5b7653/@0x5b7712].
inline constexpr int kEndRoundStatsRowStart = 48;
inline constexpr int kEndRoundStatsRowStep = 48;
// Labels left-aligned at x 200 (HUD_DrawTextLeftScaled -> HUD_DrawTextLeft_HalfBright),
// values RIGHT-aligned at x 620 (HUD_DrawTextRightAlignedScaled (ex sub_580BC0) ->
// HUD_DrawTextRightAligned_HalfBright) [orig: @0x5b76ad/@0x5b76d7].
inline constexpr int kEndRoundStatsLabelX = 200;
inline constexpr int kEndRoundStatsValueX = 620;

// The counters the four rows consume — the 0xC846xx SP stat block
// (world::MissionKillStats plus the subgoal pair).
struct EndRoundStatisticsInput {
	// Raised box: the between-rounds gate with a team-1 (player) win
	// [orig: g_spawn_success_gate && g_endround_winner_team == 1 @0x5b763b].
	bool raised = false;
	int32_t subgoals_won = 0;        // [orig: 0xC846D0 — one per first SubGoalWon]
	int32_t subgoals_defined = 0;    // [orig: 0xC8468C — the leading win-condition scan]
	int32_t enemy_kills = 0;         // [orig: 0xC846A8+B0+B8+D8+E0+E8 folded @0x5b771b]
	int32_t enemy_unit_total = 0;    // [orig: 0xC84690 — the mission-start classify scan]
	int32_t team_unit_kills = 0;     // [orig: 0xC846F0 + 0xC846C0 @0x5b77bd]
	int32_t friendly_unit_kills = 0; // [orig: 0xC846F8 + 0xC846C8 @0x5b783c]
};

// One row: the gametext label key (section "Epilog") and the formatted value.
struct EndRoundStatisticsRow {
	const char *label_key = "";
	std::string value;
};

inline int end_round_statistics_top(bool raised) {
	return raised ? kEndRoundStatsTopRaised : kEndRoundStatsTop;
}

// The four rows in retail draw order. The title is gametext
// ("Score", "SCORE_TITLE") [orig: @0x5b7656]; the labels resolve from
// gametext section "Epilog" [orig: @0x5b7699/@0x5b774f/@0x5b77d6/@0x5b7859].
inline std::array<EndRoundStatisticsRow, 4> end_round_statistics_rows(
		const EndRoundStatisticsInput &in) {
	char buf[32];
	std::array<EndRoundStatisticsRow, 4> rows;
	rows[0].label_key = "STREPILOG_OBJECTIVEBONUS";
	std::snprintf(buf, sizeof buf, "%d/%d", in.subgoals_won,
			in.subgoals_defined);
	rows[0].value = buf;
	// Enemy kills clamp to [0, total] before the draw [orig: @0x5b7721..0x5b772b].
	rows[1].label_key = "STREPILOG_ENEMYUNITS";
	const int32_t enemy = std::clamp(in.enemy_kills, int32_t{0},
			std::max(in.enemy_unit_total, int32_t{0}));
	std::snprintf(buf, sizeof buf, "%d/%d", enemy, in.enemy_unit_total);
	rows[1].value = buf;
	rows[2].label_key = "STREPILOG_TEAMUNITS";
	std::snprintf(buf, sizeof buf, "%d", in.team_unit_kills);
	rows[2].value = buf;
	rows[3].label_key = "STREPILOG_FRIENDLYUNITS";
	std::snprintf(buf, sizeof buf, "%d", in.friendly_unit_kills);
	rows[3].value = buf;
	return rows;
}


// The SP Show Score panel as one value (the embedder's record wraps it by
// value): the raised-box gate and the four composed rows in draw order.
struct EndRoundStatisticsPanel {
	bool raised = false;
	std::vector<EndRoundStatisticsRow> rows;
};

} // namespace opennova::hud
