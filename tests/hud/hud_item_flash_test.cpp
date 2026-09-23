// The HUD item flash BMS action 28 sub 37 arms (hud/hud_declutter.h
// HudItemFlash) and the declutter side path it takes: the 16 timers count
// down by the logic-tick delta between HUD frames, clamped at 0, the map
// overlay gate draws an idle or lit-phase timer, and the level-0 rebuild
// leaves the stored hud_detail level alone.
// [orig: RenderState_SetLayerVisibilityByIndex @0x5A3020 (@0x5A302A,
//  CRenderState_SetLayerVisibility(0) @0x5A3031); sub_59A9E0 @0x59A9E0;
//  HUD_DrawMapOverlay @0x5A785B]
#include <runtime/hud/hud_declutter.h>

#include <cstdio>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

static void test_timers_count_down_by_the_tick_delta() {
	HudItemFlash flash;
	flash.tick(100);
	flash.set(3, 40);
	flash.set(4, -5);
	flash.set(16, 9);   // past the 16 timers: not modeled, dropped
	flash.set(-1, 9);
	CHECK(flash.timer(3) == 40);
	flash.tick(100);    // the same tick changes nothing
	CHECK(flash.timer(3) == 40);
	flash.tick(110);
	CHECK(flash.timer(3) == 30);
	CHECK(flash.timer(4) == 0); // a negative timer is at or under any delta
	flash.tick(140);
	CHECK(flash.timer(3) == 0); // the delta reaches it: clamped
	for (int i = 0; i < HudItemFlash::kCount; ++i) CHECK(flash.timer(i) == 0);
}

static void test_a_tick_that_runs_backwards_grows_the_timer() {
	// The last-tick static is never reset, so a clock that restarts lower (the
	// next mission's tick 0) hands the countdown a negative delta; the signed
	// compare keeps the timer and the subtraction grows it. [orig: sub_59A9E0
	// — `sub edx, ecx` @0x59A9F0, `cmp eax, edx; jle` @0x59AA06]
	HudItemFlash flash;
	flash.tick(1000);
	flash.set(2, 50);
	flash.tick(10);
	CHECK(flash.timer(2) == 1040);
}

static void test_map_gate_draws_idle_or_lit_phase() {
	CHECK(hud_item_flash_shown(0));
	CHECK(hud_item_flash_shown(0x10));
	CHECK(hud_item_flash_shown(0x31));
	CHECK(!hud_item_flash_shown(0x20));
	CHECK(!hud_item_flash_shown(0x0F));
}

static void test_level_zero_side_path_keeps_the_stored_level() {
	HudDeclutter d;
	d.begin_authoring();
	d.set_mask(kDeclutterSpinmap, 0x2);   // level 1 only
	d.set_level(1);
	CHECK(d.visible()[kDeclutterSpinmap]);
	d.apply_level(0);
	CHECK(!d.visible()[kDeclutterSpinmap]);
	CHECK(d.level() == 1);
	d.set_mask(kDeclutterClock, 0x1);     // the next rebuild uses the stored level
	CHECK(d.visible()[kDeclutterSpinmap]);
	CHECK(!d.visible()[kDeclutterClock]);
}

int main() {
	test_timers_count_down_by_the_tick_delta();
	test_a_tick_that_runs_backwards_grows_the_timer();
	test_map_gate_draws_idle_or_lit_phase();
	test_level_zero_side_path_keeps_the_stored_level();
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_item_flash: all passed\n");
	return 0;
}
