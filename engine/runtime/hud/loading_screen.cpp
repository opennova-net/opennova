// The loading screen's wrapped text block (loading_screen.h): the line breaker
// and the line placer of render_draw_wrapped_text_block_ex, ported break rule
// for break rule. Pushed down from the Godot shell (ADR 0040 ladder E2); the
// embedder supplies the width measure and paints the placed lines.
#include <runtime/hud/loading_screen.h>

#include <cstddef>
#include <utility>

namespace opennova::hud {

namespace {

// The byte length of the UTF-8 sequence that starts at `lead` (1 for ASCII and
// for a stray continuation byte), clamped to what is left of the string. The
// breaker walks the text one character at a time the way retail walks its
// single-byte text, so a break never splits a multi-byte character.
std::size_t utf8_sequence_length(const std::string &text, std::size_t at) {
	const unsigned char lead = static_cast<unsigned char>(text[at]);
	std::size_t len = 1;
	if ((lead & 0xE0u) == 0xC0u) len = 2;
	else if ((lead & 0xF0u) == 0xE0u) len = 3;
	else if ((lead & 0xF8u) == 0xF0u) len = 4;
	const std::size_t left = text.size() - at;
	return len < left ? len : left;
}

} // namespace

// The line breaker [orig: render_draw_wrapped_text_block_ex @ 0x580eb0 — the
// 32 / 13 / 10 tests @0x580f88, @0x580fdf, @0x581128]. The two kerning
// parameters offset the first (`use_kerning_start`) and the wrapped
// (`use_kerning_wrap`) lines by the font's tab-width field font+0x168; every
// loading-screen call site passes `use_kerning_start = 0` [orig: @0x521fee,
// @0x52201a, @0x52203c, @0x5220bd] and the field is 0 unless
// GText_SetLineSpacing @ 0x674720 sets one, so the offset folds out here --
// D-LOADSCR-2 carries the residual with the rest of the CGameFont metrics.
std::vector<std::string> wrap_text_lines(const TextExtent &extent, const std::string &text,
		int max_width) {
	std::vector<std::string> out;
	if (!extent || text.empty() || max_width <= 0) return out;
	const std::size_t n = text.size();
	std::size_t line_start = 0;
	std::size_t i = 0;
	std::size_t last_space = 0; // absolute index; 0 means "no usable space on this line"
	int width = 0;              // loop-carried: a NUL/end step re-uses the last measure
	while (i <= n) {
		const unsigned char ch = i < n ? static_cast<unsigned char>(text[i]) : 0;
		const std::size_t ch_len = i < n ? utf8_sequence_length(text, i) : 0;
		if (ch == kTextSpace) last_space = i;
		if (ch != 0) {
			// Measure the whole prefix up to and INCLUDING this character, the
			// way retail NUL-terminates one past it and re-measures the line.
			const std::size_t span = i - line_start + ch_len;
			width = span > 0 ? static_cast<int>(extent(text.substr(line_start, span))) : 0;
		}
		std::size_t break_at = i;
		std::size_t break_len = ch_len;
		if (width <= max_width) {
			// Only '\r' and the terminator break a line that still fits; a '\n'
			// is an ordinary glyph unless it trails a break.
			if (ch != kTextCarriageReturn && ch != 0) {
				i += ch_len;
				continue;
			}
		} else if (last_space != 0) {
			// Overflow with a space on this line: break at the LAST space.
			break_at = last_space;
			break_len = 1;
		}
		// Overflow with no space breaks at the current character, which is then
		// dropped with every other break character.
		out.push_back(text.substr(line_start, break_at - line_start));
		if (break_at >= n) return out;
		line_start = break_at + break_len;
		if (line_start < n && static_cast<unsigned char>(text[line_start]) == kTextLineFeed)
			++line_start; // a '\n' right after the break is swallowed
		i = line_start;
		last_space = 0;
		width = 0;
	}
	return out;
}

// The line placer, the drawing half of the same routine [orig:
// render_draw_wrapped_text_block_ex @ 0x580eb0 — the alignment fold (4 =
// centred on left + width/2 @0x5810ab, 5 = right aligned on rect_right
// @0x581094, else left @0x58107f), the half advance `extent >> 1`
// @0x580f40/@0x581005 against the full advance @0x5810d7, the skip_lines jump
// @0x580ffb, and the ordering of the end-of-text return @0x581155 ahead of the
// bottom test @0x58111e; the line pitch is the 'I' character's own extent
// @0x580f2b].
TextBlock layout_text_block(const TextExtent &extent, int line_height, const std::string &text,
		int left, int top, int right, int bottom, TextBlockAlign align, int skip_lines) {
	TextBlock out;
	const int width = right - left;
	if (!extent || text.empty() || width <= 0) return out;
	const int line_h = line_height;
	const int half_h = line_h >> 1;
	int cursor = top;
	const std::vector<std::string> lines = wrap_text_lines(extent, text, width);
	for (std::size_t i = 0; i < lines.size(); ++i) {
		const std::string &line = lines[i];
		if (static_cast<int>(i) >= skip_lines) {
			const float line_w = line.empty() ? 0.0f : extent(line);
			if (line_w <= 0.0f) {
				cursor += half_h;
			} else {
				float px = static_cast<float>(left);
				if (align == TextBlockAlign::kCenter)
					px = static_cast<float>(left + (width >> 1)) - line_w * 0.5f;
				else if (align == TextBlockAlign::kRight)
					px = static_cast<float>(right) - line_w;
				out.lines.push_back(TextBlockLine{line, px, static_cast<float>(cursor)});
				cursor += line_h;
			}
		}
		// The terminating break reports "consumed"; only a line that still has
		// text behind it can run the box out of room, and top == bottom disables
		// the vertical clip entirely.
		if (i + 1 == lines.size()) return out;
		if (bottom != top && cursor + line_h > bottom) {
			out.stopped_at = static_cast<int>(i) + 1;
			return out;
		}
	}
	return out;
}

} // namespace opennova::hud
