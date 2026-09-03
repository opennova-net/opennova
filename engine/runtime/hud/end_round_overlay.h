#pragma once

// THE END-OF-ROUND OVERLAY (net-re §5.68, S2C 0x1D): the Impact38 text ladder
// retail draws every HUD frame from the round-end announcement until stat.mnu
// opens — the headline, the game-type second line, the score lines and the
// game-time line — plus the stat-field column layout of the first pass.
// The key selection here is the exact port of draw_endround_stats_overlay
// @0x5b7cd0; the strings themselves are the embedder's gametext Overlays
// table, so each line carries its KEY + fallback and the printf-style
// arguments, and the presenter resolves them.
// [orig: UI_ProcessEndRoundScreenTransition @0x5b8600 (called every HUD frame
//  while g_spawn_success_gate && is_in_session from HUD_DrawOverlayPanels @0x5c0072):
//  first pass Server_ResetBalanceCounters + Game_InitRespawnState +
//  Overlay_ComputeStatFieldColumnLayout(40, 984) + byte_28E561C = 1; every
//  pass UI_TeardownScene (ex sub_54E650) (the UI scene teardown) then draw_endround_stats_overlay
//  @0x5b7cd0; once g_scoreboardDirty && now - t0 >= 6000 ms ->
//  UI_OpenMenuScreen("stat.mnu", "STAT") once (byte_28E561D)]

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <utility>

namespace opennova::hud {

// One printf argument of a line: a gametext key (with its fallback) or a
// literal string, or an integer.
struct EndRoundArg {
	std::string key;      // Overlays/<key> when set
	std::string fallback; // the "!..." fallback retail passes, else ""
	std::string literal;  // used when key is empty
	int32_t number = 0;
	bool is_number = false;
};

// What the presenter does when this line's key resolves to an EMPTY string
// (present-but-empty gametext; the fallback covers only a missing key):
// the headline re-looks-up Overlays/STROVER1 with the "!Mission Completed"
// fallback, and the second line is dropped with every later line moving up
// 32 px (retail's y stays 350, so the score lines start at 382).
// [orig: draw_endround_stats_overlay @0x5B7CD0 — the empty-headline test
//  @0x5b7e59 -> GameText_GetStringWithFallback @0x5b7e5b; the empty-second-line
//  test @0x5b83e5 — draw @0x5b83fe / y=382 store @0x5b8406 only when
//  text_buf[0]]
enum class EndRoundEmptyFold : uint8_t {
	kNone = 0,
	kHeadlineStrover1 = 1,
	kCollapse = 2,
};

struct EndRoundLine {
	// The line's format: a gametext key (Overlays section) with fallback, or
	// a literal printf template when `key` is empty.
	std::string key;
	std::string fallback;
	std::string literal;
	std::vector<EndRoundArg> args;
	int y = 0; // design-space y (1024x768), x is always 512 centred
	EndRoundEmptyFold fold = EndRoundEmptyFold::kNone;
};

struct EndRoundOverlayInput {
	uint32_t game_type = 0;
	bool draw = false;            // g_endround_draw_flag
	int winner_team = 0;          // g_endround_winner_team
	uint8_t local_team = 0;       // byte_A85B48
	bool death_screen = false;    // g_death_screen_active
	int32_t team_scores[2] = {0, 0}; // g_scoreTeamScore0/1
	// The non-team form's three named players (byte_24C1A98/B7C/C60 + the
	// i16 triple @0x24C1AD4/BB8/C9C); empty names = absent.
	std::string player_names[3];
	int32_t player_scores[3] = {0, 0, 0};
	int32_t round_time_remaining_ticks = 0; // g_round_time_remaining
};

// The text ladder [orig: draw_endround_stats_overlay @0x5b7cd0]: headline at
// y 300 (STROVER34 draw / STROVER35 the two-name tie / STROVER32..33,61,62
// per winner team in team modes / STROVER_PLAYERWIN with the first name /
// STROVER1 "!Mission Completed"), the game-type second line at y 350 (the
// STROVER100..120 family with BLUETEAM/REDTEAM names and the
// g_round_time_remaining arm; the objective-family STROVER1/2 arm), then from
// y+32 the "%s : %ld" score lines stepping 40 (the team pair, or up to three
// named players) followed by +24 and STROVER_GAMETIME "%s : %d:%02d:%02d".
std::vector<EndRoundLine> end_round_overlay_lines(const EndRoundOverlayInput &in);

// The stat-field column layout of the first pass [orig:
// Overlay_ComputeStatFieldColumnLayout(40, 984) @0x5b7a10]: the name column
// is the widest player name (min 100) + 10; each enabled field (every field
// when the show-disabled toggle is set, in which case the SMALL label keys
// are used) measures its Overlays/STROVER_STATFIELD%02d label (min 40) + 10;
// the leftover width is split: a quarter shifts the block right, half is
// spread evenly over the columns + the name column (per gap = half /
// (count + 1)); the x positions run from the shifted left edge + the padded
// name width, each column's centre at x + width/2.
struct EndRoundColumn {
	int field_id = 0;
	int field_index = 0; // index into the declared fields
	std::string label_key;
	std::string label_fallback;
	int x = 0;
	int center_x = 0;
	int width = 0;
};
struct EndRoundColumnLayout {
	int left = 0;        // dword_28E3AB0 (after the quarter shift)
	int name_width = 0;  // the padded name column
	int right = 0;       // dword_28E3AB4 (the x after the last column)
	std::vector<EndRoundColumn> columns;
};
// `measure` returns the design-space width of a label (the bold label font
// through the overlay's 1024 scale); `fields` is the declared {id, enabled}
// list; `player_names` feeds the name column measure.
EndRoundColumnLayout end_round_column_layout(
		int region_left, int region_right,
		const std::vector<std::pair<uint8_t, uint8_t>> &fields, bool show_disabled,
		const std::vector<std::string> &player_names,
		const std::function<int(const std::string &)> &measure,
		const std::function<std::string(const std::string &key, const std::string &fallback)> &resolve);

// The STAT field-id -> STROVER string index map [orig: dword_83C840 — the
// (strIndex, fieldId) pairs, identity except 30 -> 33, 31 -> 27, 32 -> 34].
int stat_field_string_index(int field_id);

// The stat-screen delay after the announcement, milliseconds: stat.mnu opens
// once the board has reassembled AND this much wall time has passed
// [orig: 0x1770 @0x5b8615 in UI_ProcessEndRoundScreenTransition @0x5b8600].
inline constexpr int kEndRoundStatScreenDelayMsec = 6000;

// The overlay's design-space safe area: the full 1024x768 frame's top and
// bottom rows [orig: dword_24C1900 / dword_24C1904, stamped at display-mode
// set @0x587634 / @0x58760b — 0 / 768 for the full frame].
inline constexpr int kEndRoundOverlayTop = 0;
inline constexpr int kEndRoundOverlayBottom = 768;

// The gametext Overlays-table lookup the resolver reads through: true when
// `key` EXISTS in the table (its value may be empty — that is the fold case),
// false when the key is missing (the fallback case).
using EndRoundTextLookup =
		std::function<bool(const std::string &key, std::string &value)>;

struct EndRoundResolvedLine {
	std::string text;
	int y = 0;
};

// Resolve the ladder: every line's key through the Overlays table (a missing
// key takes the "!..." fallback with its marker stripped, a present-but-empty
// key takes the line's EndRoundEmptyFold), then the printf arguments in
// retail's sprintf forms (%s, %d, %ld, %02d) [orig: draw_endround_stats_overlay
// @0x5b7cd0 — GameText_GetStringWithFallback per line/argument, the sprintf
// per arm; LABEL_30 @0x5b7e59 -> the STROVER1 re-lookup @0x5b7e5b; LABEL_144
// @0x5b83e5 — the second line draws nothing and the score lines start at 382].
std::vector<EndRoundResolvedLine> end_round_overlay_resolve(
		const std::vector<EndRoundLine> &lines, const EndRoundTextLookup &lookup);


// The resolved ladder as one value (the embedder's record wraps it by value):
// the resolved lines and the overlay safe-area top/bottom in design px.
struct EndRoundOverlayLadder {
	std::vector<EndRoundResolvedLine> lines;
	int top = kEndRoundOverlayTop;
	int bottom = kEndRoundOverlayBottom;
};

} // namespace opennova::hud
