// Pins the loading screen spec's shell-facing rules (runtime/hud/
// loading_screen.h): the .bms -> .pcx sidecar name, the present-due rule and
// the composited resource names.

#include <runtime/hud/loading_screen.h>

#include "common/test_font.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool ok, const char *what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

} // namespace

int main() {
	using namespace opennova::hud;

	// The sidecar rule: the file part, its extension replaced or appended.
	check(loading_sidecar_image_name("00TRg.bms") == "00TRg.pcx", "sidecar .bms");
	check(loading_sidecar_image_name("TDH_I5A.BMS") == "TDH_I5A.pcx", "sidecar .BMS");
	check(loading_sidecar_image_name("dvxi5") == "dvxi5.pcx", "sidecar appended");
	check(loading_sidecar_image_name("maps/ASH_I5A.bms") == "ASH_I5A.pcx", "sidecar strips dir");
	check(loading_sidecar_image_name("maps\\ASH_I5A.bms") == "ASH_I5A.pcx",
			"sidecar strips backslash dir");
	// The swap replaces from the FIRST '.' left after the last one is cut
	// [orig: Path_RemoveExtension @ 0x521d66; Path_ReplaceOrAppendExtension's
	// scan @ 0x53c7c4].
	check(loading_sidecar_image_name("op.v2.bms") == "op.pcx", "sidecar cut at the first dot");

	// The present-due rule pumps on its interval or a real checkpoint change;
	// repeated presents never invent work between checkpoints.
	check(loading_present_due(100, false), "due at the interval");
	check(!loading_present_due(99, false), "not due under the interval");
	check(loading_present_due(0, true), "due when reported changed");
	check(!loading_present_due(0, false), "unchanged checkpoints are not due early");

	// The composited resource names.
	check(std::strcmp(kLoadingFallbackImage, "loadscrn.pcx") == 0, "fallback image");
	check(std::strcmp(kLoadingFontSmall, "Arials18.fnt") == 0, "small font");
	check(std::strcmp(kLoadingFontLarge, "Arial22.fnt") == 0, "large font");
	check(std::strcmp(kLoadingServerMessageLabelKey, "LT_SERVERMSG") == 0, "label key");
	check(std::strcmp(kLoadingServerMessageLabelFallback, "Message from Game Server") == 0,
			"label fallback");

	// --- the wrapped text block [orig: Render_DrawWrappedTextBlockEx @ 0x580eb0] ---
	// A monospace measure (8 px per byte) so every box width derives from the
	// same rule the assertions pin: the BREAK RULE, not a glyph metric.
	const TextExtent mono = [](const std::string &s) { return 8.0f * static_cast<float>(s.size()); };
	const auto wrap = [&](const char *text, int max_width) {
		return wrap_text_lines(mono, text, max_width);
	};
	// The overflowing prefix rewinds to the LAST SPACE on the line, and the
	// space itself is consumed with the break.
	{
		const auto lines = wrap("aaa bbb ccc", 8 * 10 - 1);
		check(lines.size() == 2 && lines[0] == "aaa bbb" && lines[1] == "ccc",
				"wrap breaks at the last space");
	}
	// With no space to rewind to, the line breaks at the overflowing character,
	// which is dropped like every other break character.
	{
		const auto lines = wrap("abcdef", 8 * 4 - 1);
		check(lines.size() == 2 && lines[0] == "abc" && lines[1] == "ef",
				"wrap without a space drops the overflowing character");
	}
	// A carriage return is the hard break; a line feed is swallowed only when it
	// trails one, and is an ordinary glyph otherwise.
	{
		const auto cr = wrap("one\rtwo", 400);
		check(cr.size() == 2 && cr[0] == "one" && cr[1] == "two", "\\r is the hard break");
		const auto crlf = wrap("one\r\ntwo", 400);
		check(crlf.size() == 2 && crlf[0] == "one" && crlf[1] == "two",
				"the \\n right after the break is swallowed");
		check(wrap("one\ntwo", 400).size() == 1, "a bare \\n never breaks a line that still fits");
		const auto dbl = wrap("one\r\rtwo", 400);
		check(dbl.size() == 3 && dbl[0] == "one" && dbl[1].empty() && dbl[2] == "two",
				"two carriage returns leave an empty line between them");
	}
	// A multi-byte character never splits: the overflow break drops it whole.
	{
		const auto lines = wrap("ab\xC3\xA9" "cd", 8 * 3 - 1); // "abécd", the é is 2 bytes
		check(lines.size() == 2 && lines[0] == "ab" && lines[1] == "cd",
				"a multi-byte overflow character is dropped whole");
	}
	check(wrap("abc", 0).empty(), "a zero box wraps nothing");
	check(wrap("", 100).empty(), "empty text wraps nothing");
	check(wrap_text_lines(TextExtent{}, "abc", 100).empty(), "no measure, no lines");

	// The placer: an empty line advances HALF the line height, a drawn line the
	// whole one; skipped lines cost no vertical space; alignment places each
	// line from its own width; the box stops once the NEXT line would pass
	// `bottom`, the final line never trips it, and top == bottom disables the clip.
	{
		const TextBlock b = layout_text_block(mono, 20, "one\r\rtwo", 0, 0, 400, 0, TextBlockAlign::kLeft);
		check(b.lines.size() == 2, "the blank line is consumed, never placed");
		check(b.lines[0].y == 0.0f && b.lines[1].y == 30.0f,
				"one full line plus the blank line's half advance");
		check(b.stopped_at == 0, "consumed");
	}
	{
		const TextBlock b = layout_text_block(mono, 20, "one\rtwo\rthree", 0, 0, 400, 0,
				TextBlockAlign::kLeft, 2);
		check(b.lines.size() == 1 && b.lines[0].text == "three" && b.lines[0].y == 0.0f,
				"skipped lines are consumed without advancing");
	}
	{
		const int left = 20;
		const int right = left + 400;
		const float text_w = 8.0f * 3.0f;
		check(layout_text_block(mono, 20, "one", left, 0, right, 0, TextBlockAlign::kLeft).lines[0].x == 20.0f,
				"left alignment starts at rect_left");
		check(layout_text_block(mono, 20, "one", left, 0, right, 0, TextBlockAlign::kCenter).lines[0].x ==
						static_cast<float>(left + (400 >> 1)) - text_w * 0.5f,
				"centre alignment centres on left + width/2");
		check(layout_text_block(mono, 20, "one", left, 0, right, 0, TextBlockAlign::kRight).lines[0].x ==
						static_cast<float>(right) - text_w,
				"right alignment ends on rect_right");
	}
	{
		const TextBlock clipped = layout_text_block(mono, 20, "one\rtwo\rthree\rfour", 0, 0, 400, 40,
				TextBlockAlign::kLeft);
		check(clipped.stopped_at == 2 && clipped.lines.size() == 2, "two lines fit in a two-line box");
		const TextBlock open = layout_text_block(mono, 20, "one\rtwo\rthree\rfour", 0, 0, 400, 0,
				TextBlockAlign::kLeft);
		check(open.stopped_at == 0 && open.lines.size() == 4, "top == bottom disables the clip");
		const TextBlock last = layout_text_block(mono, 20, "one\rtwo", 0, 0, 400, 40, TextBlockAlign::kLeft);
		check(last.stopped_at == 0 && last.lines.size() == 2, "the final line never trips the bottom test");
	}
	check(layout_text_block(mono, 20, "one", 10, 0, 10, 0, TextBlockAlign::kLeft).lines.empty(),
			"a zero-width box lays out nothing");

	// The splash's continue line (D-LOADSCR-10): at 1280 x 720 the design point (512, 730) lands at
	// (640, 684) through the integer rounding, the run centred on it at the large slot's 1280 / 800, each
	// glyph at the phase's halved colour, the raw diffuse the font page's MODULATE2X doubles.
	{
		const opennova::fnt::fnt_font_t uniform = test_font::uniform_test_font();
		GameFont font;
		font.set_font(&uniform);
		const GameFontRun on = splash_continue_run(font, "GO", 1280, 720, true);
		check(on.quads.size() == 2, "the line's two glyphs");
		if (on.quads.size() == 2) {
			const float left = on.quads[0].x_top_left + 0.5f, right = on.quads[1].x_top_right + 0.5f;
			check(on.quads[0].y_top == 684.0f - 0.5f, "the line's top at (730 * 720 + 384) / 768");
			check(left < 640.0f && right > 640.0f && (left + right) * 0.5f > 638.0f &&
			              (left + right) * 0.5f < 642.0f,
			      "centred on (512 * 1280 + 512) / 1024");
			// The glyph's 16 texel rows and the drawer's half-texel bottom bias
			// (D-FNT-6), at the slot's scale.
			check(std::fabs(on.quads[0].y_bottom - on.quads[0].y_top - 16.5f * 1.6f) < 0.01f,
			      "the glyph at the slot's 1280 / 800");
			check(on.quads[0].color == 0xFF7F7F7Fu && on.quads[1].color == 0xFF7F7F7Fu,
			      "the white phase halved, alpha forced");
		}
		const GameFontRun off = splash_continue_run(font, "GO", 1280, 720, false);
		check(!off.quads.empty() && off.quads[0].color == 0xFF7F4040u, "the red phase halved");
		check(splash_continue_run(font, "GO", 0, 720, true).quads.empty(), "no display, no line");
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("loading_screen_test: ok");
	return EXIT_SUCCESS;
}
