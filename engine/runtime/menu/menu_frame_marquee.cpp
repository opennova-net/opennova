// The MARQUEE_WND interior for the menu frame compiler: the credits a marquee's
// DATASOURCEs loaded (menu_credits.h), scrolled a fixed step per rendered frame.
// [orig: CMarqueeWnd_Render @ 0x65cf90 -> CMarqueeWnd_RenderScrollingCredits @ 0x65ca00]

#include <runtime/menu/menu_frame_internal.h>

#include <string>

namespace opennova::menu {

// The credits scroller [orig: CMarqueeWnd_RenderScrollingCredits @ 0x65ca00]:
// - every node's y falls by SCROLL_RATE on each rendered frame (the float at
//   +0x2DC); when the LAST node's y passes above the clip top, every node goes
//   back to its initial y (the rect's bottom edge plus its offset), so the roll
//   loops as one unit;
// - a text node draws with its own font (not drawn when that font does not
//   load), measured at scale 1, justified by its +216 (0 at the left edge, 2 at
//   the right edge less 5, else centred), in its ~C colour (the text sink forces
//   alpha), after its two marks are remapped (marquee_node_text).
// With no nodes a marquee draws only its frame, appearance and children: it has
// no STRING and no label pass. Image nodes (~I, ~F) and the 50px edge fade are
// the D-MNU-13 residue. The clip viewport is drawn as the node's line meeting
// the rect (a line crossing an edge is drawn whole).
void MenuFrameCompiler::emit_marquee(int index, const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s,
		const MenuFrameState &frame, const MenuWidgetState *ws) {
	(void)node;
	if (ws == nullptr || ws->marquee.nodes.empty()) {
		return;
	}
	const MarqueeCredits &credits = ws->marquee;
	MarqueeScroll &roll = marquee_scroll_[index];
	const bool restart = !roll.valid || ws->marquee_reset;
	const bool step = restart || frame.time_ms != roll.last_ms;
	if (restart) {
		roll.scrolled = 0.0;
		roll.valid = true;
	}
	roll.last_ms = frame.time_ms;
	if (step) {
		roll.scrolled += static_cast<double>(credits.scroll_rate);
		const double last_y = static_cast<double>(rect.bottom) +
				static_cast<double>(credits.nodes.back().offset) - roll.scrolled;
		if (last_y < static_cast<double>(rect.top)) {
			roll.scrolled = 0.0;
		}
	}
	for (const MarqueeCreditNode &credit : credits.nodes) {
		if (!credit.text) {
			continue;
		}
		const int32_t font = font_slot_(credit.font);
		if (font == 0) {
			continue;
		}
		const std::string text = marquee_node_text(credits, credit);
		int text_w = 0;
		int text_h = 0;
		measure_with_(font_at_(font), text, &text_w, &text_h);
		const int y = static_cast<int>(static_cast<double>(rect.bottom) +
				static_cast<double>(credit.offset) - roll.scrolled);
		int x = rect.left;
		if (credit.justify == 2) {
			x = rect.left + (rect.right - rect.left) - text_w - 5;
		} else if (credit.justify != 0) {
			x = rect.left + ((rect.right - rect.left) >> 1) - ((text_w + 1) >> 1);
		}
		if (y + text_h <= rect.top || y >= rect.bottom) {
			continue;
		}
		emit_glyph_run_with_(font, text, x, y, s, credit.color | 0xFF000000u, -1);
	}
}

} // namespace opennova::menu
