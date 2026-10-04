// THE TAB PLAYER LIST element: the titled stdbox, the centred header ladder
// with its per-mode team-score block and flag-carrier line, and the paged
// player rows. Policy (formats, columns, colors, the page fold, the header
// lines) lives in hud/hud_scoreboard.h; this TU walks it in the drawer's order.
// [orig: HUD_DrawKillListIfVisible @0x424300 gates on g_ScoreboardPanelVisible
//  — TOGGLED by the playerlist input action (Scoreboard_TogglePlayerList
//  @0x4244c0), cleared on respawn init @0x4993ae; rows HUD_DrawKillList
//  @0x423a30; the centred header block HUD_DrawGameScoreOverlay @0x423060]

#include <runtime/hud/hud_frame.h>

#include <algorithm>
#include <cstdio>

namespace opennova::hud {

// The per-row connection icon: one band of the neticon2.tga 4-row vertical
// atlas, band = quality - 1. Any quality outside 1..3 draws NOTHING —
// retail's own gate (the parser clamps the byte at 4, and 4 still selects no
// band) [orig: NetIcon_DrawConnectionQualityBand @0x4c2ee0 — the 1..3
// switch, default returns; the atlas load @0x4c2cf0 sets band = tgaH/4].
void HudFrameCompiler::emit_net_icon(float x0, float y0, float x1, float y1,
		int quality) {
	if (!layout_.net_icon_texture_valid) return;
	if (quality < 1 || quality > 3) return;
	const float band = 1.0f / 4.0f;
	const float v0 = static_cast<float>(quality - 1) * band;
	emit_rect_uv(x0, y0, x1, y1, 0.0f, v0, 1.0f, v0 + band, 0xFFFFFFFFu,
			kHudTexNetIcon);
}

void HudFrameCompiler::element_scoreboard(const HudFrameState &state, float w,
		float h) {
	// Drawn last — above every other overlay.
	if (!state.scoreboard.shown) return;
	const HudScoreboardState &sb = state.scoreboard;

	// Every string on the board rides the BOLD label font at the slot scale
	// [orig: g_HUDLabelFontBold at every draw site — the title @0x51f13a, the
	// header rungs, the rank/rows/footer HUD_DrawTextAligned (ex sub_5D3F30) calls]; layout-only
	// embedders fall back to the hudpos font.
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	if (bf.font() == nullptr) return;
	const auto text = [&](const char *t, float design_x, float design_y,
			uint32_t argb, uint32_t flags) {
		if (t == nullptr || t[0] == 0) return;
		const GameFontRun run = bf.layout(t, sx(design_x, w), sy(design_y, h),
				bscale, bscale, flags, argb);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
				run.quads.end());
		draw_list_.underlines.insert(draw_list_.underlines.end(),
				run.underlines.begin(), run.underlines.end());
	};

	const uint32_t hud = active_color(state);
	// The panel frame behind everything, with the title's bar notched into
	// the top border: the gap is the measured bold title + 2, less 12*s once
	// it exceeds that [orig: HUD_DrawLabelBox @0x51f0ea-0x51f114].
	const float s = w / kBoxScaleRef;
	float title_gap = 0.0f;
	if (!sb.title.empty()) {
		int tw = 0;
		int th = 0;
		bf.measure(sb.title.c_str(), bscale, bscale, &tw, &th);
		title_gap = static_cast<float>(tw) + kBoxTitlePad;
		if (title_gap > kBoxTitleTrim * s) title_gap -= kBoxTitleTrim * s;
	}
	emit_stdbox(sx(static_cast<float>(kBoardX1), w),
			sy(static_cast<float>(kBoardY1), h),
			sx(static_cast<float>(kBoardX2), w),
			sy(static_cast<float>(kBoardY2), h), w, 0xFFu, title_gap);
	// The title just inside the panel's top-left corner, white, left-aligned
	// [orig: (x+15, y+2) @0x51f002/@0x51f006; the caller's -1 @0x423a90].
	text(sb.title.c_str(), static_cast<float>(kBoardX1 + kTitleDx),
			static_cast<float>(kBoardY1 + kTitleDy), 0xFFFFFFFFu, 0u);

	// The centred header ladder on its FIXED rungs — a missing string leaves
	// its rung blank rather than compacting the ladder [orig: the
	// unconditional +0x14 steps @0x42315c/@0x423184/@0x4231da/@0x423225].
	// Server name and mission title render white (retail embeds an explicit
	// <cFFFFFF> run [orig: @0x51f42d/@0x51f497]); the rest take the HUD color.
	float hy = static_cast<float>(kHeaderY);
	text(sb.server_name.c_str(), kHeaderX, hy, 0xFFFFFFFFu, kFontAlignCenter);
	hy += kHeaderStep;
	text(sb.mission_title.c_str(), kHeaderX, hy, 0xFFFFFFFFu, kFontAlignCenter);
	hy += kHeaderStep;
	text(sb.game_type_label.c_str(), kHeaderX, hy, hud, kFontAlignCenter);
	hy += kHeaderStep;
	text(sb.players_line.c_str(), kHeaderX, hy, hud, kFontAlignCenter);
	hy += kHeaderStep;
	// Only the spectator rung is conditional [orig: the nonzero gate
	// @0x42322a].
	if (!sb.spectators_line.empty()) {
		text(sb.spectators_line.c_str(), kHeaderX, hy, hud, kFontAlignCenter);
		hy += kHeaderStep;
	}
	// The per-mode team-score block, one step per line in the active color
	// [orig: the switch @0x4232bf..0x423925, every line
	// HUD_DrawTextAtVirtualPos(ctx, 502, y, 0, text, bold, active, 2)].
	ScoreboardHeaderInput header;
	header.game_type = sb.game_type;
	header.team_count = sb.team_count;
	header.time_limit = sb.time_limit;
	header.teams = sb.teams;
	for (const std::string &line : scoreboard_team_score_lines(header, sb.header_text)) {
		text(line.c_str(), kHeaderX, hy, hud, kFontAlignCenter);
		hy += kHeaderStep;
	}
	// The flag carrier line on the final rung, which it does NOT advance
	// [orig: @0x423932..0x423a12].
	if (scoreboard_has_flag_carrier_line(sb.game_type) && sb.flag_carrier) {
		const std::string line =
				scoreboard_flag_carrier_text(sb.flag_carrier_label, sb.flag_carrier_name);
		text(line.c_str(), kHeaderX, hy,
				scoreboard_flag_carrier_color(sb.flag_carrier_team, hud), kFontAlignCenter);
	}

	// The list base sits one step below the header's return, and every row
	// cursor PRE-increments before its row draws [orig: base = return + 0x14
	// @0x423aad; row_y = cursor + 18 @0x423d30].
	const int list_base = static_cast<int>(hy) + kListGap;
	const bool non_team = scoreboard_is_non_team(sb.game_type);
	// The two teams (and colors) a team-mode board columns this frame: 1/2,
	// or the 3/4 page on the frame counter's bit 7 once more than two sides
	// are configured [orig: @0x423cd0-0x423cf1].
	const ScoreboardTeamPage page = scoreboard_team_page(sb.team_count, sb.frame_counter);

	// The pre-pass: the column counts size the pages and seed the spectator
	// cursor below the LONGER player column [orig: @0x423af1..0x423c3a]. The
	// stored page folds into range and is written back before the rows walk.
	const ScoreboardColumnCounts counts = scoreboard_column_counts(sb.rows, non_team);
	const int pages = scoreboard_page_count(counts, list_base);
	scoreboard_page_ = scoreboard_page_fold(scoreboard_page_, pages);
	const int row_base = scoreboard_row_base(list_base, scoreboard_page_);
	int y_a = row_base;
	int y_b = row_base;
	int y_spec = row_base + kRowPitch *
			(std::max(counts.column_a, counts.column_b) + counts.header_rows);

	ScoreboardRowContext ctx;
	ctx.non_team = non_team;
	ctx.status_suffix = sb.status_suffix;
	ctx.timed = sb.timed;
	ctx.time_limit = sb.time_limit;
	ctx.local_team = sb.local_team;
	ScoreboardRowComposer composer(ctx, sb.class_names);

	// One GLOBAL rank counter in wire order — both columns share it, and it
	// advances for every non-spectator row whether or not the row draws
	// [orig: the ++ @0x42424f sits outside the visibility test].
	int rank = 1;
	int ordinal = 0;
	for (const ScoreboardEntry &e : sb.rows) {
		// Team modes draw only rows whose slot still binds a live entity on
		// one of the page's two teams — a leaver's row vanishes, and the
		// other page's teams wait their 128 frames [orig: the entity-null
		// fallthrough @0x423d1b; the team tests @0x423d28/@0x423d45].
		if (!non_team && !e.spectator &&
				!(e.has_entity && (e.team == page.team_a || e.team == page.team_b))) {
			continue;
		}
		const int col = scoreboard_column_x(e, non_team, ordinal, page);
		if (!e.spectator) ++ordinal;
		const int row_rank = rank;
		if (!e.spectator) ++rank;
		int *cursor = e.spectator ? &y_spec : (col == kColumnAX ? &y_a : &y_b);
		*cursor += kRowPitch;
		const int row_y = *cursor;
		// The text is built BEFORE the visibility test, so an off-page row
		// still moves the class-suffix carry [orig: @0x423ddf..0x424150].
		const std::string line = composer.compose(e);
		// Rows draw only between the unscrolled base and the panel bottom
		// [orig: `y >= base && y < 490` @0x424168].
		if (row_y < list_base || row_y >= kListBottom) continue;
		const float fy = static_cast<float>(row_y);
		const uint32_t color = scoreboard_row_color(e, non_team, hud, page);
		// Spectators carry no rank [orig: the rank sprintf sits inside the
		// non-spectator arm @0x42416e].
		if (!e.spectator) {
			char rankbuf[16];
			std::snprintf(rankbuf, sizeof(rankbuf), "%2d.", row_rank);
			// [orig: "%2ld." @0x424186 at column - 40, yellow -256 @0x4241b0]
			text(rankbuf, static_cast<float>(col + kRankDx), fy, kRankColor, 0u);
		}
		text(line.c_str(), static_cast<float>(col), fy, color, 0u);
		// The connection icon draws for EVERY row with a live slot —
		// spectators included; a wiped slot's quality 0 is the no-draw gate
		// [orig: the slot test @0x4241e2, the 16x16 quad @0x424203-0x424244].
		emit_net_icon(sx(static_cast<float>(col + kIconDx), w), sy(fy, h),
				sx(static_cast<float>(col + kIconDx + kIconSize), w),
				sy(fy + static_cast<float>(kIconSize), h), e.quality);
	}

	// The paging hint, centred yellow [orig: the draw @0x4242d4].
	text(sb.footer.c_str(), static_cast<float>(kFooterX), static_cast<float>(kFooterY),
			kRankColor, kFontAlignCenter);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
