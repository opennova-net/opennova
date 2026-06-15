// Local-player controller — M1 tests: the org2 dispatch + the input model + the look
// application (yaw applied directly with no turn-rate clamp; view pitch clamped to +/-80
// deg, +/-40 deg when crouched). [orig: Entity_UpdateInfantryPhysics @0x4b40e0 look path;
// the scaled-delta apply + clamp in Input_HandleActionBinding_0 @0x4e0420 (@0x4e0d44 /
// 0x4e0ffe).] Locomotion/gravity/ground (M2), stance (M3), camera (M4), recoil (M5) are
// tested in their own milestones. See docs/world/player-controller-re.md.
#include <cstdint>
#include <cstdio>

#include "world/ai.h"
#include "world/world.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// View-pitch clamp immediates (mirrors of the cited values in player.cpp).
constexpr int32_t kPitchClampStand = 0x38E38E00;  // +/-80 deg (BAM)
constexpr int32_t kPitchClampCrouch = 0x1C71C700; // +/-40 deg (BAM)

// Attach an org2 local-player entity and register it as the local player.
AiEntity *make_player(AiSystem &ai) {
    int idx = ai.attach(EntityHandle::make(0, 0));
    AiEntity *e = ai.at(idx);
    e->player.active = true;
    e->heading = 0;
    e->pitch = 0;
    ai.set_local_player(idx);
    return e;
}

void run_ticks(AiSystem &ai, World &w, uint32_t from, uint32_t to_excl) {
    TickContext ctx;
    ctx.world = &w;
    ctx.is_authority = true;
    for (uint32_t t = from; t < to_excl; ++t) {
        ctx.logic_tick = t;
        ai.tick(w, ctx);
    }
}

PlayerInputCommand idle() { return PlayerInputCommand{}; }

} // namespace

int main() {
    // ---- dispatch + designation ----
    {
        World w; AiSystem ai;
        AiEntity *e = make_player(ai);
        CHECK(ai.local_player_index() == 0);
        CHECK(e->player.active);
        CHECK(!e->inf.active); // routed to tick_player, NOT the infantry motor
    }

    // ---- look yaw: scaled delta applied directly, no per-tick turn-rate clamp ----
    {
        World w; AiSystem ai;
        AiEntity *e = make_player(ai);
        PlayerInputCommand cmd = idle();
        cmd.look_yaw_delta = 0x01000000;
        ai.set_player_input(cmd);
        run_ticks(ai, w, 1, 2);
        CHECK(e->heading == 0x01000000);
        // a fresh idle command (the host pushes one per tick) stops the rotation
        ai.set_player_input(idle());
        run_ticks(ai, w, 2, 3);
        CHECK(e->heading == 0x01000000);
        // a large yaw delta is NOT clamped (unlike the AI body-turn)
        cmd.look_yaw_delta = 0x40000000;
        ai.set_player_input(cmd);
        run_ticks(ai, w, 3, 4);
        CHECK(e->heading == 0x41000000);
    }

    // ---- idle input: no motion ----
    {
        World w; AiSystem ai;
        AiEntity *e = make_player(ai);
        e->heading = 0x12345678;
        e->pitch = 0x00100000;
        ai.set_player_input(idle());
        run_ticks(ai, w, 1, 5);
        CHECK(e->heading == 0x12345678);
        CHECK(e->pitch == 0x00100000);
    }

    // ---- look pitch: clamped to +/-80 deg standing ----
    {
        World w; AiSystem ai;
        AiEntity *e = make_player(ai);
        PlayerInputCommand cmd = idle();
        cmd.look_pitch_delta = 0x40000000; // +90 deg, beyond the +80 clamp
        ai.set_player_input(cmd);
        run_ticks(ai, w, 1, 2);
        CHECK(e->pitch == kPitchClampStand);

        cmd.look_pitch_delta = -0x7F000000; // far below the -80 clamp
        ai.set_player_input(cmd);
        run_ticks(ai, w, 2, 3);
        CHECK(e->pitch == -kPitchClampStand);
    }

    // ---- crouch reduces the pitch clamp to +/-40 deg ----
    {
        World w; AiSystem ai;
        AiEntity *e = make_player(ai);
        PlayerInputCommand cmd = idle();
        cmd.crouch = true;
        cmd.look_pitch_delta = 0x40000000; // beyond +40 deg
        ai.set_player_input(cmd);
        run_ticks(ai, w, 1, 2);
        CHECK(e->pitch == kPitchClampCrouch);
    }

    if (failures == 0) std::printf("player_input_test: OK\n");
    return failures ? 1 : 0;
}
