// The takeover status dialog (parity lane LH): its composition from the nearest
// zone-timer entry's facts, the scaled line's clip, and its element in the frame
// walk off the death screen.
// [orig: HUD_DrawTakeoverStatus @0x59B630, called from HUD_RenderAllOverlays
//  @0x5A87E4; Render_ClipAndDrawLine2DScaled @0x5D3160]
#include <runtime/hud/hud_elements.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_takeover_status.h>

#include "common/test_font.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// The installed strings this dialog reads, by key; any other key answers its
// fallback (a GameText_GetString miss is "").
GameTextLookup gametext() {
	return [](const char *section, const char *key, const char *fallback) -> std::string {
		if (std::strcmp(section, "Overlays") == 0 && std::strcmp(key, "STROVER_PSP") == 0)
			return "PSP %c";
		if (std::strcmp(section, "Overlays") == 0 &&
				std::strcmp(key, "STROVER_OBJECTIVEPOINT_SHORT") == 0)
			return "Objective %s";
		if (std::strcmp(section, "WPNames") == 0 && std::strcmp(key, "STRWPNAME003") == 0)
			return "Charlie";
		return fallback;
	};
}

// An unnumbered building PSP whose window runs: the PSP name over its letter,
// the window's fraction, the building PSP line one row down.
HudTakeoverFacts building_psp() {
	HudTakeoverFacts f;
	f.present = true;
	f.zone_byte = 0;
	f.zone_index = 1;
	f.distance_q16 = 50 << 16;
	f.bound_radius_q16 = 4 << 16;
	f.spawn_point_def = true;
	f.def_type = 5;
	f.team_a = 1;
	f.team_b = 2;
	f.window_current = 31;
	f.window_limit = 62;
	f.window_active = true;
	return f;
}

// A numbered zone on the value channel: entry[9] the target, [10] the limit,
// [11] the rate, [12] active.
HudTakeoverFacts numbered_zone(int team_a, int team_b, int32_t target, int32_t rate) {
	HudTakeoverFacts f;
	f.present = true;
	f.zone_byte = 3;
	f.zone_index = 2;
	f.team_a = team_a;
	f.team_b = team_b;
	f.value_current = 0x10000 * 31;
	f.value_target = target;
	f.value_limit = 0x10000 * 62;
	f.value_rate = rate;
	f.value_active = true;
	f.count_owner = 3;
	f.count_other = 1;
	return f;
}

void compose_cases() {
	const GameTextLookup text = gametext();
	// No entry, and an unnumbered zone with no window, draw nothing
	// [orig: @0x59B66A..0x59B689].
	CHECK(!compose_takeover_status(HudTakeoverFacts{}, 1, text).shown);
	HudTakeoverFacts quiet = building_psp();
	quiet.window_active = false;
	quiet.value_active = true;
	CHECK(!compose_takeover_status(quiet, 1, text).shown);

	// The building PSP.
	HudTakeoverStatusState s = compose_takeover_status(building_psp(), 1, text);
	CHECK(s.shown && s.name == "PSP B");
	// 50 units less half the bound radius (2): "48m" [orig: @0x59B77B..0x59B79A].
	CHECK(s.distance == "48m");
	CHECK(s.palette == 5); // entry[2] == 2 [orig: @0x59B80E..0x59B811]
	CHECK(std::fabs(s.first_fraction - 0.5f) < 1e-6f);
	CHECK(!s.first_count && s.window_row && !s.second_bar);
	CHECK(s.status == "!Taking over building PSP");
	// Inside half the bound radius the distance floors at zero.
	HudTakeoverFacts near = building_psp();
	near.distance_q16 = 1 << 16;
	CHECK(compose_takeover_status(near, 1, text).distance == "0m");
	// The def picks the line: a vehicle PSP, a plain spawn point, then the
	// non-SpawnPoint defs [orig: @0x59B925..0x59B9D6].
	HudTakeoverFacts f = building_psp();
	f.def_type = 1;
	CHECK(compose_takeover_status(f, 1, text).status == "!Taking over vehicle PSP");
	f.def_type = 3;
	CHECK(compose_takeover_status(f, 1, text).status == "!Taking over spawn point");
	f.spawn_point_def = false;
	f.def_type = 5;
	CHECK(compose_takeover_status(f, 1, text).status == "!Taking over building");
	f.def_type = 1;
	CHECK(compose_takeover_status(f, 1, text).status == "!Taking over vehicle");
	f.def_type = 0;
	CHECK(compose_takeover_status(f, 1, text).status == "!Taking over object");

	// A numbered zone: "Objective <name>", the first bar full while the value
	// runs with entry[1] == entry[2], the second bar the value's fraction, the
	// excess beside it only at a nonzero rate [orig: @0x59B6BD..0x59B70D,
	//  @0x59B871..0x59B87F, @0x59BA45..0x59BA4E, @0x59BA7B..0x59BAC8].
	s = compose_takeover_status(numbered_zone(1, 1, 0x10000 * 40, 62), 1, text);
	CHECK(s.shown && s.name == "Objective Charlie");
	CHECK(s.palette == 3);
	CHECK(s.first_fraction == 1.0f && !s.first_count && !s.window_row);
	CHECK(s.second_bar && std::fabs(s.second_fraction - 0.5f) < 1e-6f);
	CHECK(s.second_count && s.count == "2x");
	CHECK(s.status == "!Setting up camp");
	CHECK(compose_takeover_status(numbered_zone(1, 2, 0x10000 * 40, 62), 1, text).first_fraction == 0.0f);
	// The camp lines [orig: @0x59BADD..0x59BC6E].
	CHECK(compose_takeover_status(numbered_zone(1, 1, 0x10000 * 62, 0), 1, text).status == "!Camp set up");
	CHECK(compose_takeover_status(numbered_zone(2, 2, 0x10000 * 40, 62), 1, text).status ==
			"!Enemy is setting up camp");
	CHECK(compose_takeover_status(numbered_zone(0, 1, 0x10000 * 40, -62), 1, text).status ==
			"!Taking down neutral camp");
	CHECK(compose_takeover_status(numbered_zone(1, 1, 0x10000 * 40, -62), 1, text).status ==
			"!Enemy is destroying camp");
	CHECK(compose_takeover_status(numbered_zone(2, 2, 0x10000 * 40, -62), 1, text).status ==
			"!Destroying enemy camp");
	s = compose_takeover_status(numbered_zone(1, 1, 0x10000 * 40, 0), 1, text);
	CHECK(s.status.empty() && !s.second_count);
	CHECK(compose_takeover_status(numbered_zone(0, 2, 0, -62), 1, text).status == "!No camp");
	CHECK(compose_takeover_status(numbered_zone(0, 0, 0, -62), 1, text).status.empty());
	// A numbered zone's window: the excess beside the first bar, the LFP line,
	// and the second bar one row down at zero [orig: @0x59B8AB..0x59B920].
	HudTakeoverFacts lfp = numbered_zone(1, 2, 0, 0);
	lfp.value_active = false;
	lfp.window_active = true;
	lfp.window_current = 10;
	lfp.window_limit = 40;
	s = compose_takeover_status(lfp, 1, text);
	CHECK(s.first_count && s.window_row && s.second_bar && s.second_fraction == 0.0f && !s.second_count);
	CHECK(std::fabs(s.first_fraction - 0.25f) < 1e-6f && s.status == "!Taking over LFP");
}

// The clip [orig: Render_ClipAndDrawLine2DScaled @0x5D3160].
void clip_cases() {
	int32_t x0 = 200, y0 = 626, x1 = 440, y1 = 626;
	CHECK(clip_design_line(x0, y0, x1, y1) && x0 == 200 && x1 == 440 && y0 == 626 && y1 == 626);
	// Wholly right of the surface: nothing.
	x0 = 1100; y0 = 10; x1 = 1200; y1 = 20;
	CHECK(!clip_design_line(x0, y0, x1, y1));
	// Across the right edge: cut at 1023 [orig: @0x5D3258..0x5D3296].
	x0 = 900; y0 = 100; x1 = 1100; y1 = 300;
	CHECK(clip_design_line(x0, y0, x1, y1) && x1 == 1023 && y1 == 300 + (1023 - 1100) * (100 - 300) / (900 - 1100));
	// Across the left and the bottom edges [orig: @0x5D320B..0x5D3243, @0x5D3336..0x5D336E].
	x0 = -100; y0 = 600; x1 = 100; y1 = 900;
	CHECK(clip_design_line(x0, y0, x1, y1) && x0 == 0 && y0 == 750 && y1 == 767 &&
			x1 == (767 - 900) * (0 - 100) / (750 - 900) + 100);
	// Cut at the left edge, the rest lies below the surface: nothing.
	x0 = -100; y0 = 700; x1 = 100; y1 = 900;
	CHECK(!clip_design_line(x0, y0, x1, y1));
}

// The element: the stdbox, the rule, the bars at LFP_TAKEOVERDLG; none on the
// death screen [orig: `cmp g_DeathScreenActive` @0x5A87E2].
void element_cases(const opennova::fnt::fnt_font_t *font) {
	HudFrameCompiler compiler;
	HudLayout layout;
	layout.lfp_takeover_x = 200;
	layout.lfp_takeover_y = 600;
	compiler.configure(layout, font);
	compiler.configure_label_fonts(font, font, font, 1.0f, 1.0f);
	HudFrameState state;
	state.takeover_status = compose_takeover_status(building_psp(), 1, gametext());
	const HudDrawList &list = compiler.compile(state, 1024.0f, 768.0f);
	bool rule = false;
	for (const HudLine &l : list.lines)
		if (l.x0 == 200.0f && l.y0 == 626.0f && l.x1 == 440.0f && l.y1 == 626.0f && l.color == 0xFFC0C0C0u)
			rule = true;
	CHECK(rule);
	// The first bar's border at (200, 632)..(420, 646) in palette 5.
	bool bar = false;
	for (const HudQuad &q : list.quads)
		if (q.x0 == 200.0f && q.y0 == 632.0f && q.x1 == 420.0f && q.y1 == 646.0f && q.color == 0xFFFF5050u)
			bar = true;
	CHECK(bar);
	bool span = false;
	for (const HudElementSpan &s : list.element_spans)
		if (s.element == HudElement::TakeoverStatus) span = true;
	CHECK(span);
	CHECK(std::strcmp(hud_element_token(HudElement::TakeoverStatus), "takeover_status") == 0);
	state.combat.death_screen = true;
	const HudDrawList &dead = compiler.compile(state, 1024.0f, 768.0f);
	bool dead_span = false;
	for (const HudElementSpan &s : dead.element_spans)
		if (s.element == HudElement::TakeoverStatus) dead_span = true;
	CHECK(!dead_span);
}

} // namespace

int main() {
	opennova::fnt::fnt_font_t font = test_font::uniform_test_font();
	compose_cases();
	clip_cases();
	element_cases(&font);
	opennova::fnt::fnt_free(&font);
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_takeover_status_test OK\n");
	return 0;
}
