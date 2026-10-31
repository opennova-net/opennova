// The HUD's per-title variants (DFX2-91..): what dfx2.exe's HUD draws where Jointops.exe's draws
// otherwise, each read off the session's Title (hud::HudFrameState::title).
// [orig: sub_5BEEA0 @0x5BEEA0; sub_5B8050 @0x5B8050 (dfx2.exe)]
#include <base/title/title.h>
#include <runtime/hud/hud_frame.h>

#include "common/test_font.h"

#include <cstddef>
#include <cstdio>
#include <vector>

using namespace opennova::hud;
using opennova::fnt::fnt_font_t;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

struct Span {
	bool found = false;
	HudElementSpan span;
};

Span element_span(const HudDrawList &list, HudElement element) {
	Span out;
	for (const HudElementSpan &span : list.element_spans)
		if (span.element == element) {
			out.found = true;
			out.span = span;
		}
	return out;
}

std::vector<GameFontQuad> element_glyphs(const HudDrawList &list, HudElement element) {
	const Span s = element_span(list, element);
	if (!s.found) return {};
	return std::vector<GameFontQuad>(
			list.glyphs.begin() + static_cast<std::ptrdiff_t>(s.span.begin.glyphs),
			list.glyphs.begin() + static_cast<std::ptrdiff_t>(s.span.end.glyphs));
}

const opennova::Title &title(const char *code) {
	const opennova::Title *t = opennova::title_by_code(code);
	if (t == nullptr) {
		std::printf("FAIL no title %s\n", code);
		++failures;
		static const opennova::Title none;
		return none;
	}
	return *t;
}

// DFX2-91: dfx2.exe's HUD draws the lone-gunner panel; Jointops.exe's and OP: Acid Gambit's
// (Joint Operations' HUD) do not.
void test_the_lone_gunner_panel_is_dfx2s() {
	CHECK(title("dfx2").hud_lone_gunner_panel);
	CHECK(!title("jo").hud_lone_gunner_panel);
	CHECK(!title("acidgambit").hud_lone_gunner_panel);
}

// DFX2-91: while the local player is a lone gunner, the titleless stdbox (320, 500)..(704, 600)
// and the two-line string centred on design (512, 528) in the large slot, palette entry 4
// (0xFFF0F000) half-bright; nothing under JO's title, a null title, or with no lone gunner.
// [orig: sub_5B8050 @0x5B8050 (dfx2.exe) - HUD_DrawLabelBox @0x5B8074,
//  HUD_DrawTextCenteredScaled @0x5B8090]
void test_the_lone_gunner_panel(const fnt_font_t *font) {
	HudLayout layout;
	layout.box_texture_valid = true;
	layout.box_tex_w = 128;
	layout.box_tex_h = 128;
	HudFrameCompiler compiler;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	HudFrameState state;
	state.title = &title("dfx2");
	state.lone_gunner = true;
	const HudDrawList list = compiler.compile(state, 1024, 768);
	const Span span = element_span(list, HudElement::LoneGunnerPanel);
	CHECK(span.found);
	if (span.found) CHECK(span.span.end.quads > span.span.begin.quads); // the stdbox
	const std::vector<GameFontQuad> g = element_glyphs(list, HudElement::LoneGunnerPanel);
	// "The tank requires another player" (32) and "in order to use the cannon" (26), spaces
	// included, over the uniform font (glyphs 8 wide, advance 9).
	CHECK(g.size() == 58);
	if (g.size() == 58) {
		CHECK(g[0].color == 0xFF787800u);
		// Each line centred on x 512 (the quads half a pixel up and left of their cursor): the
		// first measures 31 advances and a glyph (287), so it starts 143.5 left.
		CHECK(g[0].x_top_left == 512.0f - 143.5f - 0.5f);
		CHECK(g[0].y_top == 528.0f - 0.5f);
		// The second line one row lower, its 26 glyphs (233) centred: 116.5 left.
		CHECK(g[32].x_top_left == 512.0f - 116.5f - 0.5f);
		CHECK(g[32].y_top > g[0].y_top);
	}
	state.lone_gunner = false;
	CHECK(!element_span(compiler.compile(state, 1024, 768), HudElement::LoneGunnerPanel).found);
	state.lone_gunner = true;
	state.title = &title("jo");
	CHECK(!element_span(compiler.compile(state, 1024, 768), HudElement::LoneGunnerPanel).found);
	state.title = nullptr;
	CHECK(!element_span(compiler.compile(state, 1024, 768), HudElement::LoneGunnerPanel).found);
}

// DFX2-92: with the waypoint active, dfx2.exe's step sets the colour to g_HUDColors.active halved
// alpha and all and the band to 2 whatever the height; Jointops.exe's (and a null title's) picks
// by the height against the local player's, +-2.0 apart: below blue band 1, above orange band 0,
// level green band 2. Both keep the words while no waypoint is active.
// [orig: HUD_UpdateWaypointAltitudeColor @0x590970] [orig: sub_58EDF0 @0x58EDF0 (dfx2.exe)]
void test_the_waypoint_tint(const fnt_font_t *font) {
	CHECK(title("dfx2").hud_waypoint_active_tint);
	CHECK(!title("jo").hud_waypoint_active_tint);
	CHECK(!title("acidgambit").hud_waypoint_active_tint);
	HudLayout layout;
	HudFrameState state;
	state.hud_color_index = 0; // white: the twin's seed 0xFFFFFFFF
	state.waypoint.present = true;
	state.waypoint.world_z = 10 * 65536;
	state.minimap.player_z = 0;
	const auto words = [&](const opennova::Title *t) {
		HudFrameCompiler compiler;
		compiler.configure(layout, font);
		state.title = t;
		compiler.compile(state, 1024, 768);
		return compiler.process_words();
	};
	HudProcessWords w = words(&title("dfx2"));
	CHECK(w.waypoint_altitude_color == 0x7F7F7F7Fu && w.waypoint_altitude_band == 2);
	w = words(&title("jo"));
	CHECK(w.waypoint_altitude_color == 0xFF7F5000u && w.waypoint_altitude_band == 0);
	w = words(nullptr);
	CHECK(w.waypoint_altitude_color == 0xFF7F5000u && w.waypoint_altitude_band == 0);
	state.waypoint.world_z = -10 * 65536;
	w = words(&title("jo"));
	CHECK(w.waypoint_altitude_color == 0xFF20407Fu && w.waypoint_altitude_band == 1);
	w = words(&title("dfx2"));
	CHECK(w.waypoint_altitude_color == 0x7F7F7F7Fu && w.waypoint_altitude_band == 2);
	// No active waypoint: the words stand (a fresh HUD's zero).
	state.waypoint.present = false;
	w = words(&title("dfx2"));
	CHECK(w.waypoint_altitude_color == 0u && w.waypoint_altitude_band == 0);
}

} // namespace

int main() {
	test_the_lone_gunner_panel_is_dfx2s();
	fnt_font_t font = test_font::uniform_test_font();
	test_the_lone_gunner_panel(&font);
	test_the_waypoint_tint(&font);
	opennova::fnt::fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_title_draws_test OK\n");
	return 0;
}
