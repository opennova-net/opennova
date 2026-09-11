// The SIGHTS card row modes and the sight-scale cycle (runtime/hud/sight_overlay.h):
// the scaled half-extent formula about the row centre, the slide y offset, the
// plain rect, the dotsize cycle with its signed `< 3` wrap, and the scope-zero
// slide multiplier's three arms. [orig: draw_weapon_sight_overlays @0x4dce00;
//  Input_HandleActionBinding_0 case 216 @0x4e0c31; Player_InitPlayer @0x4e178c]

#include <runtime/hud/sight_overlay.h>

#include <cstdio>

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

	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("sight_overlay_test OK\n");
	return 0;
}
