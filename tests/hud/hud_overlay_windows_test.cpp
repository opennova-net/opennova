// The key-toggled overlay windows (hud/hud_overlay_windows.h +
// hud_frame_overlay_windows.cpp): the block wrapper's line breaks, skips and
// page overflow [orig: HUD_DrawWrappedText @0x580C00], the briefing pages
// [orig: HUD_CycleBriefingPage @0x5B9150], and the briefing / help / map-legend
// elements riding the draw list's top layer even at the blank level
// [orig: HUD_DrawGameplayOverlays @0x5BDE60; HelpScreen_Draw @0x497800;
// HUD_DrawHelpScreenIcons @0x497480].
#include <base/title/title.h>
#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_elements.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_overlay_windows.h>

#include "common/test_font.h"

#include <cmath>
#include <cstdint>
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
	layout.box_tex_h = 128;
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
	// The table and its row step are the title's (DFX2-89): Jointops.exe's 26 entries 40 a row,
	// the medic one the cross [orig: HUD_DrawHelpScreenIcons @0x497480 - @0x497524]; dfx2.exe's 20,
	// no medic, 52 a row [orig: sub_497490 @0x497490 - @0x497534 (dfx2.exe)].
	const opennova::Title *jo = opennova::title_by_code("jo");
	const opennova::Title *dfx2 = opennova::title_by_code("dfx2");
	CHECK(jo != nullptr && jo->hud_map_legend != nullptr && jo->hud_map_legend->count == 26 &&
			jo->hud_map_legend->row_step == 40);
	CHECK(dfx2 != nullptr && dfx2->hud_map_legend != nullptr && dfx2->hud_map_legend->count == 20 &&
			dfx2->hud_map_legend->row_step == 52);
	// The label rows' y: the even entries' (left column) labels, top to bottom.
	const auto legend_rows = [&](const opennova::Title *title, size_t *icon_tris, size_t *red) {
		state.title = title;
		state.map_legend.labels.assign(
				title != nullptr && title->hud_map_legend ? title->hud_map_legend->count : 0, "L");
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		*icon_tris = 0;
		for (size_t i = list.top_begin.tris; i < list.tris.size(); ++i)
			if (list.tris[i].texture == kHudTexMapIcons) ++*icon_tris;
		*red = 0;
		for (size_t i = list.top_begin.quads; i < list.quads.size(); ++i)
			if (list.quads[i].color == 0xFFFF0000u) ++*red;
		std::vector<float> ys;
		for (size_t i = list.top_begin.glyphs; i < list.glyphs.size(); ++i) {
			const float y = list.glyphs[i].y_top;
			if (list.glyphs[i].x_top_left < 300.0f && y > 120.0f &&
					(ys.empty() || ys.back() != y))
				ys.push_back(y);
		}
		return ys;
	};
	{
		size_t icon_tris = 0, red = 0;
		const std::vector<float> ys = legend_rows(jo, &icon_tris, &red);
		CHECK(icon_tris == static_cast<size_t>(2 * (26 - 1)));
		CHECK(red == 2); // the cross's two bars
		CHECK(ys.size() == 13 && ys[1] - ys[0] == 40.0f);
	}
	{
		size_t icon_tris = 0, red = 0;
		const std::vector<float> ys = legend_rows(dfx2, &icon_tris, &red);
		CHECK(icon_tris == static_cast<size_t>(2 * 20));
		CHECK(red == 0); // no medic entry
		CHECK(ys.size() == 10 && ys[1] - ys[0] == 52.0f);
	}
	{
		// A title with no legend draws the box alone.
		size_t icon_tris = 0, red = 0;
		CHECK(legend_rows(nullptr, &icon_tris, &red).empty() && icon_tris == 0);
	}
	state.title = nullptr;
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

// The I window in a session, the end-game screen: it stands in for the briefing panel at the
// window's slot; its box alone while the status record is not valid; its page wrapped in the
// bold slot with the 240 tab stop, its tags inert unless the residue reads 0, an overflow
// written as the next page's start; the footer centred at (512, 700) / (512, 720)
// [orig: HUD_DrawGameplayOverlays @0x5be135..0x5be145; HUD_DrawEndGameScreen @0x5bd070 -
//  the gate @0x5bd10f, the tab width @0x5bd12d..0x5bd14a, the wrap @0x5bdd36 and its store
//  @0x5bdd4b, the footer @0x5bdd7d..0x5bde31].
void test_end_game_screen(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	configure(compiler, font);
	HudFrameState state;
	state.briefing.shown = true;
	state.briefing.text = "brief";
	state.end_game.shown = true;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(box_quads_from(list, list.top_begin.quads) > 0);
		CHECK(list.glyphs.size() == list.top_begin.glyphs); // neither the page nor the briefing
	}
	state.briefing.shown = false;
	state.end_game.valid = true;
	// A tab steps to the next 240 from the text's left edge, 212.
	state.end_game.text = "A\tB";
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.glyphs.size() - list.top_begin.glyphs == 2);
		if (list.glyphs.size() - list.top_begin.glyphs == 2) {
			CHECK(std::fabs(list.glyphs[list.top_begin.glyphs].x_top_left - 211.5f) < 1.0f);
			CHECK(std::fabs(list.glyphs[list.top_begin.glyphs + 1].x_top_left - 451.5f) < 1.0f);
		}
	}
	// Inert tags draw plain; a live residue draws <b> bold, a second strike per glyph.
	state.end_game.text = "<b>A<-b>";
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.size() -
					compiler.last_draw_list().top_begin.glyphs == 1);
	state.end_game.tags_live = true;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.size() -
					compiler.last_draw_list().top_begin.glyphs == 2);
	state.end_game.tags_live = false;
	// An overflow is written as the next page's start.
	std::string tall;
	for (int i = 0; i < 400; ++i) tall += "line\r\n";
	state.end_game.text = tall;
	state.end_game.page = 1;
	compiler.briefing_pages().reset();
	compiler.compile(state, 1024.0f, 768.0f);
	CHECK(compiler.briefing_pages().starts[2] > 0);
	CHECK(compiler.briefing_pages().starts[1] == 0);
	// The footer: both lines centred at x 512, at y 700 and 720.
	state.end_game.text.clear();
	state.end_game.page_line = "P";
	state.end_game.footer = "F";
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.glyphs.size() - list.top_begin.glyphs == 2);
		if (list.glyphs.size() - list.top_begin.glyphs == 2) {
			const GameFontQuad &page = list.glyphs[list.top_begin.glyphs];
			const GameFontQuad &hint = list.glyphs[list.top_begin.glyphs + 1];
			CHECK(std::fabs((page.x_top_left + page.x_top_right) * 0.5f - 512.0f) < 1.5f);
			CHECK(std::fabs((hint.x_top_left + hint.x_top_right) * 0.5f - 512.0f) < 1.5f);
			CHECK(std::fabs(hint.y_top - page.y_top - 20.0f) < 0.01f);
		}
	}
}

// The view veil's place in the lists (D-NET-1384): the main HUD (the gameplay overlay pass, its
// ammo count among it) lies under veil_begin, where the device leg lays the damage quads and the
// sun veil; HUD_DrawGameplayOverlays' legs (the frame-rate overlay, then the Tab player list)
// past top_begin, over the big map; and the overlay panels (the pause text) after them, over
// them (D-HUD-73). The Tab list skips a dead local body and the round-over latch. The blank
// level marks the veil with nothing under it and keeps both passes' order.
// [orig: Render_ProcessMainSceneFrame @0x5CA0F0 - HUD_RenderAllOverlays @0x5CAD04, the veil
//  @0x5CAC4B, HUD_BuildMapOverlayView @0x5CAD15, HUD_DrawGameplayOverlays @0x5CAE0B,
//  HUD_DrawOverlayPanels @0x5CAE3B; HUD_DrawGameplayOverlays @0x5be121 / @0x5be124]
void test_view_veil_layer(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	configure(compiler, font);
	HudFrameState state;
	state.ticks = 10;
	state.weapon.active = true;
	state.weapon.clip = 12;
	state.weapon.reserve = 90;
	state.weapon.capacity = 30;
	state.paused = true;
	state.paused_text = "PAUSED";
	state.frame_rate.shown = true;
	state.frame_rate.average_fps = 58;
	state.scoreboard.shown = true;
	state.scoreboard.title = "Players";
	const auto at_or_before = [](const HudDrawCursor &c, const HudDrawList::TopBegin &mark) {
		return c.quads <= mark.quads && c.tris <= mark.tris && c.lines <= mark.lines &&
				c.glyphs <= mark.glyphs && c.underlines <= mark.underlines;
	};
	const auto at_or_after = [](const HudDrawCursor &c, const HudDrawList::TopBegin &mark) {
		return c.quads >= mark.quads && c.tris >= mark.tris && c.lines >= mark.lines &&
				c.glyphs >= mark.glyphs && c.underlines >= mark.underlines;
	};
	// The spans' order: each found element's index in the walk.
	const auto order = [](const HudDrawList &list, HudElement element) {
		for (size_t i = 0; i < list.element_spans.size(); ++i)
			if (list.element_spans[i].element == element) return static_cast<int>(i);
		return -1;
	};
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.veil_begin.glyphs <= list.top_begin.glyphs);
		bool ammo = false, paused = false, frame_rate = false, board = false;
		for (const HudElementSpan &span : list.element_spans) {
			if (span.element == HudElement::AmmoCount) {
				ammo = true;
				CHECK(at_or_before(span.end, list.veil_begin));
			} else if (span.element == HudElement::PausedText) {
				paused = true;
				CHECK(at_or_after(span.begin, list.top_begin));
			} else if (span.element == HudElement::FrameRateOverlay) {
				frame_rate = true;
				CHECK(at_or_after(span.begin, list.top_begin));
			} else if (span.element == HudElement::Scoreboard) {
				board = true;
				CHECK(at_or_after(span.begin, list.top_begin));
			}
		}
		CHECK(ammo && paused && frame_rate && board);
		const int fps_at = order(list, HudElement::FrameRateOverlay);
		const int board_at = order(list, HudElement::Scoreboard);
		const int paused_at = order(list, HudElement::PausedText);
		CHECK(fps_at < board_at && board_at < paused_at);
		// The overlay panels from panels_begin on, over every menu (D-HUD-74); the
		// gameplay overlays' legs ahead of it.
		for (const HudElementSpan &span : list.element_spans) {
			if (span.element == HudElement::PausedText)
				CHECK(at_or_after(span.begin, list.panels_begin));
			if (span.element == HudElement::Scoreboard ||
					span.element == HudElement::FrameRateOverlay)
				CHECK(at_or_before(span.end, list.panels_begin));
		}
		// Each window and panel draws whole before the next: an order break closes the Tab
		// list's run and the pause text's, so the device leg's kind runs keep the walk's order.
		const auto closed = [&list](const HudDrawCursor &end) {
			for (const HudDrawList::TopBegin &mark : list.order_breaks)
				if (mark.quads == end.quads && mark.tris == end.tris && mark.lines == end.lines &&
						mark.glyphs == end.glyphs && mark.underlines == end.underlines)
					return true;
			return false;
		};
		for (const HudElementSpan &span : list.element_spans)
			if (span.element == HudElement::Scoreboard || span.element == HudElement::PausedText)
				CHECK(closed(span.end));
	}
	// The Tab list's call site: a dead local body, or the round over, draws none.
	state.local_dead = true;
	CHECK(order(compiler.compile(state, 1024.0f, 768.0f), HudElement::Scoreboard) < 0);
	state.local_dead = false;
	state.round_over = true;
	CHECK(order(compiler.compile(state, 1024.0f, 768.0f), HudElement::Scoreboard) < 0);
	state.round_over = false;
	state.hud_detail_level = 3;
	state.frame_rate.shown = false;
	{
		const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
		CHECK(list.veil_begin.quads == 0 && list.veil_begin.glyphs == 0);
		CHECK(list.glyphs.size() > list.veil_begin.glyphs); // the pause text, over the veil
		// The blank level keeps both passes: the Tab list, then the pause text over it.
		const int board_at = order(list, HudElement::Scoreboard);
		CHECK(board_at >= 0 && board_at < order(list, HudElement::PausedText));
	}
	// A list the scene frame did not compile marks no veil.
	const HudDrawList &console = compiler.compile_console_messages(state, 1024.0f, 768.0f);
	CHECK(console.veil_begin.glyphs == SIZE_MAX && console.panels_begin.glyphs == SIZE_MAX);
}

} // namespace

int main() {
	fnt_font_t font = test_font::uniform_test_font();
	test_wrapped_text_layout(&font);
	test_briefing_pages();
	test_windows_ride_the_top_layer(&font);
	test_briefing_markup_draws_plain(&font);
	test_end_game_screen(&font);
	test_view_veil_layer(&font);
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_overlay_windows_test OK\n");
	return 0;
}
