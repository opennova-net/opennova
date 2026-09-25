// The SIGHTS card row modes and the sight-scale cycle (runtime/hud/sight_overlay.h):
// the scaled half-extent formula about the row centre, the slide y offset, the
// plain rect, the dotsize cycle with its signed `< 3` wrap, and the scope-zero
// slide multiplier's three arms. [orig: draw_weapon_sight_overlays @0x4dce00;
//  Input_HandleActionBinding_0 case 216 @0x4e0c31; Player_InitPlayer @0x4e178c]

#include <runtime/hud/scope_circle_mask.h>
#include <runtime/hud/sight_overlay.h>

#include <cstdio>
#include <cmath>

namespace {

namespace hud = opennova::hud;

int failures = 0;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

bool rect_is(const hud::SightRect &r, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
	return r.x1 == x1 && r.y1 == y1 && r.x2 == x2 && r.y2 == y2;
}

hud::SightRowSpec box(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
	hud::SightRowSpec row;
	row.x1 = x1;
	row.y1 = y1;
	row.x2 = x2;
	row.y2 = y2;
	return row;
}

} // namespace

int main() {
    const hud::SightRect card{384, 256, 640, 512};
    const auto four_three = hud::sight_rect_to_viewport(card, 1920, 1080, 0);
    const auto wide = hud::sight_rect_to_viewport(card, 1920, 1080, 2);
    const auto native = hud::sight_rect_to_viewport(card, 1920, 1080);
    const auto sixteen_ten = hud::sight_rect_to_viewport(card, 1920, 1080, 1);
    const auto mode_three = hud::sight_rect_to_viewport(card, 1920, 1080, 3);
    expect(four_three.y1 == 360.0f && four_three.y2 == 720.0f, "4:3 preserves rounded Y");
    expect(wide.y1 == 300.0f && wide.y2 == 780.0f, "16:9 selects .5625");
    expect(wide.y1 == native.y1, "native selects the actual viewport ratio");
    expect(std::abs(sixteen_ten.y1 - 315.0f) < .001f, "16:10 selects .60000002");
    expect(std::abs(mode_three.y1 - 324.0f) < .001f, "mode 3 uses the retail .625 literal");
	// The per-player index: default 1, +1 while the sum stays (signed) below
	// 3, else 0 — so 1 -> 2 -> 0 -> 1, and a negative index climbs.
	expect(hud::kSightScaleIndexDefault == 1, "the player-init default is 1");
	expect(hud::next_sight_scale_index(1) == 2, "1 -> 2");
	expect(hud::next_sight_scale_index(2) == 0, "2 wraps to 0");
	expect(hud::next_sight_scale_index(0) == 1, "0 -> 1");
	expect(hud::next_sight_scale_index(-3) == -2, "the signed compare keeps a negative sum");

	// The scaled half-extent: ((extent) * (index + 2)) >> 3.
	expect(hud::sight_scaled_half_extent(200, 1) == 75, "index 1: 3/8 of the extent each side");
	expect(hud::sight_scaled_half_extent(200, 0) == 50, "index 0: 2/8 each side (half box)");
	expect(hud::sight_scaled_half_extent(200, 2) == 100, "index 2: 4/8 each side (full box)");
	expect(hud::sight_scaled_half_extent(33, 1) == 12, "the arithmetic shift floors (99 >> 3)");

	// SCALED rows draw about the centre.
	{
		hud::SightRowSpec row = box(100, 200, 300, 400);
		row.scale = true;
		expect(rect_is(hud::sight_row_rect(row, 1, 0), 125, 225, 275, 375),
				"the default index draws three quarters of the box about (200, 300)");
		expect(rect_is(hud::sight_row_rect(row, 0, 0), 150, 250, 250, 350),
				"index 0 draws the half box");
		expect(rect_is(hud::sight_row_rect(row, 2, 0), 100, 200, 300, 400),
				"index 2 draws the full authored box");
		// Odd sums halve with the arithmetic shift: (101 + 300) >> 1 = 200.
		hud::SightRowSpec odd = box(101, 200, 300, 401);
		odd.scale = true;
		expect(rect_is(hud::sight_row_rect(odd, 2, 0), 101, 200, 299, 400),
				"the centre and half-extents floor independently");
		// The slide multiplier never reaches a scaled row.
		row.slide = true;
		row.slide_frames = 8;
		expect(rect_is(hud::sight_row_rect(row, 1, 5), 125, 225, 275, 375),
				"a row flagged both draws scaled: the scale test runs first");
	}

	// SLIDE rows shift y by frames * multiplier, x untouched.
	{
		hud::SightRowSpec row = box(100, 200, 300, 400);
		row.slide = true;
		row.slide_frames = 8;
		expect(rect_is(hud::sight_row_rect(row, 1, 3), 100, 224, 300, 424),
				"slide: y1 and y2 both move by 8 * 3");
		expect(rect_is(hud::sight_row_rect(row, 1, 0), 100, 200, 300, 400),
				"multiplier 0 leaves the slide row at its authored rect");
		expect(rect_is(hud::sight_row_rect(row, 1, -2), 100, 184, 300, 384),
				"a negative multiplier slides up");
	}

	// PLAIN rows keep their rect whatever the inputs.
	expect(rect_is(hud::sight_row_rect(box(10, 20, 30, 40), 0, 7), 10, 20, 30, 40),
			"a plain row ignores both inputs");

	// The slide multiplier's arms.
	{
		hud::ScopeZeroInputs in;
		expect(hud::sight_slide_multiplier(in) == 0,
				"the absent-key case (all zero) resolves to 0");

		// Zero word 0 with the def default: (default << 16) / 1638400 (= default / 25)
		// * 25 / step. scope_max_zero 10 100 300 -> 12 * 25 / 100 = 3.
		in.scope_max_zero_steps = 10;
		in.scope_zero_step = 100;
		in.scope_zero_default = 300;
		expect(hud::sight_slide_multiplier(in) == 3, "the default-zero arm: 300 m at 100 m steps");
		in.scope_zero_default = 310;
		expect(hud::sight_slide_multiplier(in) == 3,
				"the 25 m quantisation truncates first (310 / 25 = 12)");
		in.scope_zero_default = 325;
		expect(hud::sight_slide_multiplier(in) == 3, "325 / 25 = 13, * 25 / 100 = 3 (idiv truncates)");
		in.scoring_disabled = true;
		expect(hud::sight_slide_multiplier(in) == 0, "a set byte_24D217C skips the default arm to 0");
		in.scoring_disabled = false;
		in.scope_zero_step = 0;
		expect(hud::sight_slide_multiplier(in) == 0, "a zero step answers 0 (retail faults)");
		in.scope_zero_step = 100;

		// Zero word -1: rangefinder / (step << 16), negative -> 0, capped at the max.
		in.slot_zero_word = -1;
		in.rangefinder_q16 = 450 << 16;
		expect(hud::sight_slide_multiplier(in) == 4, "the rangefinder arm: 450 m / 100 m = 4");
		in.rangefinder_q16 = -(150 << 16);
		expect(hud::sight_slide_multiplier(in) == 0,
				"a negative quotient (-150 m / 100 m = -1) answers 0");
		in.rangefinder_q16 = 5000 << 16;
		expect(hud::sight_slide_multiplier(in) == 10, "the range caps at scope_max_zero_steps");
		in.scoring_disabled = true;
		expect(hud::sight_slide_multiplier(in) == 10,
				"byte_24D217C gates only the default arm");
		in.scoring_disabled = false;

		// Any other word is the multiplier itself, sign-extended.
		in.slot_zero_word = 7;
		expect(hud::sight_slide_multiplier(in) == 7, "a manual zero word is the multiplier");
		in.slot_zero_word = -5;
		expect(hud::sight_slide_multiplier(in) == -5, "the word sign-extends");
		// Word 0 with no def default falls to the word arm: 0.
		in.slot_zero_word = 0;
		in.scope_zero_default = 0;
		expect(hud::sight_slide_multiplier(in) == 0, "word 0 without a def default is 0");
	}

	// The shipped JOX weapon.def forms at the default zero (zero word 0, no
	// rangefinder sample, scoring enabled). The M16/M203 `scope_max_zero 10 50
	// 0 0` owns the ONE shipped `slide` row (m16203b.tga 112 -68 892 703, 33
	// frames): its default distance is 0, so the +0xA0 gate falls through to
	// the zero word and the row sits at its authored rect. The 200 m defaults
	// (`10 100 200 x`) give (200 << 16) / 1638400 = 8, * 25 / 100 = 2;
	// `1 100 100 1` gives 4 * 25 / 100 = 1; `1 300 300` gives 12 * 25 / 300 = 1;
	// `10 100 300 1` gives 12 * 25 / 100 = 3.
	{
		hud::SightRowSpec m203_row = box(112, -68, 892, 703);
		m203_row.slide = true;
		m203_row.slide_frames = 33;
		hud::ScopeZeroInputs m203;
		m203.scope_max_zero_steps = 10;
		m203.scope_zero_step = 50;
		m203.scope_zero_default = 0;
		expect(hud::sight_slide_multiplier(m203) == 0,
				"M16/M203 `10 50 0 0`: no default distance -> multiplier 0");
		expect(rect_is(hud::sight_row_rect(m203_row, 1, hud::sight_slide_multiplier(m203)),
					   112, -68, 892, 703),
				"the M203 sight sits at its authored rect at the default zero");

		hud::ScopeZeroInputs two_hundred;
		two_hundred.scope_max_zero_steps = 10;
		two_hundred.scope_zero_step = 100;
		two_hundred.scope_zero_default = 200;
		expect(hud::sight_slide_multiplier(two_hundred) == 2, "`10 100 200 x` -> 2");
		expect(rect_is(hud::sight_row_rect(m203_row, 1, 2), 112, -2, 892, 769),
				"a 33-frame slide row at multiplier 2 shifts 66 px down");

		hud::ScopeZeroInputs hundred;
		hundred.scope_max_zero_steps = 1;
		hundred.scope_zero_step = 100;
		hundred.scope_zero_default = 100;
		expect(hud::sight_slide_multiplier(hundred) == 1, "`1 100 100 1` -> 1");

		hud::ScopeZeroInputs three_hundred;
		three_hundred.scope_max_zero_steps = 1;
		three_hundred.scope_zero_step = 300;
		three_hundred.scope_zero_default = 300;
		expect(hud::sight_slide_multiplier(three_hundred) == 1, "`1 300 300` -> 1");

		hud::ScopeZeroInputs far_default;
		far_default.scope_max_zero_steps = 10;
		far_default.scope_zero_step = 100;
		far_default.scope_zero_default = 300;
		expect(hud::sight_slide_multiplier(far_default) == 3, "`10 100 300 1` -> 3");
	}

	// ---------------------------------------------------------------------
	// The scoped-view circle mask that follows the card on the Scoped arm
	// [orig: Hud_DrawScopeCircleMask @0x5d17a0; the cross/grid @0x5d1160].
	{
		// The fork's order: binoculars, then Sighted, then Scoped.
		expect(hud::scoped_view_overlay(true, true, true) ==
						hud::ScopedViewOverlay::kBinocularMask,
				"binoculars pre-empt both card selectors");
		expect(hud::scoped_view_overlay(false, true, true) ==
						hud::ScopedViewOverlay::kSightedCard,
				"Sighted pre-empts Scoped, so no circle mask");
		expect(hud::scoped_view_overlay(false, false, true) ==
						hud::ScopedViewOverlay::kScopedCardWithCircleMask,
				"the Scoped arm always reaches the circle mask");
		expect(hud::scoped_view_overlay(false, false, false) ==
						hud::ScopedViewOverlay::kEntityMarkers,
				"neither selector draws the entity markers");
		expect(hud::scoped_selector_from_def(1u, 0u), "Flags & 1 without Inset is Scoped");
		expect(!hud::scoped_selector_from_def(1u, 0x200u), "Inset takes the other byte");
		expect(hud::sighted_selector_from_def(2u, false), "Flags & 2 is Sighted");
		expect(!hud::sighted_selector_from_def(2u, true), "SWITCHFROM clears the Sighted byte");

		// A 1024x768 screen's inclusive overlay rect (0, 0)..(1023, 767)
		// [orig: Viewport_SetFullScreen @0x5d30e0]: centre (511, 383), ring
		// size (767 >> 3) + (767 >> 1) = 478, radii 339.38 / 717; the native
		// 4:3 ratio pins scale_y at 1 and scale_x = 511 / 383 x 0.75, a hair
		// over 1.
		const hud::ScopeCircleMaskGeometry g =
				hud::scope_circle_mask_geometry(0, 0, 1023, 767, 1024);
		expect(g.center_x == 511.0f && g.center_y == 383.0f, "the mask centres on the rect");
		expect(g.ring_size == 478.0f, "ring size = (h >> 3) + (h >> 1)");
		expect(std::abs(g.radius_inner - 339.38f) < .01f, "inner radius = 0.71 * ring size");
		expect(g.radius_outer == 717.0f, "outer radius = 1.5 * ring size");
		expect(std::abs(g.scale_x - 1.0006528f) < .00001f && std::abs(g.scale_y - 1.0f) < .0001f,
				"the native 4:3 ratio: scale_y 1, scale_x 511 / 383 x 0.75");
		expect(std::abs(g.arm_half_thickness - 3.2f) < .0001f, "arm half thickness = W / 320");
		expect(std::abs(g.tick_spacing - 16.0f) < .0001f, "tick pitch = W / 64");
		// The outer radius clears the corner, so the annulus really masks the
		// whole surface outside the scope circle.
		expect(g.radius_outer > std::sqrt(512.0f * 512.0f + 384.0f * 384.0f),
				"the outer ring covers the viewport corners");

		// A forced 4:3 ratio on a 16:9 surface keeps scale_y at 1 and widens
		// scale_x, the retail ellipse.
		const hud::ScopeCircleMaskGeometry wide =
				hud::scope_circle_mask_geometry(0, 0, 1919, 1079, 1920, 0);
		expect(std::abs(wide.scale_y - 1.0f) < .0001f, "mode 0 pins scale_y at 1");
		expect(std::abs(wide.scale_x - (959.0f / 539.0f) * 0.75f) < .0001f,
				"scale_x follows the rect's centre ratio");

		const hud::ScopeCircleMask rowless =
				hud::build_scope_circle_mask(0, 0, 1023, 767, 1024, true);
		expect(static_cast<int>(rowless.ring.size()) == hud::kScopeRingVertexCount,
				"the ring submits 130 strip vertices");
		expect(rowless.ring_indices.size() == 128u * 3u, "128 triangles expand the strip");
		expect(rowless.ring[0].argb == hud::kScopeRingInnerColor &&
						rowless.ring[1].argb == hud::kScopeRingOuterColor,
				"inner 0xFF181820, outer 0xFF040408");
		// Segment 0 sits at table index 0: cos 1, sin 0 -> due right of centre
		// (x scaled by scale_x).
		expect(std::abs(rowless.ring[0].x - 850.6016f) < .05f &&
						std::abs(rowless.ring[0].y - 383.0f) < .05f,
				"segment 0 is the inner vertex due right of centre");
		expect(std::abs(rowless.ring[1].x - 1228.468f) < .05f,
				"its outer twin shares the angle");
		// Segment 16 is a quarter turn: +Y in the table is UP on screen.
		expect(std::abs(rowless.ring[32].x - 511.0f) < .05f &&
						std::abs(rowless.ring[32].y - (383.0f - 339.38f)) < .05f,
				"a quarter of the ring is straight up");
		// The 65th stop closes the loop back onto the first.
		expect(std::abs(rowless.ring[128].x - rowless.ring[0].x) < .05f &&
						std::abs(rowless.ring[128].y - rowless.ring[0].y) < .05f,
				"the 65th stop closes the ring");

		// The cross: four spokes from the exact centre out to 0.71 of the
		// radii, breaking at 0.4, with the half-thickness across the axis.
		expect(rowless.crosshair.size() == 28u && rowless.crosshair_indices.size() == 72u,
				"four 7-vertex spokes");
		expect(rowless.crosshair[0].x == 511.0f && rowless.crosshair[0].y == 383.0f &&
						rowless.crosshair[0].argb == hud::kScopeCrosshairCenterColor,
				"each spoke starts at the centre with alpha 0x20");
		// Every endpoint passes through retail's ftol truncation: A = scale_x x
		// 478 = 478.31, so cx - 0.4 A = 319.68 truncates to 319 and cx - 0.71 A
		// = 171.40 to 171.
		expect(rowless.crosshair[2].x == 319.0f &&
						rowless.crosshair[2].argb == hud::kScopeCrosshairAxisColor,
				"the left spoke breaks at trunc(cx - 0.4 A) = 319");
		expect(rowless.crosshair[5].x == 171.0f,
				"its outer end is trunc(cx - 0.71 A) = 171");
		expect(rowless.crosshair[1].argb == hud::kScopeCrosshairEdgeColor &&
						std::abs(rowless.crosshair[1].y - (383.0f - 3.2f)) < .001f,
				"the off-axis vertices are transparent at +/- W/320");
		expect(rowless.crosshair[7 + 2].y == 191.0f &&
						rowless.crosshair[7 + 2].x == 511.0f,
				"spoke 1 runs up to trunc(cy - 0.4 * 478) = 191");
		expect(rowless.crosshair[7 + 5].y == 43.0f, "and out to 43");
		expect(rowless.crosshair[14 + 2].x == 702.0f &&
						rowless.crosshair[14 + 5].x == 850.0f,
				"spoke 2 runs right (702 / 850)");
		expect(rowless.crosshair[21 + 2].y == 574.0f &&
						rowless.crosshair[21 + 5].y == 722.0f,
				"spoke 3 runs down (574 / 722)");

		// The grid: 4 ticks per direction at i * W/64, unscaled screen pixels.
		expect(rowless.grid.size() == 80u && rowless.grid_indices.size() == 192u,
				"sixteen 5-vertex diamonds");
		expect(rowless.grid[0].x == 511.0f + 16.0f && rowless.grid[0].y == 383.0f &&
						rowless.grid[0].argb == hud::kScopeGridTickCenterColor,
				"the first tick is one pitch to the right");
		expect(rowless.grid[15 * 5].y == 383.0f - 64.0f &&
						rowless.grid[15 * 5].x == 511.0f,
				"the last tick is four pitches up");
		expect(rowless.grid[1].argb == hud::kScopeGridTickEdgeColor &&
						std::abs(rowless.grid[1].x - (511.0f + 16.0f + 3.2f)) < .001f,
				"the diamond points sit one arm half-thickness out at alpha 0x10");

		// A card that DREW rows suppresses only the cross and the grid; the
		// annulus is unconditional on the Scoped arm.
		const hud::ScopeCircleMask carded =
				hud::build_scope_circle_mask(0, 0, 1023, 767, 1024, false);
		expect(static_cast<int>(carded.ring.size()) == hud::kScopeRingVertexCount,
				"authored SIGHTS rows never suppress the circle mask");
		expect(carded.crosshair.empty() && carded.grid.empty(),
				"they suppress the inner cross and grid only");

		// The NVG Sighted arm lays the card over the 512 square with the
		// frame's own selected ratio: a 16:9 native frame corrects Y by
		// 3 / (4 x 0.5625) about 256. [orig: terrain_scene_render
		// @0x5d08d8..0x5d0927]
		const hud::SightViewportRect nvg_card = hud::sight_rect_to_viewport_at_ratio(
				hud::SightRect{256, 288, 768, 480}, 512.0f, 512.0f, 0.5625f);
		expect(nvg_card.x1 == 128.0f && nvg_card.x2 == 384.0f, "x scales to the 512 square");
		expect(std::abs(nvg_card.y1 - (256.0f + (192.0f - 256.0f) * (4.0f / 3.0f))) < .01f &&
						std::abs(nvg_card.y2 - (256.0f + (320.0f - 256.0f) * (4.0f / 3.0f))) < .01f,
				"y scales to the 512 square, then corrects about 256 by the frame's ratio");

		// The NVG lens's reticle: no annulus, the cross at UNIT scale about
		// the same centre and ring size -- on a forced 4:3 ratio over a 16:9
		// surface the spokes stay round where the mask's would stretch.
		// [orig: draw_minimap_compass_border @0x5d2798..0x5d27bc]
		const hud::ScopeCircleMask lens =
				hud::build_nvg_lens_reticle(0, 0, 1919, 1079, 1920);
		expect(lens.ring.empty() && lens.ring_indices.empty(), "the lens draws its own ring");
		expect(lens.geometry.scale_x == 1.0f && lens.geometry.scale_y == 1.0f,
				"unit scales");
		expect(lens.geometry.center_x == 959.0f && lens.geometry.center_y == 539.0f &&
						lens.geometry.ring_size == 673.0f,
				"the lens's centre and (1079 >> 3) + (1079 >> 1) ring");
		expect(lens.crosshair.size() == 28u && lens.grid.size() == 80u,
				"the four spokes and sixteen ticks");
		// trunc(959 - 0.4 x 673) = 689 and trunc(959 - 0.71 x 673) = 481 on
		// the left spoke; the up spoke uses the SAME ring at unit scale.
		expect(lens.crosshair[2].x == 689.0f && lens.crosshair[5].x == 481.0f,
				"the left spoke at unit scale");
		expect(lens.crosshair[7 + 2].y == 269.0f && lens.crosshair[7 + 5].y == 61.0f,
				"the up spoke at unit scale");
	}

	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("sight_overlay_test OK\n");
	return 0;
}
