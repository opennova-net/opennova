// The wrapped-text drawer and its measure twin for the menu frame compiler:
// the multiline edit's render and line counts, and the table cells' 0x20000
// mode. [orig: CFontCache_DrawTextWrapped @ 0x653710 via CFontCache_DrawTextWrappedClipped
// @ 0x653D60 (0x40000) and CTableWnd_CalculateAlignedTextRect @ 0x63ec50 (0x20000);
// CFontCache_CountWrappedLines @ 0x653b90; CMEditWnd_Render @ 0x6608e0]

#include <runtime/menu/menu_frame_internal.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace opennova::fnt;

namespace opennova::menu {

// The multiline edit render [orig: CMEditWnd_Render @ 0x6608e0]: frame ->
// appearance for the RAW pump state (unlike the single-line sibling, focus
// does NOT force state 2) -> the wrapped drawer. The caret blinks on the
// same (time & 0x3FF) > 0x200 gate; PASSWORD masks with '*'; the vertical
// scroll is line-based — MenuWidgetState.scroll_row carries the
// first-visible-line count (widget +3912), fed by the embedder from
// multiline_line_counts().
void MenuFrameCompiler::emit_multiline_edit(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, int visual,
		const MenuFrameState &frame, const MenuWidgetState *ws) {
	const mnu::Window &w = *node.window;
	std::string text = widget_text(node, ws);
	if (w.password) {
		text.assign(text.size(), '*');
	}
	const bool focused = ws != nullptr && ws->focused && !w.readonly;
	const bool blink_on = (frame.time_ms & 0x3FFu) > 0x200u;
	int caret = -1;
	if (focused && blink_on && ws != nullptr) {
		caret = std::clamp(ws->caret, 0,
				static_cast<int>(text.size()));
	}
	if (text.empty() && caret < 0) {
		return;
	}
	const int state = visual >= 0 && visual < 4 ? visual : 0;
	const int first_line = ws != nullptr ? std::max(ws->scroll_row, 0) : 0;
	// The edge inset rides the draw x [orig: widget[189] added into the pen
	// origin]; the block-alignment leg (whole-unwrapped-text measure) only
	// engages for fitting text — shipped multiline edits author none, so the
	// compiled path keeps the top-left origin.
	const int edge = w.string_data.has_edge ? w.string_data.edge : 0;
	const mnu::RectEdges pen{rect.left + edge, rect.top, rect.right,
			rect.bottom};
	emit_wrapped_text(node, pen, s, node.colors[state], text, first_line,
			caret);
}

// The wrapped-text drawer, clip-bottom mode [orig: CFontCache_DrawTextWrapped
// @ 0x653710 via the 0x40000 wrapper CFontCache_DrawTextWrappedClipped @ 0x653D60].
// Break rules, exactly: the line accumulates chars measured at the widget
// scale pair against trunc(wrapW * scaleX); an explicit LF (only 0x0A — CR
// is a drawn glyph) or the terminator breaks at the char; overflow breaks at
// the last space of the line (consumed), else the overflowing char starts
// the next line. Skipped lines (the scroll window) advance nothing. Each
// drawn line justifies and advances by its 1.0-scale measure; the bottom
// clip stops when the NEXT line's bottom would overflow. The caret pass
// draws "|" centered at the accumulated (scaled) width — the original mixes
// the scaled accumulator into the design-space pen, preserved verbatim.
void MenuFrameCompiler::emit_wrapped_text(const WidgetNode &node,
		const mnu::RectEdges &rect, const WalkScale &s, uint32_t color,
		const std::string &text, int first_visible_line, int caret,
		bool discard_rest) {
	const fnt_font_t *font = font_for(node);
	if (font == nullptr) {
		return;
	}
	hud::GameFont gf;
	gf.set_font(font);
	int wrap_w = rect.right - rect.left;
	if (wrap_w <= 0) {
		wrap_w = 0x10000; // [orig: 0 -> unbounded]
	}
	int avail_h = rect.bottom - rect.top;
	if (avail_h <= 0) {
		avail_h = 0x10000;
	}
	const int threshold =
			static_cast<int>(static_cast<float>(wrap_w) * s.x); // ftol
	const int bottom = rect.top + avail_h;
	const int x = rect.left;
	int cur_y = rect.top;
	int line_start = 0;
	int last_space = 0; // index 0 doubles as "none" [orig quirk]
	int line_no = 0;
	int prev_accum = 0; // scaled width of [line_start, i) for the caret pass
	const int len = static_cast<int>(text.size());
	int i = 0;
	while (true) {
		const char c = i < len ? text[static_cast<size_t>(i)] : '\0';
		if (c == ' ') {
			last_space = i;
		}
		if (caret >= 0 && i == caret) {
			int cw = 0;
			int ch = 0;
			gf.measure("|", 1.0f, 1.0f, &cw, &ch);
			emit_glyph_run(node, "|", x + prev_accum - (cw >> 1), cur_y, s,
					color, -1);
		}
		int accum_w = 0;
		int accum_h = 0;
		if (i >= line_start) {
			gf.measure(text.substr(static_cast<size_t>(line_start),
								 static_cast<size_t>(i - line_start) +
										 (c != '\0' ? 1u : 0u))
							   .c_str(),
					s.x, s.y, &accum_w, &accum_h);
		}
		int break_at;
		int next;
		if (accum_w <= threshold) {
			if (c != '\n' && c != '\0') {
				prev_accum = accum_w;
				++i;
				continue;
			}
			break_at = i;
			next = i + 1;
		} else if (last_space != 0) {
			break_at = last_space;
			next = last_space + 1;
		} else {
			break_at = i; // the overflowing char starts the next line
			next = i;
			if (next <= line_start) {
				// Finite-progress guard: a single glyph wider than the
				// widget (cannot occur with shipped fonts/rects).
				next = line_start + 1;
			}
		}
		if (discard_rest && accum_w > threshold) {
			// The 0x20000 mode: the rest of the source line is dropped; the
			// next line starts after its LF.
			const size_t lf = text.find(static_cast<char>(0x0A), static_cast<size_t>(break_at));
			next = lf == std::string::npos ? len : static_cast<int>(lf) + 1;
		}
		++line_no;
		const char at_break =
				break_at < len ? text[static_cast<size_t>(break_at)] : '\0';
		if (line_no > first_visible_line) {
			const std::string line = text.substr(
					static_cast<size_t>(line_start),
					static_cast<size_t>(break_at - line_start));
			int lw = 0;
			int lh = 0;
			gf.measure(line.c_str(), 1.0f, 1.0f, &lw, &lh);
			if (lh <= 0) {
				// Blank lines keep the font line height (the "W" measure the
				// widget family uses for row heights).
				int tw = 0;
				gf.measure("W", 1.0f, 1.0f, &tw, &lh);
			}
			if (!line.empty()) {
				emit_glyph_run(node, line, x, cur_y, s, color, -1);
			}
			if (at_break == '\0') {
				return;
			}
			cur_y += lh;
			if (!discard_rest && cur_y + lh > bottom) {
				return; // [orig: the 0x40000 bottom clip]
			}
			if (discard_rest && next >= len) {
				return;
			}
		} else if (at_break == '\0') {
			// The original returns only on the drawn branch, relying on the
			// clamped scroll range; the guard keeps a mis-clamped embedder
			// finite without changing clamped behavior.
			return;
		}
		prev_accum = 0;
		last_space = 0;
		line_start = next;
		i = next;
	}
}


bool MenuFrameCompiler::multiline_line_counts(int index,
		const MenuFrameState &state, int *fit_lines, int *total_lines) const {
	if (fit_lines == nullptr || total_lines == nullptr || index < 0 ||
			index >= static_cast<int>(nodes_.size())) {
		return false;
	}
	*fit_lines = 0;
	*total_lines = 0;
	const WidgetNode &node = nodes_[static_cast<size_t>(index)];
	const mnu::Window &w = *node.window;
	const MenuWidgetState *ws = state_for(state, index);
	std::string text = widget_text(node, ws);
	if (w.password) {
		text.assign(text.size(), '*');
	}
	const fnt_font_t *font = font_for(node);
	if (font == nullptr || text.empty()) {
		return true;
	}
	hud::GameFont gf;
	gf.set_font(font);
	const mnu::RectEdges rect = node_rect_(node, ws);
	int wrap_w = rect.right - rect.left;
	if (wrap_w <= 0) {
		wrap_w = 0x10000;
	}
	const int rect_h = rect.bottom - rect.top;
	// The measure twin replays the drawer's break rules at scale 1.0
	// [orig: CFontCache_CountWrappedLines @ 0x653b90].
	std::vector<int> heights;
	int line_start = 0;
	int last_space = 0;
	const int len = static_cast<int>(text.size());
	int i = 0;
	while (true) {
		const char c = i < len ? text[static_cast<size_t>(i)] : '\0';
		if (c == ' ') {
			last_space = i;
		}
		int accum_w = 0;
		int accum_h = 0;
		if (i >= line_start) {
			gf.measure(text.substr(static_cast<size_t>(line_start),
								 static_cast<size_t>(i - line_start) +
										 (c != '\0' ? 1u : 0u))
							   .c_str(),
					1.0f, 1.0f, &accum_w, &accum_h);
		}
		int break_at;
		int next;
		if (accum_w <= wrap_w) {
			if (c != '\n' && c != '\0') {
				++i;
				continue;
			}
			break_at = i;
			next = i + 1;
		} else if (last_space != 0) {
			break_at = last_space;
			next = last_space + 1;
		} else {
			break_at = i;
			next = i <= line_start ? line_start + 1 : i;
		}
		int lw = 0;
		int lh = 0;
		gf.measure(text.substr(static_cast<size_t>(line_start),
							 static_cast<size_t>(break_at - line_start))
						   .c_str(),
				1.0f, 1.0f, &lw, &lh);
		if (lh <= 0) {
			int tw = 0;
			gf.measure("W", 1.0f, 1.0f, &tw, &lh);
		}
		heights.push_back(lh);
		if (break_at >= len) {
			break;
		}
		last_space = 0;
		line_start = next;
		i = next;
	}
	const int total = static_cast<int>(heights.size());
	*total_lines = total;
	// fit = the last n with accumH(lines 1..n-1) + 2*h_n <= rectH; the final
	// line only needs accumH(all but last) <= rectH [orig: @ 0x653b90].
	int accum_h = 0;
	int fit = 0;
	for (int n = 1; n <= total; ++n) {
		const int h_n = heights[static_cast<size_t>(n - 1)];
		if (n == total) {
			if (accum_h <= rect_h) {
				fit = total;
			}
			break;
		}
		if (accum_h + 2 * h_n <= rect_h) {
			fit = n;
		}
		accum_h += h_n;
	}
	*fit_lines = fit;
	return true;
}


} // namespace opennova::menu
