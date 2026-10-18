// The HUD's frame counters (world::HudFrameCounters): one step per frame tick, the main-frame
// counter always, the session counter unless the menu pause, the HUD counter unless the pause or
// the epilog screen; World steps them at the head of every frame tick.
// [orig: Game_TickHudFrameCounters @0x434C00 (@0x434C05, @0x434C0B..0x434C14, @0x434C1A..0x434C23),
//  from Game_ProcessMainFrame @0x5265D5]
#include <runtime/world/world.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void test_step_gates() {
    HudFrameCounters c;
    c.step(false, false);
    check(c.main_frame == 1 && c.session_frame == 1 && c.hud_frame == 1, "a frame steps all three");
    c.step(false, true);
    check(c.main_frame == 2 && c.session_frame == 2 && c.hud_frame == 1,
            "the epilog screen holds the HUD counter alone");
    c.step(true, false);
    check(c.main_frame == 3 && c.session_frame == 2 && c.hud_frame == 1,
            "the menu pause holds the session and HUD counters");
    c.step(true, true);
    check(c.main_frame == 4 && c.session_frame == 2 && c.hud_frame == 1,
            "the pause with the epilog screen holds both");
}

void test_world_steps_once_per_tick() {
    World world;
    for (int i = 0; i < 5; ++i) world.run_logic_tick();
    check(world.hud_frames.main_frame == 5 && world.hud_frames.session_frame == 5 &&
                    world.hud_frames.hud_frame == 5,
            "five frame ticks step each counter five times");
    world.epilog.screen_active = true;
    for (int i = 0; i < 3; ++i) world.run_logic_tick();
    check(world.hud_frames.main_frame == 8 && world.hud_frames.session_frame == 8 &&
                    world.hud_frames.hud_frame == 5,
            "on the epilog screen the HUD counter holds, the others step");
}

} // namespace

int main() {
    test_step_gates();
    test_world_steps_once_per_tick();
    if (failures == 0) std::printf("hud_frame_counters: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
