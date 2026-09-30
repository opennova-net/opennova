// The capture-point labels (hud/hud_capture_labels.h): the slot walk's gates,
// its strings and bar facts, and the marker drawer's types 8/9 with the
// binoculars progress bar.
// [orig: Render_CapturePointLabels @0x5a2840 -> HUD_DrawEntityMarker @0x593140,
//  HUD_DrawProgressBar @0x59B340]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_capture_labels.h>
#include <runtime/hud/hud_frame.h>

#include "../fixtures/minimal_fnt_builder.h"

using namespace opennova;
using namespace opennova::hud;

static int failures = 0;
#define CHECK(x)                                                                                   \
	do {                                                                                           \
		if (!(x)) {                                                                                \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

constexpr int32_t u = 65536;

HudMinimapMarker zone(HudMinimapBank bank, uint16_t handle, uint8_t team) {
	HudMinimapMarker m;
	m.bank = uint8_t(bank);
	m.handle = handle;
	m.flags = 0x10;
	m.source = 0x41; // the zone number at classification, a capture bit set
	m.entity_known = 1;
	m.team = team;
	m.zone_number = 0x41;
	m.entity_bits = kMarkerEntityZoneDef;
	m.zone_index = 2;
	m.entity_x = 10 * u;
	m.entity_y = 20 * u;
	m.entity_z = 3 * u;
	return m;
}

// The gametext the walk reads: WPNames/STRWPNAME%03d and the short template.
std::string lookup(const char *section, const char *key, const char *fallback) {
	if (std::strcmp(section, "Overlays") == 0 &&
			std::strcmp(key, "STROVER_OBJECTIVEPOINT_SHORT") == 0)
		return "Objective %s";
	if (std::strcmp(section, "WPNames") == 0) {
		if (std::strcmp(key, "STRWPNAME000") == 0) return "Null";
		if (std::strcmp(key, "STRWPNAME003") == 0) return "Charlie";
	}
	return fallback;
}

// The walk: bank order, every gate, the strings, the flash and the bar facts.
// [orig: @0x5a2892..0x5a2920 (the gates), @0x5a294a..0x5a29a8 (the strings),
//  @0x5a2a13..0x5a2a5d (the colour and the flash), @0x5a2b14..0x5a2b9c (the bar)]
void walk() {
	std::vector<HudMinimapMarker> markers;
	// Listed special first: the walk still takes the banks in address order.
	auto special = zone(HudMinimapBank::kSpecial, 40, 2);
	special.flags = 0; // a special-bank slot without the 0x40 bit still walks
	markers.push_back(special);
	auto timed = zone(HudMinimapBank::kTransient, 10, 1);
	timed.timer_known = 1;
	timed.timer_active = 1;
	timed.timer_team = 1;
	timed.timer_bar_team = 2;
	timed.timer_value = 31;
	timed.timer_limit = 62;
	timed.timer_rate = -1;
	markers.push_back(timed);
	markers.push_back(zone(HudMinimapBank::kPersistent, 20, 0));
	auto flagged = zone(HudMinimapBank::kTransient, 11, 1);
	flagged.flags = 0x40;
	markers.push_back(flagged);
	markers.push_back(zone(HudMinimapBank::kTransient, 0xFFFF, 1));
	auto unresolved = zone(HudMinimapBank::kTransient, 12, 1);
	unresolved.entity_known = 0;
	markers.push_back(unresolved);
	auto no_def = zone(HudMinimapBank::kTransient, 13, 1);
	no_def.entity_bits = kMarkerEntityHasModel;
	markers.push_back(no_def);
	auto unnumbered = zone(HudMinimapBank::kTransient, 14, 1);
	unnumbered.zone_number = 0;
	markers.push_back(unnumbered);
	auto no_bit = zone(HudMinimapBank::kTransient, 15, 1);
	no_bit.source = 0x3F;
	markers.push_back(no_bit);
	std::vector<HudCaptureLabel> labels;
	capture_point_label_walk(markers, 1, lookup, labels);
	CHECK(labels.size() == 3);
	if (labels.size() != 3)
		return;
	// The timed own zone: type 9, palette[3], the letter and the name of
	// index 2, the flash, the bar in team 2's colour half full, lifted 2 u.
	const HudCaptureLabel &a = labels[0];
	CHECK(a.type == kMarkerTypeZoneOwn && a.palette == 3);
	CHECK(a.letter == "C" && a.name == "Objective Charlie");
	CHECK(a.flash && a.timer && a.bar_palette == 5 && a.bar_fraction == 0.5f);
	CHECK(a.position[0] == 10 * u && a.position[1] == 20 * u && a.position[2] == 3 * u + 0x20000);
	// No entry: the blank letter, no name, no flash, no bar.
	const HudCaptureLabel &b = labels[1];
	CHECK(b.type == kMarkerTypeZoneOther && b.palette == 1);
	CHECK(b.letter == " " && b.name.empty() && !b.flash && !b.timer);
	CHECK(labels[2].palette == 5 && labels[2].type == kMarkerTypeZoneOther);

	// The flash needs the viewer's own team, a draining rate and entry[1]
	// naming the zone's team; an inactive entry fills 0; a zone off the
	// spawn list indexes -1 -> STRWPNAME000 and '@'.
	timed.timer_rate = 0;
	timed.timer_active = 0;
	timed.zone_index = -1;
	capture_point_label_walk({ timed }, 1, lookup, labels);
	CHECK(labels.size() == 1 && !labels[0].flash && labels[0].bar_fraction == 0.0f);
	CHECK(labels[0].letter == "@" && labels[0].name == "Objective Null");
	timed.timer_rate = -1;
	timed.timer_team = 2;
	capture_point_label_walk({ timed }, 1, lookup, labels);
	CHECK(!labels[0].flash);
	timed.timer_team = 1;
	capture_point_label_walk({ timed }, 2, lookup, labels);
	CHECK(!labels[0].flash && labels[0].type == kMarkerTypeZoneOther);
	// A missing template (GameText_GetString's miss is "") leaves no name.
	timed.zone_index = 2;
	capture_point_label_walk({ timed }, 1, GameTextLookup{}, labels);
	CHECK(labels[0].name.empty() && labels[0].letter == "C" && labels[0].flash);
}

size_t count_page_glyphs(const HudDrawList &d, size_t begin, size_t end) {
	return std::count_if(d.glyphs.begin() + long(begin), d.glyphs.begin() + long(end),
			[](const GameFontQuad &g) { return g.page == 0; });
}

// The element: the stem, the tile, the letter, the name and the bar.
// [orig: HUD_DrawEntityMarker @0x5931b9..0x5937cb; Render_CapturePointLabels
//  @0x5a2ac1..0x5a2bbd]
void element() {
	const fnt::fnt_font_t font = minimal_fnt::uniform_test_font();
	HudLayout layout;
	layout.lfp_tile_own_texture_valid = true;
	layout.lfp_tile_other_texture_valid = true;
	HudFrameCompiler compiler;
	compiler.configure(layout, &font);
	HudFrameState state;
	HudCaptureLabel label;
	label.type = kMarkerTypeZoneOwn;
	label.palette = 3;
	label.letter = "C";
	label.name = "Objective Charlie";
	label.timer = true;
	label.bar_palette = 5;
	label.bar_fraction = 0.5f;
	label.point = { 500.6f, 400.2f, 0, true, 16 << 16 };
	state.combat.capture_labels = { label };
	state.ticks = 8; // off the flash phase
	const HudDrawList &d = compiler.compile(state, 1024, 768);
	// Out of binoculars: a leading break, then stem | tile + letter.
	CHECK(d.order_breaks.size() == 3);
	if (d.order_breaks.size() != 3)
		return;
	const auto &b0 = d.order_breaks[0], &b1 = d.order_breaks[1], &b2 = d.order_breaks[2];
	// The stem: the letter's height (the uniform font's 16 px) straight up
	// from the integral point, in palette[3].
	CHECK(b1.lines - b0.lines == 1);
	const HudLine &stem = d.lines[b0.lines];
	CHECK(stem.x0 == 500.0f && stem.y0 == 400.0f && stem.x1 == 500.0f && stem.y1 == 384.0f);
	CHECK(stem.color == 0xFF80A0FFu);
	// The own-zone tile: 32 x 32 design units centred 16 above the stem's
	// top, the half-bright colour under MODULATE2X.
	CHECK(b2.tris - b1.tris == 2);
	const HudTri &tile = d.tris[b1.tris];
	CHECK(tile.texture == kHudTexLfpTileOwn);
	CHECK(tile.a.x == 484.0f && tile.a.y == 352.0f && tile.c.x == 516.0f && tile.c.y == 384.0f);
	CHECK(tile.a.color == 0xFF80A0FEu);
	// The letter only (no binoculars, no name): one glyph, half-bright.
	CHECK(count_page_glyphs(d, b1.glyphs, b2.glyphs) == 1);
	CHECK(d.glyphs[b1.glyphs].color == 0xFF40507Fu);
	// Its row: 400 - trunc-free 16 * 2.3f = 363.2 -> 363, centred on x - 2.
	const float letter_top = d.glyphs[b1.glyphs].y_top;
	const float letter_mid = (d.glyphs[b1.glyphs].x_top_left + d.glyphs[b1.glyphs].x_top_right) / 2;
	CHECK(std::abs(letter_top - 363.0f) <= 0.5f);
	CHECK(std::abs(letter_mid - 498.0f) <= 1.0f);
	CHECK(d.quads.size() == b2.quads); // no bar

	// In binoculars: the name at the ground point, then the bar 36 x 8 about
	// the point, 20 below it, in team 2's palette[5], half filled.
	state.binoculars_view_active = true;
	const HudDrawList &bin = compiler.compile(state, 1024, 768);
	CHECK(bin.order_breaks.size() == 4);
	if (bin.order_breaks.size() != 4)
		return;
	const auto &c1 = bin.order_breaks[1], &c2 = bin.order_breaks[2], &c3 = bin.order_breaks[3];
	CHECK(count_page_glyphs(bin, c1.glyphs, c2.glyphs) == 1 + std::strlen("Objective Charlie"));
	CHECK(c3.quads - c2.quads == 3);
	const HudQuad &border = bin.quads[c2.quads];
	CHECK(border.x0 == 464.0f && border.x1 == 536.0f && border.y0 == 420.0f && border.y1 == 428.0f);
	CHECK(border.color == 0xFFFF5050u);
	CHECK(bin.quads[c2.quads + 1].color == 0xFF000000u);
	const HudQuad &fill = bin.quads[c2.quads + 2];
	CHECK(fill.x0 == 466.0f && fill.x1 == 466.0f + 34.0f && fill.color == 0xFFFF5050u);

	// The flash phase: (ticks & 0x18) == 0 swaps in yellow for the stem,
	// the tile and the texts; the bar keeps entry[2]'s colour.
	state.combat.capture_labels[0].flash = true;
	state.ticks = 32;
	const HudDrawList &lit = compiler.compile(state, 1024, 768);
	CHECK(lit.lines[lit.order_breaks[0].lines].color == 0xFFFFFF00u);
	CHECK(lit.tris[lit.order_breaks[1].tris].a.color == 0xFFFEFE00u);
	CHECK(lit.glyphs[lit.order_breaks[1].glyphs].color == 0xFF7F7F00u);
	CHECK(lit.quads[lit.order_breaks[2].quads].color == 0xFFFF5050u);

	// Another team's zone binds lfp_alf; an unloaded tile draws no quad; a
	// clipped point draws nothing, the bar included; no labels, no breaks.
	state.combat.capture_labels[0].type = kMarkerTypeZoneOther;
	const HudDrawList &other = compiler.compile(state, 1024, 768);
	CHECK(other.tris[other.order_breaks[1].tris].texture == kHudTexLfpTileOther);
	layout.lfp_tile_other_texture_valid = false;
	compiler.update_layout(layout);
	const HudDrawList &bare = compiler.compile(state, 1024, 768);
	CHECK(bare.order_breaks[2].tris == bare.order_breaks[1].tris);
	state.combat.capture_labels[0].point.clip = 8;
	const HudDrawList &clipped = compiler.compile(state, 1024, 768);
	CHECK(clipped.order_breaks.size() == 1 && clipped.lines.size() == clipped.order_breaks[0].lines &&
			clipped.quads.size() == clipped.order_breaks[0].quads);
	state.combat.capture_labels.clear();
	CHECK(compiler.compile(state, 1024, 768).order_breaks.empty());
	// An empty letter (a NUL %c) sizes the marker at scale * 20.0.
	label.letter.clear();
	state.combat.capture_labels = { label };
	state.binoculars_view_active = false;
	const HudDrawList &blank = compiler.compile(state, 1024, 768);
	CHECK(blank.lines[blank.order_breaks[0].lines].y1 == 380.0f);
}

// GameFont_MeasureCharHeight: the SPACE glyph's extent whatever the byte,
// truncated, then the slot scale, truncated; a control byte measures 0.
// [orig: GameFont_MeasureCharHeight @0x580a80; CGameFont_GetCharExtent
//  @0x674e2c..0x674e44, @0x674e57]
void char_height() {
	fnt::fnt_font_t font = minimal_fnt::uniform_test_font();
	font.design_width = 1024; // 16 texels * 800 / 1024 = 12.5 -> 12
	font.glyphs['A' - fnt::FNT_FIRST_CHAR].uv.v1 = 30.0f / 256.0f; // never read
	GameFont f;
	f.set_font(&font);
	CHECK(f.char_height('A', 1.0f) == 12.0f);
	CHECK(f.char_height(' ', 1.5f) == 18.0f);
	CHECK(f.char_height('0', 1.1f) == 13.0f); // 12 * 1.1 = 13.2 -> 13
	CHECK(f.char_height('\t', 1.0f) == 12.0f);
	CHECK(f.char_height(0x05, 1.0f) == 0.0f);
}

} // namespace

int main() {
	walk();
	element();
	char_height();
	if (!failures)
		std::puts("hud_capture_labels_test: all checks passed");
	return failures ? 1 : 0;
}
