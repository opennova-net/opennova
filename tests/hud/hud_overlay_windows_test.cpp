// The key-toggled overlay windows (hud/hud_overlay_windows.h +
// hud_frame_overlay_windows.cpp): the block wrapper's line breaks, skips and
// page overflow [orig: HUD_DrawWrappedText @0x580C00], the briefing pages
// [orig: sub_5B9150 @0x5B9150], and the briefing / help / map-legend
// elements riding the draw list's top layer even at the blank level
// [orig: HUD_DrawGameplayOverlays @0x5BDE60; HelpScreen_Draw @0x497800;
// HUD_DrawHelpScreenIcons @0x497480].
#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_overlay_windows.h>

#include "common/test_font.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::hud;
using opennova::fnt::fnt_font_t;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

int text_w(const GameFont &font, const std::string &s) {
	int w = 0;
	int h = 0;
	font.measure(s.c_str(), 1.0f, 1.0f, &w, &h);
	return w;
}

void test_wrapped_text_layout(const fnt_font_t *fnt) {
	GameFont font;
	font.set_font(fnt);
	const int line_h = static_cast<int>(font.char_height('I', 1.0f));
	CHECK(line_h > 0);
	// A budget that fits "aaa bbb" but not "aaa bbb ccc": the break lands on
	// the last space and the space is consumed.
	const std::string text = "aaa bbb ccc";
	const int budget = text_w(font, "aaa bbb") + 1;
	std::vector<HudWrappedLine> lines;
	CHECK(wrapped_text_layout(font, 1.0f, 1.0f, text, 0, 100, budget, 1000, 0, lines) == 0);
	CHECK(lines.size() == 2);
	if (lines.size() == 2) {
		CHECK(text.substr(lines[0].begin, lines[0].end - lines[0].begin) == "aaa bbb");
		CHECK(text.substr(lines[1].begin, lines[1].end - lines[1].begin) == "ccc");
		CHECK(lines[0].y == 100 && lines[1].y == 100 + line_h);
	}
	// A CR/LF ends a line; an empty line steps half a line.
	const std::string crlf = "aa\r\n\r\nbb";
	CHECK(wrapped_text_layout(font, 1.0f, 1.0f, crlf, 0, 0, 1000, 1000, 0, lines) == 0);
	CHECK(lines.size() == 2);
	if (lines.size() == 2) {
		CHECK(crlf.substr(lines[1].begin, lines[1].end - lines[1].begin) == "bb");
		CHECK(lines[1].y == line_h + (line_h >> 1));
	}
	// Skipped lines count but neither draw nor advance.
	CHECK(wrapped_text_layout(font, 1.0f, 1.0f, text, 0, 50, budget, 1000, 1, lines) == 0);
	CHECK(lines.size() == 1 && lines[0].y == 50);
	// A limit that holds one line returns the index of the next.
	CHECK(wrapped_text_layout(font, 1.0f, 1.0f, text, 0, 0, budget, line_h + 1, 0, lines) == 1);
	CHECK(lines.size() == 1);
	// No space: the overflowing character breaks the line and is dropped.
	const std::string word = "abcdef";
	const int tight = text_w(font, "abc");
	CHECK(wrapped_text_layout(font, 1.0f, 1.0f, word, 0, 0, tight, 1000, 0, lines) == 0);
	CHECK(lines.size() == 2);
	if (lines.size() == 2) {
		CHECK(word.substr(lines[0].begin, lines[0].end - lines[0].begin) == "abc");
		CHECK(word.substr(lines[1].begin, lines[1].end - lines[1].begin) == "ef");
	}
}

void test_briefing_pages() {
	HudBriefingPages pages;
	pages.cycle(1, false);
	CHECK(pages.page == 0); // no next start yet
	pages.starts[1] = 12;
	pages.cycle(1, false);
	CHECK(pages.page == 1);
	pages.cycle(1, false);
	CHECK(pages.page == 1); // starts[2] unset
	pages.cycle(-1, false);
	CHECK(pages.page == 0);
	pages.cycle(-1, false);
	CHECK(pages.page == 0);
	// A session's end-game screen wraps four pages both ways.
	pages.cycle(-1, true);
	CHECK(pages.page == 3);
	pages.cycle(1, true);
	CHECK(pages.page == 0);
	pages.page = 2;
	pages.cycle(0, false);
	CHECK(pages.page == 0 && pages.starts[1] == 0);
}

void configure(HudFrameCompiler &compiler, const fnt_font_t *font) {
	HudLayout layout;
	layout.box_texture_valid = true;
	layout.box_tex_w = 128;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
}

size_t box_quads_from(const HudDrawList &list, size_t begin) {
	size_t n = 0;
	for (size_t i = begin; i < list.quads.size(); ++i)
		if (list.quads[i].texture == kHudTexBoxBorder) ++n;
	return n;
}

void test_windows_ride_the_top_layer(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	configure(compiler, font);
	HudFrameState state;
	{
		const HudDrawList &none = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(box_quads_from(none, 0) == 0);
		CHECK(none.top_begin.quads == none.quads.size());
	}
	// The help list: its box and 23 rows of text land after top_begin.
	state.help_screen.shown = true;
	state.help_screen.title = "Help - Movement";
	state.help_screen.page_line = "Page 1 of 7";
	state.help_screen.footer = "PgUp and PgDn to change pages";
	state.help_screen.keys.assign(3, "W");
	state.help_screen.texts.assign(3, "Forward");
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(box_quads_from(list, list.top_begin.quads) > 0);
		CHECK(list.glyphs.size() > list.top_begin.glyphs);
	}
	// The blank level still draws the windows [orig: the level-3 jump
	// @0x5bdeaf lands before them].
	state.hud_detail_level = 3;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(box_quads_from(list, list.top_begin.quads) > 0);
	}
	state.hud_detail_level = 0;
	// The map legend wins over the help list: two textured tris per strip
	// entry, the medic entry as the three cross rects.
	state.map_legend.shown = true;
	state.map_legend.title = "Map Legend";
	state.map_legend.labels.assign(kHudMapLegendIconCount, "label");
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		size_t icon_tris = 0;
		for (size_t i = list.top_begin.tris; i < list.tris.size(); ++i)
			if (list.tris[i].texture == kHudTexMapIcons) ++icon_tris;
		CHECK(icon_tris == static_cast<size_t>(2 * (kHudMapLegendIconCount - 1)));
		size_t red = 0;
		for (size_t i = list.top_begin.quads; i < list.quads.size(); ++i)
			if (list.quads[i].color == 0xFFFF0000u) ++red;
		CHECK(red == 2); // the cross's two bars
	}
	state.map_legend.shown = false;
	state.help_screen.shown = false;
	// The briefing: a text taller than the panel overflows into page 1.
	std::string text;
	for (int i = 0; i < 400; ++i) text += "line\r\n";
	state.briefing.shown = true;
	state.briefing.text = text;
	compiler.briefing_pages().reset();
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(box_quads_from(list, list.top_begin.quads) > 0);
		CHECK(list.glyphs.size() > list.top_begin.glyphs);
	}
	CHECK(compiler.briefing_pages().starts[1] > 0);
	compiler.briefing_pages().cycle(1, false);
	CHECK(compiler.briefing_pages().page == 1);
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.glyphs.size() > list.top_begin.glyphs);
	}
}

// The briefing's inline markup (D-HUD-50): the wrapper's state block leaves
// its inert byte unwritten and the panel's residue there is not zero, so
// retail consumes every tag and draws the briefing plain in the halved
// colour, the markup neither shown nor underlined nor coloured
// [orig: HUD_DrawWrappedText @0x580caa..0x580cce; GText_ParseFormatTag
// @0x6743b0]. Retail's own briefing2 texts (CP01, 00TRa) carry this markup.
void test_briefing_markup_draws_plain(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	configure(compiler, font);
	HudFrameState state;
	state.briefing.shown = true;
	state.briefing.text = " <ucFC8932>Goals: <-uco>\r\n\r\n<cFC8932> 1.<-co> Move";
	compiler.briefing_pages().reset();
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// " Goals: " (8), " 1." (3) and " Move" (5): the tags draw nothing.
	CHECK(list.glyphs.size() - list.top_begin.glyphs == 16);
	CHECK(list.underlines.size() == list.top_begin.underlines);
	for (size_t i = list.top_begin.glyphs; i < list.glyphs.size(); ++i)
		CHECK(list.glyphs[i].color == 0xFF7F7F7Fu);
}

} // namespace

int main() {
	fnt_font_t font = test_font::uniform_test_font();
	test_wrapped_text_layout(&font);
	test_briefing_pages();
	test_windows_ride_the_top_layer(&font);
	test_briefing_markup_draws_plain(&font);
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_overlay_windows_test OK\n");
	return 0;
}
