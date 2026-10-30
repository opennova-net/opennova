// DFX2-36: dfx2.exe's EXPPOINTS element (runtime/hud/hud_exp_points.h): the fade a 0x81 fanfare
// starts and each draw steps, its tiers, its strings, the title that draws it, and the compiled
// element (the label, the score pulsed and tinted, the tracker's two lines, its gates).
// [orig: sub_59B730 @0x59B730; sub_59B7E0 @0x59B7E0; sub_5A6D20 @0x5A6D20 (@0x5A6EDE);
//  @0x422350..0x4223C5; HUD_InitOverlaySystem @0x5A3490 (dfx2.exe)]
#include <base/title/title.h>
#include <runtime/hud/hud_exp_points.h>
#include <runtime/hud/hud_frame.h>

#include "common/test_font.h"

#include <cmath>
#include <cstddef>
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

// The title draws it: dfx2.exe's HUD, not Jointops.exe's (whose EXPPOINTS words have no reader).
void test_the_title_draws_it() {
	const opennova::Title *dfx2 = opennova::title_by_code("dfx2");
	const opennova::Title *jo = opennova::title_by_code("jo");
	CHECK(dfx2 != nullptr && dfx2->hud_exp_points);
	CHECK(jo != nullptr && !jo->hud_exp_points);
}

// The start: hidden or a 0 duration starts nothing; else intensity 1, the per-channel distance in
// 16 bits, the rate 1 / duration and the pulse percent * 0.01 - 1 [orig: sub_59B730 @0x59B730].
void test_the_fade_start() {
	HudExpPointsFade fade;
	hud_exp_points_fade_start(fade, /*hidden=*/true, 62, 0xFFF0F000u, 0xFFFFFFFFu, 200);
	CHECK(fade.intensity == 0.0f && fade.rate == 0.0f);
	hud_exp_points_fade_start(fade, false, 0, 0xFFF0F000u, 0xFFFFFFFFu, 200);
	CHECK(fade.intensity == 0.0f);
	hud_exp_points_fade_start(fade, false, 62, 0xFFF0F000u, 0xFFFFFFFFu, 200);
	CHECK(fade.intensity == 1.0f && fade.rate == 1.0f / 62.0f && fade.pulse == 1.0f);
	CHECK(fade.delta[0] == -15 && fade.delta[1] == -15 && fade.delta[2] == -255);
	hud_exp_points_fade_start(fade, false, 31, 0xFF000000u, 0xFF102030u, 100);
	CHECK(fade.pulse == 0.0f && fade.delta[0] == -0x10 && fade.delta[1] == -0x20 &&
			fade.delta[2] == -0x30);
}

// A step: the intensity less the rate; above 0 the scale is pulse * it + 1 and each channel byte
// takes its distance times it, chopped, in 8 bits; at 0 or below the fade clears and the number
// draws at scale 1 in the active colour [orig: sub_59B7E0 @0x59B805..0x59B91B].
void test_the_fade_step() {
	HudExpPointsFade fade;
	HudExpPointsPulse idle = hud_exp_points_fade_step(fade, 0xFFFFFFFFu);
	CHECK(idle.scale == 1.0f && idle.color == 0xFFFFFFFFu);
	hud_exp_points_fade_start(fade, false, 62, 0xFFF0F000u, 0xFFFFFFFFu, 200);
	const HudExpPointsPulse first = hud_exp_points_fade_step(fade, 0xFFFFFFFFu);
	const float v = 1.0f - 1.0f / 62.0f;
	CHECK(fade.intensity == v);
	CHECK(first.scale == 1.0f * v + 1.0f);
	// -15 * v = -14.76 -> -14 -> 0xFF + 0xF2 = 0xF1; -255 * v = -250.9 -> -250 -> 0xFF + 6 = 0x05.
	CHECK(first.color == 0xFFF1F105u);
	HudExpPointsPulse last = first;
	for (int i = 0; i < 70; ++i) last = hud_exp_points_fade_step(fade, 0xFFFFFFFFu);
	CHECK(last.scale == 1.0f && last.color == 0xFFFFFFFFu);
	CHECK(fade.intensity == 0.0f && fade.rate == 0.0f && fade.pulse == 0.0f && fade.delta[2] == 0);
}

// The tiers: HIT 31 toward the active colour, KILL 62 toward palette 4, HEADSHOT 93 toward 5
// [orig: @0x422380..0x4223BE (dfx2.exe)]; and the strings' fallbacks
// [orig: HUD_InitOverlaySystem @0x5A3693..0x5A36F0 (dfx2.exe)].
void test_the_tiers_and_strings() {
	CHECK(hud_exp_points_tier(ScoreTone::Hit).duration == 31 &&
			hud_exp_points_tier(ScoreTone::Hit).palette == -1);
	CHECK(hud_exp_points_tier(ScoreTone::Kill).duration == 62 &&
			hud_exp_points_tier(ScoreTone::Kill).palette == 4);
	CHECK(hud_exp_points_tier(ScoreTone::Headshot).duration == 93 &&
			hud_exp_points_tier(ScoreTone::Headshot).palette == 5);
	CHECK(hud_exp_points_tier(ScoreTone::None).duration == 0);
	HudExpPointsText text;
	hud_exp_points_text(GameTextLookup(), text);
	CHECK(text.label == "!eXperience Points: " && text.session_format == "!Session XP: %d" &&
			text.total_format == "!Total XP: %d");
}

std::vector<GameFontQuad> element_glyphs(const HudDrawList &list) {
	for (const HudElementSpan &span : list.element_spans)
		if (span.element == HudElement::ExpPoints)
			return std::vector<GameFontQuad>(
					list.glyphs.begin() + static_cast<std::ptrdiff_t>(span.begin.glyphs),
					list.glyphs.begin() + static_cast<std::ptrdiff_t>(span.end.glyphs));
	return {};
}

float middle(const GameFontQuad &q) {
	return 0.5f * (q.y_top + q.y_bottom);
}

// The compiled element over the uniform font (glyphs 8 x 16, advance 9): the label at the anchor,
// the score after the label's width (17), both half-bright white; a KILL arm pulses the score by
// the hudpos 200 percent about the label's middle row and tints it toward palette 4, easing back;
// the tracker's lines one '0' height (16) apart while it is valid and neither value negative; the
// declutter slot, the hidden word and the title each keep it off, and an arm while the title draws
// none starts no fade.
// [orig: sub_59B7E0 @0x59B7E0; sub_5A6D20 @0x5A6EDE; @0x422350..0x4223C5 (dfx2.exe)]
void test_the_compiled_element(const fnt_font_t *font) {
	HudLayout layout;
	layout.exp_points = {100, 200, 0, 200, true};
	HudFrameCompiler compiler;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	HudFrameState state;
	state.hud_color_index = 0; // white: the twin's seed 0xFFFFFFFF
	state.exp_points.element = true;
	state.exp_points.score = 42;
	state.exp_points.text.label = "XP";
	state.exp_points.text.session_format = "S%d";
	state.exp_points.text.total_format = "T%d";
	std::vector<GameFontQuad> g = element_glyphs(compiler.compile(state, 1024, 768));
	CHECK(g.size() == 4);
	if (g.size() == 4) {
		// The layout's quads sit half a pixel up and left of their cursor.
		CHECK(g[0].x_top_left == 99.5f && g[0].color == 0xFF7F7F7Fu);
		CHECK(g[2].x_top_left == g[0].x_top_left + 17.0f && g[2].color == 0xFF7F7F7Fu);
		CHECK(g[2].y_top == g[0].y_top && g[2].y_bottom == g[0].y_bottom);
	}
	// A KILL arm: the score twice-ish its height about the label's middle, its colour the first
	// step's tint (0xFFF1F105) halved.
	state.exp_points.fade_arms = 1;
	state.exp_points.fade_tone = ScoreTone::Kill;
	g = element_glyphs(compiler.compile(state, 1024, 768));
	CHECK(g.size() == 4);
	if (g.size() == 4) {
		CHECK(g[0].color == 0xFF7F7F7Fu);
		CHECK(g[2].color == 0xFF787882u);
		CHECK(g[2].y_bottom - g[2].y_top > 1.9f * (g[0].y_bottom - g[0].y_top));
		CHECK(std::fabs(middle(g[2]) - middle(g[0])) < 0.51f);
	}
	for (int i = 0; i < 70; ++i) compiler.compile(state, 1024, 768);
	g = element_glyphs(compiler.compile(state, 1024, 768));
	CHECK(g.size() == 4 && g[2].color == 0xFF7F7F7Fu && g[2].y_top == g[0].y_top);
	// The tracker's lines.
	state.exp_points.tracker_valid = true;
	state.exp_points.session_points = 5;
	state.exp_points.total_points = 9;
	g = element_glyphs(compiler.compile(state, 1024, 768));
	CHECK(g.size() == 8);
	if (g.size() == 8) {
		CHECK(g[4].x_top_left == g[0].x_top_left && g[4].y_top == g[0].y_top + 16.0f);
		CHECK(g[6].x_top_left == g[0].x_top_left && g[6].y_top == g[0].y_top + 32.0f);
	}
	state.exp_points.session_points = -1;
	CHECK(element_glyphs(compiler.compile(state, 1024, 768)).size() == 4);
	state.exp_points.tracker_valid = false;
	state.exp_points.session_points = 5;
	CHECK(element_glyphs(compiler.compile(state, 1024, 768)).size() == 4);
	// The declutter slot, then the title.
	state.declutter_visible[kDeclutterExpPoints] = false;
	CHECK(element_glyphs(compiler.compile(state, 1024, 768)).empty());
	state.declutter_visible[kDeclutterExpPoints] = true;
	state.exp_points.element = false;
	state.exp_points.fade_arms = 2;
	CHECK(element_glyphs(compiler.compile(state, 1024, 768)).empty());
	state.exp_points.element = true;
	g = element_glyphs(compiler.compile(state, 1024, 768));
	CHECK(g.size() == 4 && g[2].color == 0xFF7F7F7Fu && g[2].y_top == g[0].y_top);
	// The hudpos line's hidden word.
	layout.exp_points.hidden = 1;
	HudFrameCompiler hidden;
	hidden.configure(layout, font);
	hidden.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	CHECK(element_glyphs(hidden.compile(state, 1024, 768)).empty());
}

} // namespace

int main() {
	test_the_title_draws_it();
	test_the_fade_start();
	test_the_fade_step();
	test_the_tiers_and_strings();
	fnt_font_t font = test_font::uniform_test_font();
	test_the_compiled_element(&font);
	opennova::fnt::fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_exp_points_test OK\n");
	return 0;
}
