// The HUD walk's record of its own draws (runtime/hud/hud_elements.h): which element emitted each
// run of a compiled draw list, and each element's box on the surface it was compiled at, which the
// OpenNova Editor's HUD preview names the element under a point by (ADR 0046 DI-20). The record is a
// tool's: it changes nothing the walk draws.
#include <runtime/hud/hud_elements.h>
#include <runtime/hud/hud_frame.h>

#include "common/test_font.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace opennova::hud;
using opennova::fnt::fnt_font_t;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

const HudElementBox *box_of(const std::vector<HudElementBox> &boxes, HudElement element) {
	for (const HudElementBox &box : boxes)
		if (box.element == element) return &box;
	return nullptr;
}

bool near(float a, float b) {
	return std::fabs(a - b) < 0.51f;
}

// A soldier panel: the static frame, the health bar, a stance icon and an armed weapon's ammo count,
// name and crosshair, compiled at the design size and at 640 x 480.
HudLayout soldier_layout() {
	HudLayout layout;
	layout.frame_pos = {6, 586, 0, 0, true};
	layout.frame_texture_valid = true;
	layout.frame_tex_w = 128;
	layout.frame_tex_h = 64;
	layout.health_rect = {25.0f, 741.0f, 152.0f, 10.0f, true};
	layout.stance_pos = {30, 639, 0, 0, true};
	layout.stance_texture_valid = {true, true, true, true, true, true}; // the drawer wants all six
	layout.stance_frame0_w = 32;
	layout.stance_frame0_h = 32;
	layout.alpha_fade_base_alpha = 102;
	layout.alpha_fade_max_alpha = 178;
	layout.alpha_fade_ramp_ticks = 186;
	layout.ammo_count = {168, 616, 0, 1, true};
	layout.weapon_name = {14, 588, 0, 0, true};
	layout.crosshair_texture_valid = true;
	layout.crosshair_tex_w = 64;
	layout.crosshair_tex_h = 64;
	return layout;
}

HudFrameState soldier_state() {
	HudFrameState state;
	state.ticks = 1000;
	state.health_fraction = 1.0f;
	state.stance = 0;
	state.weapon.active = true;
	state.weapon.clip = 30;
	state.weapon.reserve = 90;
	state.weapon.capacity = 30;
	state.weapon.display_name = "AR-15";
	return state;
}

void test_spans_follow_the_walk(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	compiler.configure(soldier_layout(), font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	const HudDrawList &list = compiler.compile(soldier_state(), 1024.0f, 768.0f);
	// One span an element that drew, in the walk's order: the frame, the health bar, the stance icon,
	// then the weapon cluster's ammo count, name and crosshair (no clip art, no targeting).
	std::vector<HudElement> order;
	for (const HudElementSpan &span : list.element_spans) order.push_back(span.element);
	const std::vector<HudElement> walk = {HudElement::Frame, HudElement::Health, HudElement::Stance,
	                                      HudElement::AmmoCount, HudElement::WeaponName, HudElement::Crosshair};
	CHECK(order == walk);
	// The spans cover the flat lists whole and do not overlap: every quad, triangle and glyph is one
	// element's.
	size_t quads = 0, tris = 0, glyphs = 0;
	for (const HudElementSpan &span : list.element_spans) {
		quads += span.end.quads - span.begin.quads;
		tris += span.end.tris - span.begin.tris;
		glyphs += span.end.glyphs - span.begin.glyphs;
	}
	CHECK(quads == list.quads.size() && tris == list.tris.size() && glyphs == list.glyphs.size());
	const std::vector<HudElementBox> boxes = hud_element_boxes(list);
	CHECK(boxes.size() == walk.size());
	// The frame: its texels at its hudpos place, the design size one to one.
	const HudElementBox *frame = box_of(boxes, HudElement::Frame);
	CHECK(frame && near(frame->x0, 6.0f) && near(frame->y0, 586.0f) && near(frame->x1, 134.0f) && near(frame->y1, 650.0f));
	// The health bar: its HUDHEALTH rect (the border the bar's extent).
	const HudElementBox *health = box_of(boxes, HudElement::Health);
	CHECK(health && near(health->x0, 25.0f) && near(health->y0, 741.0f) && near(health->x1, 177.0f) &&
	      near(health->y1, 751.0f));
	// The ammo count: right-aligned at AMMOCOUNTPOS, so it ends there.
	const HudElementBox *ammo = box_of(boxes, HudElement::AmmoCount);
	CHECK(ammo && ammo->x1 <= 168.5f && ammo->x0 < 168.0f && near(ammo->y0, 616.0f));
	// The weapon name: left-aligned at HUDWEAPONNAME.
	const HudElementBox *name = box_of(boxes, HudElement::WeaponName);
	CHECK(name && near(name->x0, 14.0f) && near(name->y0, 588.0f) && name->x1 > 14.0f);
	// The crosshair: about the screen's middle.
	const HudElementBox *crosshair = box_of(boxes, HudElement::Crosshair);
	CHECK(crosshair && crosshair->x0 < 512.0f && crosshair->x1 > 512.0f && crosshair->y0 < 384.0f &&
	      crosshair->y1 > 384.0f);

	// At 640 x 480 the same elements scale with the surface (the design space's own rule).
	const HudDrawList &small = compiler.compile(soldier_state(), 640.0f, 480.0f);
	const std::vector<HudElementBox> scaled = hud_element_boxes(small);
	const HudElementBox *small_health = box_of(scaled, HudElement::Health);
	// The scale is integer, rounded to the nearest pixel (hud_math's virtual-coords rule).
	CHECK(small_health && near(small_health->x0, float((25 * 640 + 512) / 1024)) &&
	      near(small_health->y1, float((751 * 480 + 384) / 768)));
}

// What a compile draws with the record and without it is the same: the record is the walk's own, and
// an element that draws nothing (no weapon: no ammo, no name, no crosshair) leaves no span.
void test_record_changes_nothing(const fnt_font_t *font) {
	HudFrameCompiler compiler;
	compiler.configure(soldier_layout(), font);
	HudFrameState state = soldier_state();
	state.weapon.active = false;
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	for (const HudElementSpan &span : list.element_spans)
		CHECK(span.element != HudElement::AmmoCount && span.element != HudElement::WeaponName &&
		      span.element != HudElement::Crosshair);
	// The blank detail level draws no soldier panel: no span of it.
	state.hud_detail_level = 3;
	const HudDrawList &blank = compiler.compile(state, 1024.0f, 768.0f);
	CHECK(box_of(hud_element_boxes(blank), HudElement::Frame) == nullptr);
}

void test_tokens() {
	for (size_t i = 0; i < kHudElementCount; ++i) {
		const char *token = hud_element_token(static_cast<HudElement>(i));
		HudElement back = HudElement::kCount;
		CHECK(token[0] != '\0' && hud_element_from_token(token, back) && back == static_cast<HudElement>(i));
	}
	HudElement none = HudElement::Frame;
	CHECK(!hud_element_from_token("no_such_element", none) && std::strcmp(hud_element_token(HudElement::kCount), "") == 0);
}

} // namespace

int main() {
	fnt_font_t font = test_font::uniform_test_font();
	test_spans_follow_the_walk(&font);
	test_record_changes_nothing(&font);
	test_tokens();
	opennova::fnt::fnt_free(&font);
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_element_spans: ok\n");
	return 0;
}
