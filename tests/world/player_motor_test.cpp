// Local-player motor — M2 tests: root-motion-driven locomotion (each move-direction advances
// the entity correctly in world space, at heading 0 and rotated), and the player gravity
// (-208/tick x1, terminal -32768) + ground settle (no +0x50000 mover offset, D-PLR-2) + fall
// damage. [orig: Entity_UpdateInfantryPhysics @0x4b40e0; gravity @0x4b7ac8/0x4b7c77/0x4b7ce0;
// shared ground resolver @0x4b2bd0.] Locomotion uses a synthetic root-motion source; gravity
// uses a synthetic flat height field — same wiring as tests/world/infantry_test.cpp.
#include <cstdint>
#include <cstdio>

#include "terrain/height_field.h"
#include "world/ai.h"
#include "world/world.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }
constexpr int32_t kTerminal = -32768;

// A 512x512 flat-per-column height field (same wiring as infantry_test::Field).
struct Field {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;
    template <typename Fn>
    explicit Field(Fn raw16_of_x) : heightmap(kDim * kDim), sector_grid(256, 1) {
        for (int z = 0; z < kDim; ++z)
            for (int x = 0; x < kDim; ++x)
                heightmap[z * kDim + x] = raw16_of_x(x);
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// Synthetic clip source: the forward locomotion gait moves `step` forward (dx); idle has no
// root motion. (The M2 motor rotates the forward clip by the move-direction offset.)
struct TestSource : IRootMotionSource {
    int32_t step = 0x4000;
    int32_t capsule_bottom = 0;  // origin->feet foot offset emitted each frame (settle floor)
    bool has_clip(int id) const override {
        return id == anim_state::kWalkForward || id == anim_state::kIdle;
    }
    bool advance(int id, int32_t &phase, RootMotionFrame &out) override {
        ++phase;
        out = RootMotionFrame{};
        if (id == anim_state::kWalkForward) out.dx = step;
        out.capsule_bottom = capsule_bottom;
        return has_clip(id);
    }
};

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

PlayerInputCommand move(int dir) {
    PlayerInputCommand c;
    c.is_moving = true;
    c.move_dir = dir;
    return c;
}

} // namespace

int main() {
    TestSource src;

    // ---- forward at heading 0 advances +X by the clip's forward step (terrain-free: no gravity) ----
    {
        World w; AiSystem ai; ai.root_motion = &src;
        AiEntity *e = player(ai);
        e->heading = 0;
        ai.set_player_input(move(kPlayerMoveForward));
        run_ticks(ai, w, 1, 2);
        CHECK(e->pos[0] == src.step);
        CHECK(e->pos[1] == 0);
        CHECK(e->pos[2] == 0);
    }

    // ---- strafe-left moves +90deg (perpendicular) to facing ----
    {
        World w; AiSystem ai; ai.root_motion = &src;
        AiEntity *e = player(ai);
        e->heading = 0;
        ai.set_player_input(move(kPlayerMoveStrafeLeft));
        run_ticks(ai, w, 1, 2);
        CHECK(e->pos[0] == 0);
        CHECK(e->pos[1] == src.step);
    }

    // ---- back moves opposite forward ----
    {
        World w; AiSystem ai; ai.root_motion = &src;
        AiEntity *e = player(ai);
        e->heading = 0;
        ai.set_player_input(move(kPlayerMoveBack));
        run_ticks(ai, w, 1, 2);
        CHECK(e->pos[0] == -src.step);
        CHECK(e->pos[1] == 0);
    }

    // ---- forward, rotated heading 90deg, moves +Y ----
    {
        World w; AiSystem ai; ai.root_motion = &src;
        AiEntity *e = player(ai);
        e->heading = 0x40000000; // 90 deg
        ai.set_player_input(move(kPlayerMoveForward));
        run_ticks(ai, w, 1, 2);
        CHECK(e->pos[0] == 0);
        CHECK(e->pos[1] == src.step);
    }

    // ---- idle: no movement ----
    {
        World w; AiSystem ai; ai.root_motion = &src;
        AiEntity *e = player(ai);
        ai.set_player_input(PlayerInputCommand{});
        run_ticks(ai, w, 1, 6);
        CHECK(e->pos[0] == 0 && e->pos[1] == 0);
    }

    // ---- gravity: falls to ground + capsule_bottom, reaches terminal, fall damage ----
    // [orig: gravity @0x4b7ac8; settle + fall damage @0x4b2bd0 / 0x4b7cf9.] The settle floors pos[2]
    // to ground + the clip's capsule_bottom (origin->feet); this source emits capsule_bottom = 0, so
    // floor = ground here. (D-PLR-2 / D-INF-6.)
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); }); // 50u everywhere
        const int32_t floor_z = fx(50);
        World w; AiSystem ai;
        ai.terrain = &flat.field;
        ai.fall_damage_scale = 1;
        AiEntity *e = player(ai);
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(200); // 150u above the floor -> reaches terminal velocity
        e->health = 30000;
        ai.set_player_input(PlayerInputCommand{});

        int32_t min_vel = 0;
        TickContext ctx;
        ctx.world = &w;
        ctx.is_authority = true;
        for (uint32_t t = 0; t < 1200; ++t) {
            ctx.logic_tick = t;
            ai.tick(w, ctx);
            if (e->player.vel[2] < min_vel) min_vel = e->player.vel[2];
        }
        CHECK(min_vel == kTerminal);
        CHECK(e->pos[2] == floor_z);
        CHECK(e->player.vel[2] == 0);
        CHECK(e->health == 30000 - ((-1057 - kTerminal) >> 4)); // 30000 - 1981
    }

    // ---- a short hop lands without fall damage ----
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        const int32_t floor_z = fx(50);
        World w; AiSystem ai;
        ai.terrain = &flat.field;
        ai.fall_damage_scale = 1;
        AiEntity *e = player(ai);
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = floor_z + 1500; // small drop: vel stays > -1057
        e->health = 100;
        ai.set_player_input(PlayerInputCommand{});
        run_ticks(ai, w, 0, 20);
        CHECK(e->pos[2] == floor_z);
        CHECK(e->health == 100);
    }

    // ---- settle floors pos[2] to ground + capsule_bottom; the FP eye is +1.0u above that origin ----
    // [orig: settle @0x4b2bd0 with entityRadius = the .bad capsule_bottom*65536; eye lift @0x437e8f
    //  (+0x10000). pos[2] is the model origin (waist), so eye = origin + 1.0u lands at head height.]
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        TestSource csrc;
        csrc.capsule_bottom = fx(1); // 1.0u origin->feet, emitted every frame (incl. idle)
        World w; AiSystem ai;
        ai.terrain = &flat.field;
        ai.fall_damage_scale = 0;
        ai.root_motion = &csrc;
        AiEntity *e = player(ai);
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(200); // falls and settles onto the capsule bottom
        ai.set_player_input(PlayerInputCommand{});
        run_ticks(ai, w, 0, 1200);
        CHECK(e->pos[2] == fx(50) + fx(1));                    // ground + capsule_bottom (feet on terrain)
        CHECK(e->player.camera.eye[2] == e->pos[2] + 0x10000); // FP eye = origin + kEyeHeight (1.0u)
    }

    if (failures == 0) std::printf("player_motor_test: OK\n");
    return failures ? 1 : 0;
}
