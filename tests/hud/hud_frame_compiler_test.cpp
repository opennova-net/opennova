// HudFrameCompiler + GameFont pins: the witnessed element walk emits the
// typed draw list (quads/tris/glyphs) with the ported policy math intact.
// [orig: HUD_RenderOverlays @ 0x5a7bb0; CGameFont_MeasureText @ 0x674e70;
//  CGameFont_DrawText @ 0x6752c0]

#include <hud/game_font.h>
#include <hud/hud_frame.h>
#include <hud/hud_math.h>

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

// Friendly tags (D-HUD-20) [orig: HUD_DrawEntityLabel @ 0x5a39b0]: the
// witnessed gates, tier colors, distance alpha, fallback name, medic plate,
// and the BRIEF tick form, over the default (retail) tag colors.
void test_compiler_friendly_tags(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24; // the fallback table's "SGT  Brown"
	state.friendly_tags.push_back(tag);

	// FULL (the boot default): centered half-bright text of the fallback name.
	const HudDrawList &full = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(full.glyphs.size() == 11,
			"the fallback '^SGT  Brown' lays out 11 glyphs");
	// 100 m -> alpha 255 - 192*50/250 = 217; good tier = tagcolor_good with
	// the alpha-preserving half-bright fold.
	const uint32_t good = (217u << 24) | (layout.tag_good & 0xFFFFFFu);
	CHECK(!full.glyphs.empty() &&
					full.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(good),
			"good tier rides tagcolor_good + the distance alpha");
	// Centered on x=200: 11 glyphs at advance 9 minus the trailing pad = 98
	// wide -> cursor 151 -> the -0.5 vertex offset.
	CHECK(!full.glyphs.empty() && full.glyphs[0].x_top_left == 150.5f,
			"the text centers on the projected x");
	CHECK(full.quads.empty(), "no medic plate without the medic flag");

	// The health tiers swap the color.
	state.friendly_tags[0].health_ratio_fp16 = 0x8000;
	const HudDrawList &mid = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t middle = (217u << 24) | (layout.tag_middle & 0xFFFFFFu);
	CHECK(!mid.glyphs.empty() &&
					mid.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(middle),
			"the middle tier rides tagcolor_middle");
	state.friendly_tags[0].health_ratio_fp16 = 0x10000;

	// The medic plate: white square + the two red cross bars, left of the text.
	state.friendly_tags[0].medic = true;
	const HudDrawList &medic = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(medic.quads.size() == 3, "the medic plate is three untextured quads");
	if (medic.quads.size() == 3) {
		CHECK(medic.quads[0].color == ((217u << 24) | 0xFFFFFFu),
				"the plate is white at the tag alpha");
		CHECK(medic.quads[1].color == ((217u << 24) | 0xFF0000u),
				"the cross bars are red at the tag alpha");
		// left = 200 - (98/2 + 16) - 0.5, a fontH/2 = 8 px square at the text top.
		CHECK(medic.quads[0].x0 == 134.5f && medic.quads[0].y0 == 91.5f,
				"the plate sits one fontH left of the text");
		CHECK(medic.quads[0].x1 - medic.quads[0].x0 == 8.0f,
				"the plate is a fontH/2 square");
	}
	state.friendly_tags[0].medic = false;

	// Gates: too close, fogged, FARBRIEF far, OFF.
	state.friendly_tags[0].dist_q16 = 0x4000;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"under 0.5 u nothing draws");
	state.friendly_tags[0].dist_q16 = 100 << 16;
	state.fog_dist_q16 = 50 << 16;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"the fog cull hides the tag");
	state.fog_dist_q16 = INT32_MAX;
	state.friendly_tag_mode = 1;
	state.friendly_tags[0].dist_q16 = 400 << 16;
	state.fog_dist_q16 = 1000 << 16;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"FARBRIEF draws nothing past 300 m");
	state.friendly_tags[0].dist_q16 = 100 << 16;
	CHECK(!compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"FARBRIEF draws text under 300 m");
	state.friendly_tag_mode = 0;
	CHECK(compiler.compile(state, 1024.0f, 768.0f).glyphs.empty(),
			"OFF draws nothing");

	// BRIEF: the three 1-px tick lines replace the text.
	state.friendly_tag_mode = 3;
	const HudDrawList &brief = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(brief.glyphs.empty(), "BRIEF draws no text");
	CHECK(brief.lines.size() == 3, "BRIEF draws the three tick lines");
	if (brief.lines.size() == 3) {
		CHECK(brief.lines[1].x0 == 200.0f && brief.lines[1].y0 == 96.0f &&
						brief.lines[1].y1 == 104.0f,
				"the tick spans +-fontH/4 around the projected point");
	}

	// A slot entry with an empty callsign draws the single bar.
	state.friendly_tag_mode = 2;
	state.friendly_tags[0].player = true;
	state.friendly_tags[0].name.clear();
	const HudDrawList &bar = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(bar.glyphs.empty() && bar.lines.size() == 1,
			"an empty player callsign draws the bar form");

	// The speaking pulse lifts the color.
	state.friendly_tags[0].player = false;
	state.friendly_tags[0].speaking = true;
	state.speaking_level255 = 255;
	const HudDrawList &speak = compiler.compile(state, 1024.0f, 768.0f);
	const uint32_t pulsed = (217u << 24) |
			(opennova::hud::friendly_tag_speaking_blend(
					 layout.tag_good, 255) &
					0xFFFFFFu);
	CHECK(!speak.glyphs.empty() &&
					speak.glyphs[0].color ==
							opennova::hud::half_bright_keep_alpha(pulsed),
			"the speaking tag pulses toward white");
}

// The overlay label fonts: friendly tags draw with the NORMAL label font,
// attach labels with the BOLD one, both at the slot scale, each in its own
// draw-list page namespace. [orig: HUD_InitAllFonts @ 0x51ee20; tag font
// g_hudLabelFont @ 0x5a3a0c; attach font (the bold slot) @ 0x5a3680; the slot
// scales enter the draw/measure/char-height helpers @ 0x580680/@ 0x580ab0/
// @ 0x580a80]
void test_compiler_label_fonts(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, 2.0f);

	HudFrameState state;
	opennova::hud::HudFriendlyTag tag;
	tag.screen_x = 200.0f;
	tag.screen_y = 100.0f;
	tag.dist_q16 = 100 << 16;
	tag.entity_id = 24; // "^SGT  Brown", 11 glyphs
	state.friendly_tags.push_back(tag);
	opennova::hud::HudAttachLabel label;
	label.screen_x = 300.0f;
	label.screen_y = 60.0f;
	label.text = "Sit";
	state.attach_labels.push_back(label);

	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	size_t normal_glyphs = 0;
	size_t bold_glyphs = 0;
	const opennova::hud::GameFontQuad *first_tag_glyph = nullptr;
	for (const opennova::hud::GameFontQuad &g : list.glyphs) {
		if (g.page ==
				static_cast<uint32_t>(opennova::hud::kHudFontSlotLabel) *
						FNT_MAX_PAGES) {
			if (first_tag_glyph == nullptr) {
				first_tag_glyph = &g;
			}
			++normal_glyphs;
		}
		if (g.page ==
				static_cast<uint32_t>(opennova::hud::kHudFontSlotLabelBold) *
						FNT_MAX_PAGES) {
			++bold_glyphs;
		}
	}
	CHECK(normal_glyphs == 11,
			"tag glyphs ride the normal label font's page namespace");
	CHECK(bold_glyphs == 3,
			"attach glyphs ride the bold label font's page namespace");
	// Scale 2: the 98-wide line centers as 200 - 98 -> 101.5 after the -0.5
	// offset; fontH = 16*2, text top = y - fontH/2 -> 83.5.
	CHECK(first_tag_glyph != nullptr &&
					first_tag_glyph->x_top_left == 101.5f &&
					first_tag_glyph->y_top == 83.5f,
			"the slot scale doubles the centered layout metrics");

	// Null label fonts fall back to the hudpos font at scale 1 (page 0).
	compiler.configure_label_fonts(nullptr, nullptr, 1.0f);
	const HudDrawList &fallback = compiler.compile(state, 1024.0f, 768.0f);
	bool all_page0 = !fallback.glyphs.empty();
	for (const opennova::hud::GameFontQuad &g : fallback.glyphs) {
		all_page0 = all_page0 && g.page == 0;
	}
	CHECK(all_page0, "absent label fonts fall back to the hudpos font");
}

} // namespace

int main() {
	fnt_font_t font = make_font();
	test_measure_advance_and_trailing_pad(&font);
	test_layout_pages_bold_underline(&font);
	test_format_tags(&font);
	test_compiler_health_and_order(&font);
	test_compiler_crosshair(&font);
	test_compiler_friendly_tags(&font);
	test_compiler_label_fonts(&font);
	fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_frame_compiler_test OK\n");
	return 0;
}
