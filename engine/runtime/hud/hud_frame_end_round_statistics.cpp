// THE TOGGLED SP "SHOW SCORE" STATISTICS PANEL element: one titled label box
// with four label/value rows (subgoals, enemy units, team units, friendly
// units). Policy, geometry and the value composition live in
// hud/end_round_statistics.h; the shell resolves the title and Epilog labels
// and formats the values because it owns the string tables.
// [orig: HUD_DrawEndRoundStatistics @0x5b7600, called from the frame drawer
//  HUD_DrawOverlayPanels @0x5c0092 while the dword_24C18AC toggle is set — the label box
//  HUD_DrawLabelBox(ctx, 128, top, 896, top + 340, SCORE_TITLE, -1) @0x5b7671,
//  labels left at x 200 via HUD_DrawTextLeftScaled (ex sub_580B40) -> HUD_DrawTextLeft_HalfBright, values
//  right-aligned at x 620 via HUD_DrawTextRightAlignedScaled (ex sub_580BC0) -> HUD_DrawTextRightAligned_HalfBright,
//  all in g_hudLabelFontLarge.]

#include <hud/hud_frame.h>
#include <hud/end_round_statistics.h>

namespace opennova::hud {

void HudFrameCompiler::element_end_round_statistics(const HudFrameState &state,
		float w, float h) {
	const HudEndRoundStatisticsState &st = state.end_round_statistics;
	if (!st.shown) return;
	// Every string rides the LARGE label slot [orig: g_hudLabelFontLarge at
	// every draw @0x5b76a8..0x5b7891]; layout-only embedders fall back.
	const bool have_large = label_font_large_.font() != nullptr;
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &lf = have_large ? label_font_large_
			: have_bold ? label_font_bold_ : font_;
	const float ls = have_large ? label_large_scale_
			: have_bold ? label_scale_ : 1.0f;
	if (lf.font() == nullptr) return;

	const float top = static_cast<float>(end_round_statistics_top(st.raised));
	const float box_y2 = top + static_cast<float>(kEndRoundStatsBoxHeight);
	float title_gap = 0.0f;
	if (!st.title.empty()) {
		int tw = 0;
		int th = 0;
		lf.measure(st.title.c_str(), ls, ls, &tw, &th);
		title_gap = static_cast<float>(tw) + kBoxTitlePad;
		const float s = w / kBoxScaleRef;
		if (title_gap > kBoxTitleTrim * s) title_gap -= kBoxTitleTrim * s;
	}
	emit_stdbox(sx(static_cast<float>(kEndRoundStatsBoxX1), w), sy(top, h),
			sx(static_cast<float>(kEndRoundStatsBoxX2), w), sy(box_y2, h), w,
			0xFFFFFFFFu, title_gap);
	// The title just inside the box corner, white [orig: HUD_DrawLabelBox's
	// (x + 15, y + 2) @0x51efe1/@0x51efe8].
	if (!st.title.empty()) {
		const GameFontRun run = lf.layout(st.title.c_str(),
				sx(static_cast<float>(kEndRoundStatsBoxX1) + 15.0f, w),
				sy(top + 2.0f, h), ls, ls, 0u, 0xFFFFFFFFu);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
				run.quads.end());
	}

	// The rows: label left at 200, value right-aligned at 620, half-bright
	// white, starting 48 below the top and stepping 48
	// [orig: @0x5b7653..0x5b7896].
	const uint32_t color = half_bright_keep_alpha(0xFFFFFFFFu);
	for (size_t i = 0; i < st.labels.size(); ++i) {
		const float y = top + static_cast<float>(kEndRoundStatsRowStart +
				kEndRoundStatsRowStep * static_cast<int>(i));
		if (!st.labels[i].empty()) {
			const GameFontRun run = lf.layout(st.labels[i].c_str(),
					sx(static_cast<float>(kEndRoundStatsLabelX), w), sy(y, h),
					ls, ls, 0u, color);
			draw_list_.glyphs.insert(draw_list_.glyphs.end(),
					run.quads.begin(), run.quads.end());
		}
		if (!st.values[i].empty()) {
			const GameFontRun run = lf.layout(st.values[i].c_str(),
					sx(static_cast<float>(kEndRoundStatsValueX), w), sy(y, h),
					ls, ls, kFontAlignRight, color);
			draw_list_.glyphs.insert(draw_list_.glyphs.end(),
					run.quads.begin(), run.quads.end());
		}
	}
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
