#pragma once

#include <runtime/hud/game_text_lookup.h>

#include <array>
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
// the board's text rides g_HUDLabelFontBold [orig: every HUD_DrawTextAligned (ex sub_5D3F30) /
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
// [orig: -256 @0x4241b0]; the two team colors are g_HUDColors[3] and [5]
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
// pair, then `g_NumTeamsConfig > 2 && (dword_A87060 & 0x80)` -> esi=3,
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
	// The status word as the 0x16 parser STORED it: zero unless the SU gate
	// was on at parse time [orig: `if (!g_ScoreboardStatusSuffixEnabled)
	// ping = 0` @0x42fbfb].
	uint16_t status_flags = 0;
	// The mode stat — SIGNED, as the parser movsx's the wire u16 into its
	// record and "%3i" prints negatives as negatives [orig: the movsx
	// @0x42fb9d into rec+0x28]. The type carries the sign so no consumer can
	// re-read 65534 for -2.
	int16_t score1 = 0;
	// The row's team as the drawer reads it: the LIVE entity team byte when
	// the slot binds one [orig: player_entity->Team @0x423d21 / the pre-pass
	// @0x423b29], else the 0x16 row's own flags >> 1 (which the parser also
	// wrote onto the entity @0x42fc88).
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
	                    // @0x42fd46], at most 31 characters [orig:
	                    // Napi_CopyString(rec, .., 32) @0x42fd59/@0x42fd6a].
	// The row's highlighted label (the <ch>%s<co> run): the slot's
	// clan-registry tag (slot+0x20, PlayerSlot_SetName @0x4348f0) copied into
	// rec+32 at parse time, 7 characters at most [orig:
	// Napi_CopyString(rec + 32, tag, 8) @0x42fd85; empty @0x42fd8f].
	std::string label;
	uint8_t quality = 0;  // the 0x46 quality byte, 1..3 draws an icon band
	// The row entity's playerClass (entity+0x294, 5..9) — the same-team
	// class suffix's switch [orig: @0x423d8a].
	uint8_t player_class = 0;
};

// THE STATUS SUFFIX — gated per recipient by the SU text command
// [orig: g_ScoreboardStatusSuffixEnabled test @0x423ef8; set by S2C 0x24
// "SU <n>" @0x429f71]. Gate OFF: nothing. Gate ON: `" ["`, one character per
// set bit IN THE WITNESSED APPEND ORDER, then `"]"`, and finally a trailing
// `S` for bit 0x400 AFTER the bracket — so a zero word still yields the empty
// pair " []" [orig: the append chain @0x423ef1-0x4240e8; " [" @0x423f1c,
// "]" @0x4240c8].
std::string scoreboard_status_suffix(bool enabled, uint16_t status_flags);

// The class names the same-team suffix switches over, resolved by the
// embedder from gametext Overlays [orig: @0x423d8a..0x423dbe]: index 0..4 =
// playerClass 5..9 (STROVR_MEDIC / _SNIPER / _GUNNER / _RIFLEMAN /
// _ENGINEER), index 5 = every other class (STROVR_UNKNOWN).
inline constexpr int kScoreboardClassNameCount = 6;
using ScoreboardClassNames = std::array<std::string, kScoreboardClassNameCount>;
const char *scoreboard_class_name_key(int index);
const std::string &scoreboard_class_name(const ScoreboardClassNames &names,
		uint8_t player_class);

// What the row composer reads beside the row.
struct ScoreboardRowContext {
	bool non_team = false;
	// The SU gate [orig: g_ScoreboardStatusSuffixEnabled @0xA85B49].
	bool status_suffix = false;
	// Solo KOTH's countdown rows [orig: g_ScoreboardFlags & 2 @0x423e7d].
	bool timed = false;
	// The countdown's minutes [orig: is_authority ? g_TimeLimitMinutes :
	// dword_A821C0 @0x423a4b..0x423a5e].
	int time_limit = 0;
	// The local player's team byte, -1 without a local entity (retail
	// dereferences g_LocalPlayerEntity unguarded @0x423d64; no entity never
	// matches here).
	int local_team = -1;
};

// The row text composer — one per board draw, fed the rows that reach the
// drawn arms IN WIRE ORDER (every non-team row; team-mode spectators and
// team-mode rows on the page's two teams — including the ones the page fold
// scrolls out of view, since the text is built before the visibility test).
// Rows: non-team PLAYER rows lead with the score, or with solo KOTH's
// countdown when the timed flag is up [orig: "%3i %s<ch>%s<co> [%02ld]"
// @0x423ee9; "%2i:%02i %s<ch>%s<co> [%02ld]" with (60T - score) / 60 and
// % 60 @0x423ec9]; team rows AND spectator rows in either mode carry no score
// [orig: "%s<ch>%s<co> [%02ld]" @0x423ddf/@0x423e2d]. The bracketed value is
// the SLOT id [orig: the lookup @0x423c6a]. Then the SU suffix, then the
// class suffix " (%s)"
// [orig: @0x424104]. The composer carries retail's class-suffix LEAK: the
// team arm stashes the class name of any row whose entity shares the local
// team, and only a NON-spectator row consumes it — so a same-team
// spectator's stash rides into the next drawn row's text [orig: the stash
// @0x423dc3, the consume + clear gated on `!spectator` @0x4240f0..0x424150].
class ScoreboardRowComposer {
public:
	ScoreboardRowComposer(const ScoreboardRowContext &ctx, const ScoreboardClassNames &names)
		: ctx_(ctx), names_(names) {}
	// The row's text, without the rank (which draws separately in its own
	// color).
	std::string compose(const ScoreboardEntry &e);

private:
	ScoreboardRowContext ctx_;
	const ScoreboardClassNames &names_;
	const std::string *pending_class_ = nullptr;
};

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
// [orig: g_HUDColors.active @0x423d00/@0x423df8; the palette picks @0x423cb9].
uint32_t scoreboard_row_color(const ScoreboardEntry &e, bool non_team,
                              uint32_t hud_color,
                              const ScoreboardTeamPage &page = ScoreboardTeamPage{});

// THE PAGE FOLD [orig: HUD_DrawKillList @0x423bac..0x423c3a]. The pre-pass
// counts the two player columns — non-team rows alternate, team rows count
// teams 1|3 into A and 2|4 into B whatever the page (a row needs a live
// entity) [orig: @0x423af1..0x423b93] — and the spectators; two spacer rows
// sit between players and spectators when both exist [orig: @0x423ba1].
// The spectator column seeds below the LONGER player column plus those rows
// [orig: @0x423c3a].
struct ScoreboardColumnCounts {
	int column_a = 0;
	int column_b = 0;
	int spectators = 0;
	int header_rows = 0;
};
ScoreboardColumnCounts scoreboard_column_counts(const std::vector<ScoreboardEntry> &rows,
		bool non_team);
// The float the page height is scaled by — 1/18 rounded to float
// [orig: flt_7C4C78, the fmul @0x423bd4].
inline constexpr float kScoreboardPageScale = 0.0555555559694767f;
// pages = max(1, ftol(ceil((max(A, B) + header_rows + spectators) /
// ((490 - base) * 1/18f)))) [orig: @0x423bb4..0x423bf1].
int scoreboard_page_count(const ScoreboardColumnCounts &counts, int list_base);
// The stored page folds before every draw and is WRITTEN BACK: below zero
// wraps to the last page, past the last to the first
// [orig: @0x423bf6..0x423c0f].
int scoreboard_page_fold(int page, int pages);
// The row cursors' base: the list base scrolled up one page height per page
// [orig: base - page * (490 - base) @0x423c19..0x423c1c]. A row draws only
// while base <= y < 490 against the UNSCROLLED base [orig: @0x424168].
int scoreboard_row_base(int list_base, int page);

// The special-key handler's scoreboard arm: PgUp/PgDn move the page only in
// a session with the board up, and are CONSUMED then — ahead of the help and
// briefing page keys [orig: Input_HandleSpecialKeys @0x49c8fd..0x49c945 —
// is_in_session, g_ScoreboardPanelVisible == 1, VK 0x21 @0x49c912 page -= 1
// @0x49c917, VK 0x22 page += 1 @0x49c93e]. The open edge resets the page
// [orig: Scoreboard_TogglePlayerList @0x4244e4].
inline bool scoreboard_takes_page_keys(bool in_session, bool board_open) {
	return in_session && board_open;
}

// THE TEAM-SCORE HEADER BLOCK — the per-mode lines the header draws after
// its spectator rung, centred at x 502 in the bold slot, the active HUD
// color, one 20-unit step each [orig: HUD_DrawGameScoreOverlay @0x423060,
// the gate @0x423287..0x4232b4 (skipped for the co-op 0x10020 family and the
// non-team types 0/1/8), the switch @0x4232bf..0x423925]. `teams` is indexed
// by team 0..4 (T0 neutral first, the 0x16 team-table order): score1
// @0xA85AEC+16t, the ctf byte @+8, the koth byte @+12 [orig: the stores
// @0x42fe08..0x42fe42]. The scores are the table's SIGN-EXTENDED u16s.
struct ScoreboardTeamScore {
	int score1 = 0;
	int ctf_flag = 0;
	int koth_hold = 0;
};
// The Client gametext labels the block formats with [orig: STRCLI05 /
// STRCLI06 / STRCLI17 / STRCLI18 the four team names, STRCLI07 / STRCLI08
// the "(of" pair].
struct ScoreboardHeaderText {
	std::array<std::string, 4> team_names;
	std::string of_team_a;
	std::string of_team_b;
};
struct ScoreboardHeaderInput {
	uint32_t game_type = 0;
	int team_count = 0;   // [orig: g_ScoreboardTeamCount — the 4-team rows test == 4]
	int time_limit = 0;   // TKOTH's minutes (ScoreboardRowContext::time_limit)
	std::array<ScoreboardTeamScore, 5> teams{};
};
std::vector<std::string> scoreboard_team_score_lines(const ScoreboardHeaderInput &in,
		const ScoreboardHeaderText &text);

// THE FLAG CARRIER LINE — FlagBall (0x10008) and the stand-alone type 8 only,
// while a carrier is latched [orig: `gameType != 65544 && gameType != 8`
// @0x423932..0x423937; `dword_A860C4` @0x423944]: the Overlays label
// (STROVER_FLAGCARRIER, fallback "!FlagCarrier:" [orig: @0x423959]), two
// spaces, the carrier's entity name (entity+0xF4) [orig: @0x4239b1..0x4239ee],
// centred at x 502 on the header's final y WITHOUT advancing it
// [orig: HUD_DrawTextAligned(.., 2, 256) @0x423a0a; `return yPos` @0x423a12].
inline bool scoreboard_has_flag_carrier_line(uint32_t game_type) {
	return game_type == 0x10008u || game_type == 8u;
}
std::string scoreboard_flag_carrier_text(const std::string &label, const std::string &name);
// The carrier's team byte picks the color: 1 the light-blue palette[3], 2
// the salmon palette[5], anything else the active HUD color
// [orig: @0x42396f..0x42398e].
uint32_t scoreboard_flag_carrier_color(uint8_t team, uint32_t hud_color);

// The board's gametext the drawers look up each draw: the class names
// (GameText_GetString, "" when absent [orig: @0x423d96..0x423dbe]), the
// Client team/"(of" labels [orig: @0x4232d5..0x4238ed], and the flag
// carrier label with its literal fallback [orig: GameText_GetStringWithFallback
// ("Overlays", "STROVER_FLAGCARRIER", "!FlagCarrier:") @0x423959].
struct ScoreboardText {
	ScoreboardClassNames class_names;
	ScoreboardHeaderText header;
	std::string flag_carrier_label;
};
ScoreboardText scoreboard_text(const GameTextLookup &gametext);

// The board's header strings as the drawer composes them each draw: the title, gametext's
// Overlays/STROVER_KILLLIST with its literal [orig: GameText_GetStringWithFallback("Overlays",
// "STROVER_KILLLIST", "!Kill List") @0x423a75]; the game type's rung, its Overlays key
// (game_type::overlay_label_key) read as GameText_GetString reads one, "" on a miss [orig:
// HUD_GetGameTypeOverlayLabel @0x5b8680]; the players line, "%s %i" of Client/STRCLI04 ("" on a miss: no
// table @0x51ebd7, no entry @0x51ec00) and the count [orig: @0x4231e8..0x4231fb]; the spectators line
// likewise of STRCLI23, only while there are spectators [orig: the count's gate @0x42322a; @0x423237..0x42324a];
// the paging hint, keyhelp's Text/CHANGE_SCREEN with its literal, which the getter answers only once
// gametext is loaded [orig: KeyHelp_GetStringWithFallback("Text", "CHANGE_SCREEN", "!PgUp and PgDn to change
// pages") @0x424272; the gametext gate @0x51ed47]. `keyhelp` answers the keyhelp table's entry, else its
// fallback untouched (controls::key_help_string_raw over the process's table, or a table of the caller's).
struct ScoreboardHeaderStrings {
	std::string title;
	std::string game_type_label;
	std::string players_line;
	std::string spectators_line; // "" with no spectators
	std::string footer;
};
ScoreboardHeaderStrings scoreboard_header_strings(const GameTextLookup &gametext, bool gametext_loaded,
		const GameTextLookup &keyhelp, uint32_t game_type, int players, int spectators);

} // namespace opennova::hud
