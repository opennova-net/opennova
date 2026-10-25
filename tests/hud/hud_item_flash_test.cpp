// The HUD item flash BMS action 28 sub 37 arms (hud/hud_declutter.h
// HudItemFlash) and the declutter side path it takes: the 16 timers count
// down by the logic-tick delta since the last tick that ran, clamped at 0, the
// overlay-buffer reset zeroes that last tick, the map overlay gate draws an
// idle or lit-phase timer, and the level-0 rebuild leaves the stored
// hud_detail level alone. Where the tick runs (the overlay pass's head) is
// hud_overlay_pass_head's.
// [orig: RenderState_SetLayerVisibilityByIndex @0x5A3020 (@0x5A302A,
//  CRenderState_SetLayerVisibility(0) @0x5A3031); HUD_TickItemFlashTimers @0x59A9E0;
//  HUD_ResetAllOverlayBuffers @0x59DD7D; HUD_DrawMapOverlay @0x5A785B]
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

static void test_the_reset_zeroes_the_last_tick_and_keeps_the_timers() {
	// The overlay-buffer reset (every round init and mission start) zeroes the
	// last tick alone: the next tick's delta is the whole clock. A mission start
	// zeroes the clock after it, so no delta runs backwards there.
	// [orig: HUD_ResetAllOverlayBuffers - `mov g_HUDItemFlashLastTick, esi` @0x59DD7D;
	//  HUD_TickItemFlashTimers - `sub edx, ecx` @0x59A9F0, `cmp eax, edx; jle` @0x59AA06;
	//  Game_StartMission - HUD_InitOverlaySystem @0x525733, g_CurrentTick = 0 @0x525B9F]
	HudItemFlash flash;
	flash.tick(1000);
	flash.set(2, 1500);
	flash.set(7, 500);
	flash.reset_last_tick();
	CHECK(flash.timer(2) == 1500);
	CHECK(flash.timer(7) == 500);
	flash.tick(1010);
	CHECK(flash.timer(2) == 490);
	CHECK(flash.timer(7) == 0);
	flash.set(2, 40);
	flash.reset_last_tick();
	flash.tick(0);      // the restarted clock: the same tick changes nothing
	CHECK(flash.timer(2) == 40);
	flash.tick(5);
	CHECK(flash.timer(2) == 35);
}

static void test_the_timers_outlive_a_mission() {
	// The timers are process words no mission start writes: the next mission's HUD takes
	// them as the last one left them, over its own zero last tick, so its first pass at the
	// restarted clock's tick 30 takes the 30 ticks off.
	// [orig: g_HUDItemFlashTimers @0x2723CF8 - the tick's walk @0x59A9F7, the arm's store
	//  @0x5A302A; HUD_InitOverlaySystem @0x5A4620 - the HUD_ResetAllOverlayBuffers call
	//  @0x5A4949]
	HudItemFlash last;
	last.tick(5000);
	last.set(9, 0x400);
	last.tick(5100);
	HudItemFlash next;
	next.carry_timers(last.timers());
	CHECK(next.timer(9) == 0x400 - 100);
	next.tick(30);
	CHECK(next.timer(9) == 0x400 - 130);
	CHECK(next.timer(8) == 0);
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
	test_the_reset_zeroes_the_last_tick_and_keeps_the_timers();
	test_the_timers_outlive_a_mission();
	test_map_gate_draws_idle_or_lit_phase();
	test_level_zero_side_path_keeps_the_stored_level();
	if (failures) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("hud_item_flash: all passed\n");
	return 0;
}
