#pragma once

// THE TAKEOVER STATUS DIALOG: a stdbox at the hudpos LFP_TAKEOVERDLG anchor
// over the zone-timer entry nearest the local player that is running, whose
// zone holds the player in its cylinder and whose map row the player may see.
// It shows the zone's name and distance, a rule, the takeover bar, the
// attackers' excess ("%dx") and a status line: "Taking over building PSP", or,
// for a numbered zone, the camp's state ("Setting up camp", "Enemy is
// destroying camp", ...).
// [orig: HUD_DrawTakeoverStatus @0x59B630, called from HUD_RenderAllOverlays
//  @0x5A87E4 only while g_DeathScreenActive is clear (`cmp` @0x5A87E2), after
//  HUD_DrawSquadOrderLines; the nearest entry Entity_FindNearestInProximityList
//  @0x537F80 with filter 7 (`push 7` @0x59B658)]
//
// The facts are the client's (inmatch/takeover_status_feed.h); this header
// composes the dialog's strings and colours from them, and
// HudFrameCompiler::element_takeover_status draws it.

#include <cstdint>
#include <string>

#include <runtime/hud/game_text_lookup.h>

namespace opennova::hud {

// The nearest qualifying zone-timer entry and its zone, as the dialog reads them.
// The entry's 13 dwords: [1] / [2] the teams, [3] the window's current, [5] its
// limit, [7] its active flag, [8] the value's current, [9] its target, [10] its
// limit, [11] its rate, [12] its active flag [orig: ZoneTimerList_SetEntryValue
// @0x537EC0, ZoneTimerList_SetEntryWindow @0x537DE0].
struct HudTakeoverFacts {
	bool present = false;
	int zone_byte = 0;              // the zone entity's +0x21A
	int zone_index = -1;            // SpawnZoneList_IndexOf (a miss is -1)
	int32_t distance_q16 = 0;       // ftol of the 3-D length to the player
	int32_t bound_radius_q16 = 0;   // the zone entity's +0
	bool spawn_point_def = false;   // its def +0x54 & 0x40000
	int def_type = 0;               // its def +0x5C (1 vehicle, 5 building)
	uint8_t count_owner = 0;        // +0x220
	uint8_t count_other = 0;        // +0x221
	int32_t team_a = 0;             // entry[1]
	int32_t team_b = 0;             // entry[2]
	int32_t window_current = 0;     // entry[3]
	int32_t window_limit = 0;       // entry[5]
	bool window_active = false;     // entry[7]
	int32_t value_current = 0;      // entry[8]
	int32_t value_target = 0;       // entry[9]
	int32_t value_limit = 0;        // entry[10]
	int32_t value_rate = 0;         // entry[11]
	bool value_active = false;      // entry[12]
};

// The dialog's geometry, design units off the LFP_TAKEOVERDLG anchor (x, y)
// [orig: HUD_DrawTakeoverStatus @0x59B630].
// The box (x - 20, y - 20)..(x + 260, y + 92) [orig: @0x59B68F..0x59B6AB].
inline constexpr int kTakeoverBoxLeft = -20;
inline constexpr int kTakeoverBoxTop = -20;
inline constexpr int kTakeoverBoxRight = 260;
inline constexpr int kTakeoverBoxBottom = 92;
// The distance, right-aligned at x + 230 on the name's row
// [orig: `lea ebx, [ebp+0F0h]; lea ecx, [ebx-0Ah]` @0x59B7C9..0x59B7D0].
inline constexpr int kTakeoverDistanceDx = 230;
// The rule (x, y + 26)..(x + 240, y + 26) in 0xFFC0C0C0 [orig: `lea eax,
// [edi+1Ah]` @0x59B7E5, the pushes @0x59B7DE..0x59B7EE].
inline constexpr int kTakeoverRuleDy = 26;
inline constexpr int kTakeoverRuleWidth = 240;
inline constexpr uint32_t kTakeoverRuleColor = 0xFFC0C0C0u;
// The name and the distance in 0xFFD0D0D0 [orig: `push 0FFD0D0D0h`
// @0x59B7A1 / @0x59B7BC].
inline constexpr uint32_t kTakeoverTextColor = 0xFFD0D0D0u;
// The first bar's row y + 32, each bar 220 wide and 14 tall [orig: `add edi,
// 20h` @0x59B7FE, `lea eax, [ebp+0DCh]` @0x59B81F, `lea edx, [edi+0Eh]`
// @0x59B833].
inline constexpr int kTakeoverBarDy = 32;
inline constexpr int kTakeoverBarWidth = 220;
inline constexpr int kTakeoverBarHeight = 14;
// The excess count at (x + 222, bar row - 1) [orig: `lea eax, [ebp+0DEh]`
// @0x59B8EA, `lea edx, [edi-1]` @0x59B8E6].
inline constexpr int kTakeoverCountDx = 222;
// Each further row steps 18 [orig: `add edi, 12h` @0x59B90A / @0x59BAD0].
inline constexpr int kTakeoverRowStep = 18;

// The dialog as the frame draws it.
struct HudTakeoverStatusState {
	bool shown = false;
	std::string name;     // the first row, left at x
	std::string distance; // the first row, right-aligned at x + 230
	// The bars' and the bold texts' colour: g_HUDColors entry 3 for entry[2] 1,
	// 5 for 2, else 1 [orig: @0x59B801..0x59B819].
	int palette = 1;
	// The first bar: the window's fraction while entry[7], else 1 when the
	// value channel runs with entry[1] == entry[2], else 0 [orig:
	// @0x59B863..0x59B883].
	float first_fraction = 0.0f;
	// The excess beside the first bar: a numbered zone's window [orig:
	// @0x59B8AB..0x59B8F6].
	bool first_count = false;
	// A window entry steps the rows past the first bar [orig: @0x59B8FE..0x59B90A].
	bool window_row = false;
	// A numbered zone's second bar on the current row: the value's fraction
	// while entry[12], else 0 [orig: @0x59B9E9..0x59BA71].
	bool second_bar = false;
	float second_fraction = 0.0f;
	// The excess beside it while the value channel runs at a nonzero rate
	// [orig: @0x59BA7B..0x59BAC8].
	bool second_count = false;
	std::string count;    // "%dx" of +0x220 - +0x221 [orig: @0x59B8BE..0x59B8D9]
	std::string status;   // the bold status line
};

// The dialog's strings, colour and bars from the nearest entry's facts;
// `local_team` is the local player's Team byte, sign-extended, the camp lines'
// own-side test [orig: `movsx eax, byte ptr [edx+162h]` @0x59BB26 / @0x59BBC8].
HudTakeoverStatusState compose_takeover_status(const HudTakeoverFacts &facts, int local_team,
		const GameTextLookup &gametext);

// The scaled 2-D line's clip, in design units: Cohen-Sutherland against
// 0..1023 x 0..767, the x edges first, then the y edges over the clipped
// points, each cut in 32-bit integer arithmetic (the products wrap, the
// divides truncate). False when the segment lies wholly outside.
// [orig: Render_ClipAndDrawLine2DScaled @0x5D3160 -- the outcodes
//  @0x5D3186..0x5D31EB, the x cuts @0x5D3201..0x5D3296, the y outcodes
//  @0x5D329C..0x5D32D2, the y cuts @0x5D32DF..0x5D336E]
bool clip_design_line(int32_t &x0, int32_t &y0, int32_t &x1, int32_t &y1);

} // namespace opennova::hud
