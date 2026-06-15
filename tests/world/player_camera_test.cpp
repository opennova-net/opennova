// Local-player M3/M4 tests: stance derivation from input (stand/crouch/prone) and the
// first-person camera composition (eye = body pos + foot eye-height; view angles = body
// euler). [orig: stance entity+0x12C 0x100/0x200; camera Camera_ComputeThirdPersonView
// @0x437d10 (foot eye height +0x10000) + Player_UpdateFirstPersonCamera @0x4dd380.]
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

constexpr int32_t kEyeHeight = 0x10000;

AiEntity *player(AiSystem &ai) {
    int idx = ai.attach(EntityHandle::make(0, 0));
    AiEntity *e = ai.at(idx);
    e->player.active = true;
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

} // namespace

int main() {
    // ---- stance derivation (M3): stand / crouch / prone ----
    {
        World w; AiSystem ai;
        AiEntity *e = player(ai);

        PlayerInputCommand c;
        c.crouch = true;
        ai.set_player_input(c);
        run_ticks(ai, w, 1, 2);
        CHECK(e->player.stance == 1); // crouch

        c = PlayerInputCommand{};
        c.prone = true;
        ai.set_player_input(c);
        run_ticks(ai, w, 2, 3);
        CHECK(e->player.stance == 2); // prone

        ai.set_player_input(PlayerInputCommand{});
        run_ticks(ai, w, 3, 4);
        CHECK(e->player.stance == 0); // stand
    }

    // ---- FP camera (M4): eye = body pos + eye-height; view = body euler ----
    {
        World w; AiSystem ai;
        AiEntity *e = player(ai);
        e->pos[0] = 0x00111111;
        e->pos[1] = 0x00222222;
        e->pos[2] = 0x00333333;
        e->heading = 0x0A000000;
        e->pitch = 0x01000000;
        e->roll = 0;
        ai.set_player_input(PlayerInputCommand{});
        run_ticks(ai, w, 1, 2);

        const PlayerCamera &cam = e->player.camera;
        CHECK(cam.valid);
        CHECK(cam.eye[0] == 0x00111111);
        CHECK(cam.eye[1] == 0x00222222);
        CHECK(cam.eye[2] == 0x00333333 + kEyeHeight);
        CHECK(cam.view_yaw == 0x0A000000);
        CHECK(cam.view_pitch == 0x01000000);
        CHECK(cam.view_roll == 0);
    }

    // ---- camera tracks a mouse-look yaw change ----
    {
        World w; AiSystem ai;
        AiEntity *e = player(ai);
        PlayerInputCommand c;
        c.look_yaw_delta = 0x02000000;
        ai.set_player_input(c);
        run_ticks(ai, w, 1, 2);
        CHECK(e->heading == 0x02000000);
        CHECK(e->player.camera.view_yaw == 0x02000000);
    }

    if (failures == 0) std::printf("player_camera_test: OK\n");
    return failures ? 1 : 0;
}
