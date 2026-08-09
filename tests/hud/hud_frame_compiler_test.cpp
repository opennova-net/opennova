// HudFrameCompiler + GameFont pins: the witnessed element walk emits the
// typed draw list (quads/tris/glyphs) with the ported policy math intact.
// [orig: HUD_RenderOverlays @ 0x5a7bb0; CGameFont_MeasureText @ 0x674e70;
//  CGameFont_DrawText @ 0x6752c0]

#include <hud/game_font.h>
#include <hud/hud_frame.h>

#include <cstdio>
#include <cstring>

using opennova::hud::GameFont;
using opennova::hud::GameFontState;
using opennova::hud::HudDrawList;
using opennova::hud::HudFrameCompiler;
using opennova::hud::HudFrameState;
using opennova::hud::HudLayout;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s\n", msg);                           \
			++failures;                                                        \
		}                                                                      \
	} while (0)

// A synthetic 1-page font: every glyph 8x16 px on the 256-page grid,
// spacing 2, design width 800 (scale 1).
fnt_font_t make_font() {
	fnt_font_t font{};
	fnt_init_blank(&font, 1, 2);
	font.design_width = 800;
	for (uint32_t i = 0; i < FNT_GLYPH_COUNT; ++i) {
		font.glyphs[i].page = 0;
		font.glyphs[i].uv.u0 = 0.0f;
		font.glyphs[i].uv.v0 = 0.0f;
		font.glyphs[i].uv.u1 = 8.0f / 256.0f;
		font.glyphs[i].uv.v1 = 16.0f / 256.0f;
	}
	return font;
}

void test_measure_advance_and_trailing_pad(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	int w = 0;
	int h = 0;
	// Each glyph advances floor(8 + (2-1) + 0.5) = 9; the final width strips
	// the trailing (spacing-1) pad: 3 glyphs -> 27 - 1 = 26.
	gf.measure("abc", 1.0f, 1.0f, &w, &h);
	CHECK(w == 26, "measure width = glyph advances minus the trailing pad");
	CHECK(h == 16, "measure height = the SPACE glyph's v-extent");

	// Controls and the three retail control bytes are skipped.
	int w2 = 0;
	gf.measure("a\x7f\x01"
			   "bc",
			1.0f, 1.0f, &w2, &h);
	CHECK(w2 == w, "0x7F and control bytes measure nothing");

	// Newline folds the max and adds a line.
	int w3 = 0;
	int h3 = 0;
	gf.measure("abc\nabcd", 1.0f, 1.0f, &w3, &h3);
	CHECK(w3 == 35, "multiline width is the widest line");
	CHECK(h3 == 32, "each newline adds one line height");
}

void test_layout_pages_bold_underline(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	const auto run = gf.layout("ab", 100.0f, 50.0f, 1.0f, 1.0f, 0u,
			0xFF102030u);
	CHECK(run.quads.size() == 2, "one quad per glyph on the one page");
	CHECK(run.quads[0].x_top_left == 99.5f,
			"the drawer's -0.5 vertex offset is kept");
	CHECK(run.quads[1].x_top_left == 108.5f,
			"the second glyph starts at the floored advance");
	CHECK(run.underlines.empty(), "no underline without the flag");

	const auto bold = gf.layout("a", 0.0f, 0.0f, 1.0f, 1.0f,
			opennova::hud::kFontStyleBold, 0xFF000000u);
	CHECK(bold.quads.size() == 2, "bold double-strikes each glyph");
	CHECK(bold.quads[1].x_top_left == bold.quads[0].x_top_left + 1.0f,
			"the second strike lands one pixel right");

	const auto lined = gf.layout("a", 0.0f, 0.0f, 1.0f, 1.0f,
			opennova::hud::kFontStyleUnderline, 0xFF404040u);
	CHECK(lined.underlines.size() == 1, "underline emits one segment");
	CHECK((lined.underlines[0].color & 0xFFFFFFu) == 0x808080u,
			"the underline color doubles the text color");
}

void test_format_tags(const fnt_font_t *font) {
	GameFont gf;
	gf.set_font(font);
	GameFontState state;
	int idx = 0;
	CHECK(GameFont::parse_format_tag("<B>", &idx, &state), "well-formed <B>");
	CHECK(state.bold, "<B> sets bold");
	idx = 0;
	CHECK(GameFont::parse_format_tag("<-B>", &idx, &state), "<-B> parses");
	CHECK(!state.bold, "<-B> clears bold");
	idx = 0;
	CHECK(!GameFont::parse_format_tag("<B", &idx, &state),
			"an unterminated tag reports failure");
	idx = 0;
	GameFontState color_state;
	CHECK(GameFont::parse_format_tag("<C102030>", &idx, &color_state),
			"hex color tag parses");
	CHECK((color_state.color_xor & 0xFFFFFFu) == 0x102030u,
			"the color XOR folds the tag color");
	// Measurement skips tags entirely.
	int w = 0;
	int h = 0;
	gf.measure("<B>ab", 1.0f, 1.0f, &w, &h);
	int w2 = 0;
	gf.measure("ab", 1.0f, 1.0f, &w2, &h);
	CHECK(w == w2, "tags measure zero width");
}

void test_compiler_health_and_order(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.health_rect = {10.0f, 20.0f, 100.0f, 8.0f, true};
	layout.heat_rect = {200.0f, 20.0f, 40.0f, 8.0f, true};
	compiler.configure(layout, font);

	HudFrameState state;
	state.ticks = 100;
	state.health_fraction = 0.5f;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Health at 0.5: the mid band fill + the wireframe border.
	CHECK(list.quads.size() == 2, "health emits fill + border");
	CHECK(list.quads[0].filled && !list.quads[1].filled,
			"fill first, border on top");
	CHECK(list.quads[0].color == layout.tag_middle,
			"0.5 health takes the middle band color");

	// Heat with an armed weapon.
	HudFrameState hot = state;
	hot.weapon.active = true;
	hot.weapon.heat = 0x8000;
	const HudDrawList &heat = compiler.compile(hot, 1024.0f, 768.0f);
	CHECK(heat.quads.size() == 4, "heat adds its border + fill");

	// The message ring: pushed lines draw until expiry.
	compiler.push_message("hello", 100);
	const HudDrawList &with_msg = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(!with_msg.glyphs.empty(), "a live message emits glyphs");
	HudFrameState later = state;
	later.ticks = 100 + 930 + 1;
	const HudDrawList &expired = compiler.compile(later, 1024.0f, 768.0f);
	CHECK(expired.glyphs.empty(), "the 930-tick life expires the line");
}

void test_compiler_crosshair(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.crosshair_texture_valid = true;
	layout.crosshair_tex_w = 64;
	layout.crosshair_tex_h = 64;
	compiler.configure(layout, font);

	HudFrameState state;
	state.weapon.active = true;
	state.aimed_shot_available = false;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	// Five regions: four 5-vertex strips (3 tris) + one 4-vertex center
	// (2 tris) = 14 triangles.
	CHECK(list.tris.size() == 14, "the five tapered regions emit 14 triangles");

	HudFrameState aimed = state;
	aimed.aimed_shot_available = true;
	const HudDrawList &hidden = compiler.compile(aimed, 1024.0f, 768.0f);
	CHECK(hidden.tris.empty(), "a settled aimed shot hides the reticle");
}

} // namespace

int main() {
	fnt_font_t font = make_font();
	test_measure_advance_and_trailing_pad(&font);
	test_layout_pages_bold_underline(&font);
	test_format_tags(&font);
	test_compiler_health_and_order(&font);
	test_compiler_crosshair(&font);
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_frame_compiler_test OK\n");
	return 0;
}
