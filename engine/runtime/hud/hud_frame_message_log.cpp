// THE RECENT MESSAGES (J-key) WINDOW element: one titled stdbox with the CHAT
// ring down the left column and the SYSTEM ring down the right, sixteen rows
// each, no expiry gate [orig: HUD_DrawMessageLog @0x5b9d70, called from the
// frame drawer Server_DrawStatusScreen @0x50a2d0 (the call @0x50b21f) when
// g_showMessageLog @0x24C18C0 is set — no hud_detail / declutter test on that
// path].
//
// Geometry is computed in SCREEN pixels from the surface width
// (hud/hud_message_log.h carries the witnessed functions) and the box corners
// are pushed back through the design-space inverse before the stdbox draw
// [orig: Viewport_ScreenToVirtual @0x5b9e23/@0x5b9e33]; the text rows stay in
// screen pixels [orig: HUD_DrawTextAligned_HalfBright (ex sub_5D2F20) @0x5b9ebc/@0x5b9f02 takes viewport x/y].

#include <hud/hud_frame.h>
#include <hud/hud_message_log.h>

#include <algorithm>

namespace opennova::hud {

void HudFrameCompiler::element_message_log(const HudFrameState &state, float w,
		float h) {
	if (!state.message_log_shown) return;
	// Every row rides the BOLD label font [orig: &g_hudLabelFontBold at both
	// column draws @0x5b9eac/@0x5b9ef2]; layout-only embedders fall back.
	const bool have_bold = label_font_bold_.font() != nullptr;
	const GameFont &bf = have_bold ? label_font_bold_ : font_;
	const float bscale = have_bold ? label_scale_ : 1.0f;
	if (bf.font() == nullptr) return;

	const int surface_w = static_cast<int>(w);
	const int surface_h = static_cast<int>(h);
	const int step_px = message_log_step_px(surface_w);
	const int top_px = message_log_top_px(surface_w);
	const int text_top_px = message_log_text_top_px(surface_w);
	const int chat_x_px = message_log_chat_x_px(surface_w);
	const int system_x_px = message_log_system_x_px(surface_w);

	// The stdbox: screen top/bottom (top + 16 rows) back to design space, then
	// the 32-px design growth at both ends, at design x 8..1016, titled with
	// STROVER43 in white [orig: @0x5b9e1f..0x5b9e73 —
	//  HUD_DrawLabelBox(&overlayCtx, 8, outY - 32, 1016, bottom + 32, title, -1)].
	const float box_y1 = message_log_to_design_y(top_px, surface_h) -
			kMessageLogBoxPadY;
	const float box_y2 = message_log_to_design_y(
			top_px + kMessageLogRows * step_px, surface_h) + kMessageLogBoxPadY;
	float title_gap = 0.0f;
	if (!state.message_log_title.empty()) {
		int tw = 0;
		int th = 0;
		bf.measure(state.message_log_title.c_str(), bscale, bscale, &tw, &th);
		title_gap = static_cast<float>(tw) + kBoxTitlePad;
		const float s = w / kBoxScaleRef;
		if (title_gap > kBoxTitleTrim * s) title_gap -= kBoxTitleTrim * s;
	}
	emit_stdbox(sx(kMessageLogBoxX1, w), sy(box_y1, h), sx(kMessageLogBoxX2, w),
			sy(box_y2, h), w, 0xFFFFFFFFu, title_gap);
	// The title just inside the box corner, white [orig: HUD_DrawLabelBox's
	// (x + 15, y + 2) @0x51efe1/@0x51efe8].
	if (!state.message_log_title.empty()) {
		const GameFontRun run = bf.layout(state.message_log_title.c_str(),
				sx(kMessageLogBoxX1 + kMessageLogTitleDx, w),
				sy(box_y1 + kMessageLogTitleDy, h), bscale, bscale, 0u,
				0xFFFFFFFFu);
		draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
				run.quads.end());
	}

	// The rows: window row k (top first) shows ring slot 16 - k of each ring —
	// the oldest of the sixteen newest at the top, the newest at the bottom,
	// blank slots blank; the stored colour is drawn AS STORED, no expiry test
	// [orig: the walk @0x5b9e8a..0x5b9f1a from unk_B405BC (slot 16) down by
	//  128 to byte_B3FDBC, the colour at +120].
	const auto column = [&](const std::vector<HudMessageLine> &ring, int x_px,
			uint32_t align) {
		const int count = static_cast<int>(ring.size());
		for (int row = 0; row < kMessageLogRows; ++row) {
			const int src = message_log_row_source(row, count);
			if (src < 0) continue;
			const HudMessageLine &line = ring[static_cast<size_t>(src)];
			if (line.text.empty()) continue;
			const GameFontRun run = bf.layout(line.text.c_str(),
					static_cast<float>(x_px),
					static_cast<float>(text_top_px + row * step_px), bscale,
					bscale, align, line.color);
			draw_list_.glyphs.insert(draw_list_.glyphs.end(), run.quads.begin(),
					run.quads.end());
		}
	};
	// CHAT down the left, left-aligned [orig: @0x5b9ebc, x = 32*w/1024 + 2];
	// SYSTEM down the right, right-aligned at 990*w/1024 [orig: @0x5b9f02,
	// draw mode 1].
	column(chat_lines_, chat_x_px, 0u);
	column(feed_lines_, system_x_px, kFontAlignRight);
	++draw_list_.elements_drawn;
}

} // namespace opennova::hud
