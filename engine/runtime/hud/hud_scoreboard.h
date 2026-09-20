#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {

// THE TAB PLAYER LIST — retail's in-match scoreboard
// [orig: rows HUD_DrawKillList @0x423A30; the header block @0x423060].
// This is NOT the post-round STAT screen (`stat.mnu`, @0x5b8600), which is a
// separate menu-driven surface — runtime/inmatch/stat_screen_feed.h feeds it, and
// the end-round overlay that precedes it is hud/end_round_overlay.h.
//
// Every constant below is a RAW RETAIL NUMBER in the 1024x768 design space:
// the drawer puts box corners and text through the same integer scaler every
// other hudpos coordinate uses [orig: Viewport_ScaleToVirtualCoords @0x5d2b20;
// the text path @0x5d3f30/@0x5d3ec0], which is `hud::scale_axis` here. All of
// the board's text rides g_hudLabelFontBold [orig: every HUD_DrawTextAligned (ex sub_5D3F30) /
// HUD_DrawTextAtVirtualPos site in the drawer passes @0xB4C394].

// The panel rect, as CORNERS (not extents)
// [orig: the HUD_DrawLabelBox call @0x423a90 — (20, 78) .. (1004, 550)].
inline constexpr int kBoardX1 = 20;
inline constexpr int kBoardY1 = 78;
inline constexpr int kBoardX2 = 1004;
inline constexpr int kBoardY2 = 550;
// The title rides just inside the top-left corner, left-aligned white
// [orig: HUD_DrawLabelBox @0x51f002/@0x51f006 — (x+15, y+2); the caller's -1].
inline constexpr int kTitleDx = 15;
inline constexpr int kTitleDy = 2;
// The centred header ladder [orig: HUD_DrawGameScoreOverlay(ctx, 0x1f6, 0x69)
// @0x423a95]. The rungs are FIXED — each `add ebx, 14h` runs whether or not
// its line's string is empty [orig: @0x42315c/@0x423184/@0x4231da/@0x423225]:
// server name @105, mission title @125, game-type label @145, player count
// @165, spectator count @185 (that line alone is gated on a nonzero count
// [orig: @0x42322a], and only IT compacts the ladder).
inline constexpr int kHeaderX = 502;
inline constexpr int kHeaderY = 105;
inline constexpr int kHeaderStep = 20;
// The list base is one step below the header block's RETURN (itself one step
// past its last rung), and every row cursor pre-increments before drawing —
// so the first row lands a further row pitch down
// [orig: base = the return + 0x14 @0x423aad; row_y = cursor + 18 @0x423d30].
inline constexpr int kListGap = 20;
// Rows draw only while their y stays above this [orig: the `< 490` arm of the
// visibility test @0x424168].
inline constexpr int kListBottom = 490;
inline constexpr int kRowPitch = 18; // [orig: the +18 walks @0x423d30/@0x423d51]
// The two player columns and the spectator column
// [orig: 0xbe @0x423e52, 0x2b2 @0x423e6a, 0x1b8 @0x423e1f].
inline constexpr int kColumnAX = 190;
inline constexpr int kColumnBX = 690;
inline constexpr int kColumnSpectatorX = 440;
// The spectator column does NOT start at the list top: its cursor is seeded
// below the LONGER player column, plus two spacer rows when both players and
// spectators exist [orig: base + 18 * (max(teamA, teamB) + header_rows)
// @0x423c3a; header_rows = 2 iff spectators && players @0x423ba1-0x423baa].
inline constexpr int kSpectatorGapRows = 2;
// The rank number sits left of its column, "%2ld." in the literal yellow
// [orig: the sprintf @0x424186; x = column - 40 and color -256 @0x4241b0].
// One GLOBAL counter ranks the rows in wire order — both columns share it,
// and it advances for every non-spectator row, clipped or not
// [orig: the ++ @0x42424f sits outside the visibility test].
inline constexpr int kRankDx = -40;
// The connection icon sits between the rank and the name — for EVERY row with
// a live connection slot, spectators included [orig: the icon block gates on
// the slot, not the spectator byte @0x4241e2; x = column - 20 @0x424203; the
// 16x16 quad scaled as a size @0x42421f]. It samples one band of the
// neticon2.tga 4-row atlas, band = quality - 1; any quality outside 1..3
// draws nothing — retail's own gate (quality clamps at 4 on store yet 4 still
// draws no band) [orig: NetIcon_DrawConnectionQualityBand @0x4c2ee0 — the
// 1..3 switch, default returns; the atlas load @0x4c2cf0].
inline constexpr int kIconDx = -20;
inline constexpr int kIconSize = 16;
// The paging hint, centred yellow [orig: the draw @0x4242d4 — (0x1f6, 510),
// color -256, mode 2].
inline constexpr int kFooterX = 502;
inline constexpr int kFooterY = 510;

// Packed ARGB. The rank is a literal yellow the drawer pushes directly
// [orig: -256 @0x4241b0]; the two team colors are g_hudColorTable[3] and [5]
// — the cells 0x24c1844 / 0x24c184c the scheme writer fills
// [orig: HUD_InitTeamColorTable @0x51f240, stores @0x51f26d/@0x51f259].
inline constexpr uint32_t kRankColor = 0xFFFFFF00u;
inline constexpr uint32_t kTeamAColor = 0xFF80A0FFu;
inline constexpr uint32_t kTeamBColor = 0xFFFF5050u;
// The second page's pair — literals the drawer pushes for teams 3/4
// [orig: 0xFFFFFF00 / 0xFFFF027F @0x423cec/@0x423cf1].
inline constexpr uint32_t kTeamCColor = 0xFFFFFF00u;
inline constexpr uint32_t kTeamDColor = 0xFFFF027Fu;

// The two teams a team-mode board columns this frame, with their colors.
// Two sides configured: teams 1/2 in the palette pair, always. More than two:
// the board ALTERNATES between the 1/2 page and a 3/4 page on bit 7 of the
// HUD frame counter — a 128-frame period on the same per-main-frame clock
// the blink masks read [orig: Game_TickHudFrameCounters @0x434c14
// ++dword_A87060] — and the 3/4 page swaps in kTeamC/DColor
// [orig: HUD_DrawKillList @0x423cd0-0x423cf1: esi=1/edi=2 with the palette
// pair, then `g_num_teams_config > 2 && (dword_A87060 & 0x80)` -> esi=3,
// edi=4, ecx=0xFFFFFF00, edx=0xFFFF027F].
struct ScoreboardTeamPage {
	uint8_t team_a = 1;
	uint8_t team_b = 2;
	uint32_t color_a = kTeamAColor;
	uint32_t color_b = kTeamBColor;
};
ScoreboardTeamPage scoreboard_team_page(int team_count, int frame_counter);

// The non-team game types — these draw rows in two alternating columns with a
// score; every other type columns BY TEAM and draws no score
// [orig: the type < 2 || type == 8 test @0x423acb].
inline bool scoreboard_is_non_team(uint32_t game_type) {
	return game_type == 0u || game_type == 1u || game_type == 8u;
}

// One row as the drawer needs it.
struct ScoreboardEntry {
	uint8_t slot_id = 0;
	uint16_t status_flags = 0;
	// The mode stat — SIGNED, as the parser movsx's the wire u16 into its
	// record and "%3i" prints negatives as negatives [orig: the movsx
	// @0x42fb9d into rec+0x28]. The type carries the sign so no consumer can
	// re-read 65534 for -2.
	int16_t score1 = 0;
	uint8_t team = 0;
	bool spectator = false;
	// Whether the row's connection slot still binds a live entity. Team modes
	// draw ONLY such rows — a leaver's row vanishes from a team board the
	// moment its 0x46 removal lands, while non-team boards keep showing the
	// row-carried strings [orig: the entity-null fallthrough in the team arm
	// @0x423d1b draws nothing and advances nothing].
	bool has_entity = false;
	std::string name;   // "clan name" — already joined by the embedder in the
	                    // parser's own order [orig: "%s %s" (clan, name)
	                    // @0x42fd46]. The row's third string (the slot+0x20
	                    // squad label, drawn highlighted <ch>..<co>
	                    // @0x423ddf) ships empty and is a recorded residual.
	uint8_t quality = 0;  // the 0x46 quality byte, 1..3 draws an icon band
};

// The status-glyph suffix a row appends when its bitfield is non-zero: `" ["`,
// one character per set bit IN THE WITNESSED APPEND ORDER, then `"]"`, and
// finally a trailing `S` for bit 0x400 AFTER the bracket
// [orig: the append chain @0x423ef1-0x4240e8; " [" @0x423f1c, "]" @0x4240c8].
// Retail's append runs under the per-recipient SU gate
// [orig: g_scoreboardStatusSuffixEnabled test @0x423ef8] and, when that gate
// is ON, a zero word still yields the empty bracket pair " []". The gate is
// an unported residual (D-HUD-24); until it lands, "zero word, no suffix"
// reproduces the gate-OFF default exactly (the parser zeroes every word when
// the gate is off [orig: @0x42fbfb]) and approximates gate-ON by dropping the
// empty " []".
std::string scoreboard_status_glyphs(uint16_t status_flags);

// The row's text, without the rank (which draws separately in its own color).
// Non-team PLAYER rows lead with the score [orig: fmt "%3i %s<ch>%s<co>
// [%02ld]" @0x7c4bc4]; team rows AND spectator rows in either mode carry no
// score [orig: fmt @0x7c4c00; the spectator arm @0x423e04 takes it in
// non-team mode too]. The bracketed value is the player's SLOT id
// [orig: the lookup @0x423c6a].
std::string scoreboard_row_text(const ScoreboardEntry &e, bool non_team);

// The column a row draws in. Non-team mode alternates A/B by non-spectator
// ordinal; team mode maps the page's team_a -> A and team_b -> B; spectators
// have their own column [orig: @0x423d3b / @0x423d5c / @0x423d0f]. Team-mode
// rows with any other team (or no live entity) do not draw at all
// [orig: the fallthrough @0x423d45-0x423d47].
int scoreboard_column_x(const ScoreboardEntry &e, bool non_team, int ordinal,
                        const ScoreboardTeamPage &page = ScoreboardTeamPage{});

// The row's color: team modes take the team palette, non-team rows and
// spectators take the active HUD color (passed in, since the scheme is a
// client setting rather than wire data)
// [orig: g_hudActiveColor @0x423d00/@0x423df8; the palette picks @0x423cb9].
uint32_t scoreboard_row_color(const ScoreboardEntry &e, bool non_team,
                              uint32_t hud_color,
                              const ScoreboardTeamPage &page = ScoreboardTeamPage{});

} // namespace opennova::hud
