#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {

// THE TAB PLAYER LIST — retail's in-match scoreboard
// [orig: rows HUD_DrawKillList @0x423A30; the header block @0x423060].
// This is NOT the post-round STAT screen (`stat.mnu`, @0x5b8600), which is a
// separate menu-driven surface.
//
// Every constant below is a RAW RETAIL NUMBER in the 1024x768 design space:
// the drawer puts box corners and text through the same integer scaler every
// other hudpos coordinate uses [orig: Viewport_ScaleToVirtualCoords @0x5d2b20;
// the text path @0x5d3f30/@0x5d3ec0], which is `hud::scale_axis` here.

// The panel rect, as CORNERS (not extents)
// [orig: the stdbox call @0x423a72 — (0x14, 0x4e) .. (0x3ec, 0x226)].
inline constexpr int kBoardX1 = 20;
inline constexpr int kBoardY1 = 78;
inline constexpr int kBoardX2 = 1004;
inline constexpr int kBoardY2 = 550;
// The title rides just inside the top-left corner [orig: +0xf, +2].
inline constexpr int kTitleDx = 15;
inline constexpr int kTitleDy = 2;
// The centred header block and its line ladder [orig: FUN_00423060(ctx, 0x1f6,
// 0x69) @0x423a95; the +0x14 steps].
inline constexpr int kHeaderX = 502;
inline constexpr int kHeaderY = 105;
inline constexpr int kHeaderStep = 20;
// The list starts one step below the header's LAST line [orig: the return
// value + 0x14 @0x423aad].
inline constexpr int kListGap = 20;
// Rows draw only while y stays above this [orig: the 0x1ea bound @0x423bba].
inline constexpr int kListBottom = 490;
inline constexpr int kRowPitch = 18;
// The two player columns and the spectator column
// [orig: 0xbe @0x423e52, 0x2b2 @0x423e6a, 0x1b8 @0x423d0f].
inline constexpr int kColumnAX = 190;
inline constexpr int kColumnBX = 690;
inline constexpr int kColumnSpectatorX = 440;
// The rank number sits left of its column [orig: lea eax,[esi - 0x28] @0x4241a7].
inline constexpr int kRankDx = -40;
// The connection icon sits between the rank and the name [orig: add esi,-0x14
// @0x4241fb; the 16x16 quad @0x4241e4]. It samples one band of the neticon2
// 4-row atlas, band = quality - 1; a quality outside 1..3 draws nothing, which
// is retail's own gate rather than a fallback [orig: FUN_004c2ee0 @0x4c2ee0].
inline constexpr int kIconDx = -20;
inline constexpr int kIconSize = 16;
// The paging hint [orig: push 0x1f6/0x1fe @0x4242b7/@0x4242b2].
inline constexpr int kFooterX = 502;
inline constexpr int kFooterY = 510;

// Packed ARGB. The rank is a literal yellow the drawer pushes directly
// [orig: push 0xffffff00 @0x424192]; the two team colors are the palette
// cells the HUD scheme writer fills [orig: FUN_0051f240 @0x51f26d/@0x51f259 —
// the ONLY writer of 0x24c1844 / 0x24c184c].
inline constexpr uint32_t kRankColor = 0xFFFFFF00u;
inline constexpr uint32_t kTeamAColor = 0xFF80A0FFu;
inline constexpr uint32_t kTeamBColor = 0xFFFF5050u;

// The non-team game types — these draw rows in two alternating columns with a
// score; every other type columns BY TEAM and draws no score
// [orig: the branch @0x423ab0].
inline bool scoreboard_is_non_team(uint32_t game_type) {
	return game_type == 0u || game_type == 1u || game_type == 8u;
}

// One row as the drawer needs it.
struct ScoreboardEntry {
	uint8_t slot_id = 0;
	uint16_t status_flags = 0;
	uint16_t score1 = 0;
	uint8_t team = 0;
	bool spectator = false;
	std::string name;   // already joined with the clan prefix by the embedder
	uint8_t quality = 0;  // 0x46 connection quality, 1..3 draws an icon band
};

// The status-glyph suffix a row appends when its bitfield is non-zero: `" ["`,
// one character per set bit IN THE WITNESSED APPEND ORDER, then `"]"`, and
// finally a trailing `S` for bit 0x400 AFTER the bracket
// [orig: the append chain @0x423ef1-0x4240e8; the token table @0x7c4b8c].
// A zero word yields an empty string — retail's parser zeroes it when the
// scoreboard-detail global is clear [orig: @0x42fbfb], so "no word, no glyphs"
// reproduces the gate rather than approximating it.
std::string scoreboard_status_glyphs(uint16_t status_flags);

// The row's text, without the rank (which draws separately in its own color).
// Non-team rows lead with the score [orig: fmt "%3i %s [%02ld]" @0x7c4bc4];
// team rows carry no score [orig: fmt "%s [%02ld]" @0x7c4c00]. The bracketed
// value is the player's SLOT id, the same value that indexes the roster pool
// [orig: the lookup @0x423c60].
std::string scoreboard_row_text(const ScoreboardEntry &e, bool non_team);

// The column a row draws in. Non-team mode alternates A/B by row ordinal;
// team mode maps team 1 -> A and team 2 -> B; spectators have their own column
// [orig: @0x423d3b / @0x423d5c / @0x423d0f].
int scoreboard_column_x(const ScoreboardEntry &e, bool non_team, int ordinal);

// The row's color: team modes take the team palette, non-team rows and
// spectators take the active HUD color (passed in, since the scheme is a
// client setting rather than wire data).
uint32_t scoreboard_row_color(const ScoreboardEntry &e, bool non_team,
                              uint32_t hud_color);

} // namespace opennova::hud
