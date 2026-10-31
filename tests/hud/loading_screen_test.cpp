// Pins the loading screen spec's shell-facing rules (runtime/hud/
// loading_screen.h): the .bms -> .pcx sidecar name, the present-due rule, the
// composited resource names, the wrapped block and the session block's
// composite into the background.

#include <base/title/title.h> // the splash's large slot (Title::hud_fonts)
#include <runtime/hud/hud_math.h> // loading_bar_fill_span
#include <runtime/hud/loading_screen.h>
#include <runtime/hud/cine_text.h>
#include <runtime/world/epilog_cine.h>

#include <formats/fnt/fnt_stroke_font.h>

#include "common/test_font.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
	// The breaker walks bytes, as the game's code-page text: a two-byte pair splits, its
	// first byte the overflow the break drops [orig: the walk @0x580f77..0x581139].
	{
		const auto lines = wrap("a\xC3\xA9" "b", 8 * 2 - 1);
		check(lines.size() >= 2 && lines[0] == "a" && lines[1] == "\xA9",
				"the breaker walks bytes");
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
		const opennova::Title &jo = *opennova::title_by_code("jo"); // the large slot over 800
		const GameFontRun on = splash_continue_run(font, "GO", 1280, 720, true, jo);
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
		const GameFontRun off = splash_continue_run(font, "GO", 1280, 720, false, jo);
		check(!off.quads.empty() && off.quads[0].color == 0xFF7F4040u, "the red phase halved");
		check(splash_continue_run(font, "GO", 0, 720, true, jo).quads.empty(),
		      "no display, no line");
	}

	// The epilog cine's text through the large slot's glyph runs (D-LOADSCR-2's epilog screen): a
	// TextFade's box over the viewport, its lines wrapped and centred on the box's middle in the
	// node's colour halved keeping its alpha, so the line fades; a counter's label left and value
	// right on its row, opaque half-white; the count-up column only while it runs.
	// [orig: CCineEventTextFade_Draw @0x573020 -> HUD_DrawWrappedText @0x580C00 (mode 4);
	//  CineEventEpilogCounter_Draw @0x573440]
	{
		const opennova::fnt::fnt_font_t uniform = test_font::uniform_test_font();
		GameFont font;
		font.set_font(&uniform);
		opennova::world::CineEvent line;
		line.kind = opennova::world::CineEventKind::TextFade;
		line.x = 0;
		line.y = 120;
		line.box_width = 1024;
		line.box_height = 100;
		line.alpha = 0.5f;
		line.color_mask = 0xFFFFFFu;
		check(opennova::world::cine_text_fade_argb(line) == 0x7FFFFFFFu,
		      "the alpha byte over the colour");
		check(opennova::world::cine_event_draw_argb(line) == 0x7F7F7F7Fu, "halved, the alpha kept");
		const CineViewport screen{0, 0, 1024, 768};
		const GameFontRun run = cine_event_text_run(line, "GO", font, 1.0f, screen);
		check(run.quads.size() == 2, "the line's two glyphs");
		if (run.quads.size() == 2) {
			const float left = run.quads[0].x_top_left + 0.5f;
			const float right = run.quads[1].x_top_right + 0.5f;
			check(run.quads[0].y_top == 120.0f - 0.5f, "from the box's top");
			check((left + right) * 0.5f > 510.0f && (left + right) * 0.5f < 514.0f,
			      "centred on the box's middle, x 0 + (1024 >> 1)");
			check(run.quads[0].color == 0x7F7F7F7Fu, "the faded colour");
		}
		// Over a 1280 x 720 viewport the box lands at y = 120 / 768 * 720, truncated.
		const CineViewport hd{0, 0, 1280, 720};
		const GameFontRun wide = cine_event_text_run(line, "GO", font, 1.0f, hd);
		check(!wide.quads.empty() && wide.quads[0].y_top == 112.0f - 0.5f,
		      "the box's top truncated");
		// A box too narrow for both words wraps them onto two lines, one line height apart.
		line.box_width = 30;
		const GameFontRun wrapped = cine_event_text_run(line, "GO ON", font, 1.0f, screen);
		check(wrapped.quads.size() == 4 && wrapped.quads[2].y_top > wrapped.quads[0].y_top,
		      "two lines");

		opennova::world::CineEvent counter;
		counter.kind = opennova::world::CineEventKind::EpilogCounter;
		counter.label_x = 200;
		counter.value_x = 800;
		counter.count_x = 900;
		counter.row_y = 300;
		counter.value = 3;
		counter.max = 5;
		const GameFontRun row = cine_event_text_run(counter, "AB", font, 1.0f, hd);
		// "AB" plus "3/5", no count-up column (its target and rate both -1).
		check(row.quads.size() == 5, "the label and the value, no count");
		if (row.quads.size() == 5) {
			check(row.quads[0].x_top_left == 250.0f - 0.5f, "the label at 200 / 1024 * 1280");
			check(row.quads[0].y_top == 281.0f - 0.5f, "the row at 300 / 768 * 720, truncated");
			check(row.quads[4].x_top_right + 0.5f < 1001.0f && row.quads[4].x_top_right > 990.0f,
			      "the value ends at 800 / 1024 * 1280");
			check(row.quads[0].color == 0xFF7F7F7Fu && row.quads[4].color == 0xFF7F7F7Fu,
			      "opaque half-white");
		}
		check(cine_epilog_count_text(-1, -1, 7).empty() && cine_epilog_count_text(10, -1, 7) == "7",
		      "the count-up column runs on a target or a rate");
		opennova::world::CineEvent image;
		image.kind = opennova::world::CineEventKind::ImageFade;
		check(cine_event_text_run(image, "X", font, 1.0f, screen).quads.empty(),
		      "an image draws no text");
	}

	// The progress bar's rects: at the design size the placement is the
	// virtual one; the three borders inset by one; the fill from x + 3 to the
	// span's right edge, empty at 0 and stopping inside the track at 100.
	{
		const LoadingBarRects at0 = loading_bar_rects(1024, 768, 0);
		check(at0.outer.x == kLoadingBarX && at0.outer.y == kLoadingBarY &&
				      at0.outer.w == kLoadingBarW + 6 && at0.outer.h == kLoadingBarH + 6,
		      "the outer rect at the design size");
		check(at0.gray.x == kLoadingBarX + 1 && at0.gray.w == kLoadingBarW + 4 &&
				      at0.track.y == kLoadingBarY + 2 && at0.track.h == kLoadingBarH + 2,
		      "the gray and track insets");
		check(at0.fill.x == kLoadingBarX + 3 && at0.fill.y == kLoadingBarY + 3 &&
				      at0.fill.h == kLoadingBarH && at0.fill.w == 0,
		      "the empty fill at 0");
		const LoadingBarRects full = loading_bar_rects(1024, 768, 100);
		check(full.fill.w == kLoadingBarW, "the full fill spans the bar's width");
		const LoadingBarSpan span = loading_bar_fill_span(kLoadingBarX, kLoadingBarW, 37);
		const LoadingBarRects part = loading_bar_rects(1024, 768, 37);
		check(part.fill.x == span.left && part.fill.w == span.right - span.left,
		      "the fill's edges are loading_bar_fill_span's");
		// An 800 x 640 surface through Viewport_ScaleToVirtualCoords' integer
		// round-half-up: x (368 * 800 + 512) / 1024 = 288, y (732 * 640 + 384) /
		// 768 = 610, w (286 * 800 + 512) / 1024 = 223, h (15 * 640 + 384) / 768
		// = 13 (a float ratio truncated gives 287 / 609 / 223 / 12).
		const LoadingBarRects small = loading_bar_rects(800, 640, 100);
		check(small.outer.x == 288 && small.outer.y == 610 && small.outer.w == 229 &&
				      small.outer.h == 19,
		      "the placement at 800 x 640 rounds as retail's integer scaler");
		check(small.fill.x == 291 && small.fill.y == 613 && small.fill.w == 223 &&
				      small.fill.h == 13,
		      "the full fill at 800 x 640");
		// 1600 x 900: y (732 * 900 + 384) / 768 = 858, h (15 * 900 + 384) / 768
		// = 18 (the float ratio truncated gives 857 / 17).
		const LoadingBarRects wide = loading_bar_rects(1600, 900, 0);
		check(wide.outer.x == 575 && wide.outer.y == 858 && wide.outer.w == 447 + 6 &&
				      wide.outer.h == 18 + 6,
		      "the placement at 1600 x 900 rounds as retail's integer scaler");
	}

	// --- the start-mission splash [orig: Game_ShowStartMissionSplash @ 0x520820] ---
	{
		StartMissionSplash splash;
		check(splash.state() == StartMissionSplash::State::kNone && !splash.draws_overlay(),
				"no splash until raised");
		check(!splash.exit(true) && !splash.frame(1, true), "nothing exits or steps unraised");
		check(splash.raise() && !splash.raise(), "raised once");
		check(splash.draws_overlay() && !splash.frame(0, true), "a frame with no button holds");
		// A press: one background-only frame, then the release.
		check(splash.exit(true) && !splash.exit(true), "the press exits once");
		check(splash.state() == StartMissionSplash::State::kClosing && !splash.draws_overlay(),
				"the closing frame draws the background alone");
		check(!splash.frame(0, true), "the closing frame presents");
		check(splash.frame(0, true) && splash.state() == StartMissionSplash::State::kNone,
				"then the release");
		// A held button exits on its frame, the closing frame after it.
		splash.raise();
		check(!splash.frame(1, true) && splash.state() == StartMissionSplash::State::kClosing,
				"a held button exits");
		check(!splash.frame(1, true) && splash.frame(1, true),
				"its closing frame, then the release");
		// No renderer to wait on: the release on the next frame.
		splash.raise();
		check(splash.exit(false) && splash.frame(0, false), "no closing frame to present");
	}
	check(splash_blink_on(0x200) && splash_blink_on(0x3FF) && !splash_blink_on(0x1FF) &&
			      !splash_blink_on(0x400),
	      "the continue line rides GetTickCount()'s bit 0x200");
	{
		// The arrow's sides divide as integers: 32 * 1024 / 800 = 40 (40.96), 32 * 768 / 600 = 40.
		const SplashArrowSize at1024 = splash_arrow_size(32, 32, 1024, 768);
		check(at1024.width == 40 && at1024.height == 40, "the arrow at 1024 x 768");
		const SplashArrowSize at800 = splash_arrow_size(32, 32, 800, 600);
		check(at800.width == 32 && at800.height == 32, "the arrow at its art's size at 800 x 600");
	}
	{
		// The session block over an 800 x 600 custom background (band right 661).
		const LoadingSessionBoxes boxes = loading_session_boxes(800, 600, kLoadingBandRightCustom);
		check(boxes.title.x == 21 && boxes.title.y == 29 && boxes.title.width == 640 &&
				      boxes.title.bottom == 500 && boxes.title.align == TextBlockAlign::kLeft &&
				      boxes.title.argb == 0xFFFFFFFFu,
		      "the title on the band, left");
		check(boxes.mission.align == TextBlockAlign::kCenter &&
				      boxes.game_type.align == TextBlockAlign::kRight &&
				      boxes.mission.width == 640 && boxes.game_type.bottom == 500,
		      "the mission centred and the game type right on the same band");
		check(boxes.message_label.x == 16 && boxes.message_label.width == 768 &&
				      boxes.message_label.y == 522 && boxes.message_label.bottom == 600 &&
				      boxes.message_label.argb == 0xFF80E0FFu,
		      "the message label at the image fractions, light blue");
		check(boxes.message_body.y == 540 && boxes.message_body.argb == 0xFFFFFFFFu,
		      "the message body under it, white");
		check(loading_message_label_text("Message from Game Server") ==
				      "Message from Game Server:",
		      "the label's colon");
	}

	// --- the session block's composite (D-LOADSCR-2) [orig: CGameFont_DrawBlendedGlyph
	// @0x6744a0; CGameFont_RenderText @0x675e40; Render_DrawWrappedTextBlockEx @0x580eb0;
	// Render_LoadingScreen @0x521fe1-0x522123] ---
	{
		// A blank font whose 'A' is two texels wide and one tall at the page's corner.
		opennova::fnt::fnt_font_t font{};
		check(opennova::fnt::fnt_init_blank(&font, 1, 2) == opennova::fnt::FNT_OK,
				"the blank font");
		opennova::fnt::fnt_glyph_t &a = font.glyphs['A' - opennova::fnt::FNT_FIRST_CHAR];
		a.page = 0;
		a.uv = { 0.0f, 0.0f, 2.0f / 256.0f, 1.0f / 256.0f };
		const auto texel = [&font](int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t al) {
			uint8_t *p = font.pages + (static_cast<size_t>(y) * 256 + x) * 4;
			p[0] = r;
			p[1] = g;
			p[2] = b;
			p[3] = al;
		};
		texel(0, 0, 255, 255, 255, 128);
		texel(1, 0, 255, 255, 255, 255);
		texel(0, 1, 100, 50, 200, 255); // the row one past the glyph's extent
		std::vector<uint8_t> pixels(16 * 16 * 4);
		const auto reset = [&pixels]() {
			for (size_t i = 0; i < pixels.size(); i += 4) {
				pixels[i] = 10;
				pixels[i + 1] = 20;
				pixels[i + 2] = 30;
				pixels[i + 3] = 255;
			}
		};
		const auto px = [&pixels](int x, int y) {
			const uint8_t *p = &pixels[(static_cast<size_t>(y) * 16 + x) * 4];
			return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
		};
		const auto rgb = [](uint32_t r, uint32_t g, uint32_t b) {
			return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
		};
		const uint32_t base = rgb(10, 20, 30);
		LoadingSurface surface{ pixels.data(), 16, 16 };

		// The blend: per channel ((256 - A) * d * 256 + A * c * t) >> 16, blue's product cut
		// to 8 bits first; the rows run one past the extent; an alpha-0 texel keeps the pixel.
		reset();
		loading_blend_glyph(surface, font, a, 5, 7, 0xFF8040C0u);
		check(px(5, 7) == rgb(68, 41, 110), "a half-alpha texel's blend");
		check(px(6, 7) == rgb(127, 63, 190), "an opaque texel's blend");
		check(px(5, 8) == rgb(49, 12, 149), "the row past the extent blends too");
		check(px(6, 8) == base && px(7, 7) == base && px(5, 9) == base,
				"an alpha-0 texel and the pixels off the glyph keep theirs");

		// The walk: each glyph at the cursor truncated, the cursor stepping
		// floor(cursor + 2 + (spacing - 1) + 0.5).
		GameFontState state;
		state.color = state.original_color = 0xFFFFFFFFu;
		reset();
		loading_render_text(surface, font, 0, "AA", 5.7f, 0.0f, 0, state);
		check(px(5, 0) != base && px(6, 0) != base && px(7, 0) == base && px(8, 0) == base &&
						px(9, 0) != base && px(10, 0) != base,
				"glyphs at 5 and floor(5.7 + 3.5) = 9");
		// Centred on 10: the measure 5, the cursor 7.5, the glyphs at 7 and 11.
		reset();
		loading_render_text(surface, font, 0, "AA", 10.0f, 0.0f, 1, state);
		check(px(6, 0) == base && px(7, 0) != base && px(10, 0) == base && px(11, 0) != base,
				"a centred line from the anchor less half its measure");
		// A tab steps to the next stop from the anchor; with no stop width it is a space.
		reset();
		GameFontState tabbed = state;
		tabbed.tab_width = 8;
		loading_render_text(surface, font, 8, "A\tA", 2.0f, 0.0f, 0, tabbed);
		check(px(9, 0) == base && px(10, 0) != base, "the tab steps to 2 + 8");
		reset();
		loading_render_text(surface, font, 0, "A\tA", 2.0f, 0.0f, 0, state);
		check(px(5, 0) == base && px(6, 0) != base, "a tab with no stop is the space (width 0)");
		// Bold strikes again one right; the state comes back with the tag's style.
		reset();
		GameFontState styled = state;
		loading_render_text(surface, font, 0, "<B>A", 0.0f, 0.0f, 0, styled);
		check(px(2, 0) != base && styled.bold, "bold strikes again at x + 1 and stays on");

		// A block's line that measures nothing still runs its tags: green carries to the
		// next line.
		reset();
		const LoadingTextBox box{ 2, 3, 12, 16, TextBlockAlign::kLeft, 0xFFFFFFFFu };
		loading_draw_text_block(surface, font, 0, "<C00FF00>\rA", box);
		check(px(3, 3) == rgb(0, 253, 0), "the tag line's green draws the next line");
		opennova::fnt::fnt_free(&font);
	}
	{
		// The session block over a black 800 x 600 background with the stroke font: the title
		// on the band, nothing under it without a server message, the message under it with one.
		opennova::fnt::fnt_font_t font{};
		check(opennova::fnt::stroke_font::build_font(&font) == opennova::fnt::FNT_OK,
				"the stroke font");
		const auto composite = [&font](const char *message) {
			std::vector<uint8_t> image(800 * 600 * 4, 0);
			LoadingSurface surface{ image.data(), 800, 600 };
			composite_loading_session_text(surface, &font, &font, "HOST", "", "", "Message",
					message, kLoadingBandRightCustom);
			return image;
		};
		const auto lit = [](const std::vector<uint8_t> &image, int x0, int y0, int x1, int y1) {
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x)
					if (image[(static_cast<size_t>(y) * 800 + x) * 4] != 0) return true;
			return false;
		};
		const std::vector<uint8_t> quiet = composite("");
		check(lit(quiet, 21, 29, 200, 80), "the title blends into the band");
		check(!lit(quiet, 0, 505, 800, 600), "no server message, nothing under the band");
		const std::vector<uint8_t> told = composite("HI");
		check(lit(told, 16, 522, 400, 600), "the server message under the band");
		opennova::fnt::fnt_free(&font);
	}

	if (g_failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::puts("loading_screen_test: ok");
	return EXIT_SUCCESS;
}
