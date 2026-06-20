// The host's own-player keystone (net-re §5.2b/§5.38): spawn_player builds a pool-0
// player-infantry entity, apply_player_move_order ports the 8-way input mapping
// [orig: Player_PackInputStateToEntity @0x4df450], and the infantry motor's local-player
// branch drives + mirrors the pose to the registry Entity (the S2C 0x0A source).
#include "world/ai.h"
#include "world/geom.h"
#include "world/infantry.h"
#include "world/player_input.h"
#include "world/player_spawn.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Synthetic clip: any state advances with a fixed forward (entity-local +X) root delta.
struct ForwardClip : IRootMotionSource {
    int32_t fwd;
    explicit ForwardClip(int32_t f) : fwd(f) {}
    bool has_clip(int) const override { return true; }
    bool advance(int, int32_t &phase, RootMotionFrame &out) override {
        phase += 1;
        out.dx = fwd;
        out.dy = 0;
        out.dz = 0;
        out.events = 0;
        return true;
    }
};

int main() {
    // --- §5.2b spawn: a gated pool-0 player-infantry, motor mounted, local_player published.
    {
        World w;
        AiSystem ai;
        w.ai = &ai;
        w.registry.configure_pool(0, 8);

        PlayerSpawn ps;
        ps.position = {10.0f, 20.0f, 5.0f};
        ps.yaw = 0;
        ps.team = 1;
        ps.net_id = 0xFFF0;
        const EntityHandle h = spawn_player(w, ps);

        CHECK(h.valid());
        CHECK(h.pool() == 0);
        CHECK(w.cached.local_player == h);

        const Entity *e = w.registry.get(h);
        CHECK(e != nullptr);
        CHECK(e->item_id == kPlayerInfantryTypeId);
        CHECK((e->flags & 2u) == 0u); // §5.2b gate cleared
        CHECK(e->health > 0);
        CHECK(e->kind == EntityKind::Organic);

        CHECK(ai.count() == 1);
        const AiEntity *ae = ai.at(0);
        CHECK(ae->inf.active);
        CHECK(ae->inf.is_local_player);
    }

    // --- input → 8-way move order (the witnessed mapping + opposing-key cancel).
    {
        World w;
        AiSystem ai;
        w.ai = &ai;
        w.registry.configure_pool(0, 8);
        spawn_player(w, PlayerSpawn{});
        AiEntity &ae = *ai.at(0);

        PlayerInput in;
        in.forward = true;
        in.look_heading = 0x10000000;
        apply_player_move_order(ae, in);
        CHECK(ae.inf.move_mode != 0);
        CHECK(ae.inf.target_dist > 0);
        CHECK(ae.inf.move_offset == 0);             // index 0 = forward
        CHECK(ae.inf.target_heading == 0x10000000); // look → facing

        in = PlayerInput{};
        in.left = true;
        apply_player_move_order(ae, in);
        CHECK(ae.inf.move_offset == 0x40000000);    // index 2 = +90°

        in = PlayerInput{};
        in.forward = true;
        in.right = true;
        apply_player_move_order(ae, in);
        CHECK(ae.inf.move_offset == static_cast<int32_t>(7u * 0x20000000u)); // index 7

        in = PlayerInput{}; // opposing keys cancel → idle
        in.forward = true;
        in.back = true;
        apply_player_move_order(ae, in);
        CHECK(ae.inf.move_mode == 0);
        CHECK(ae.inf.target_dist == 0);
    }

    // --- motor drive: forward input advances pos along facing AND mirrors to the Entity.
    {
        World w;
        AiSystem ai;
        w.ai = &ai;
        ForwardClip clip(0x8000); // 0.5 u forward per tick
        ai.root_motion = &clip;
        w.registry.configure_pool(0, 8);

        PlayerSpawn ps;
        ps.position = {0.0f, 0.0f, 0.0f};
        ps.yaw = 0;
        const EntityHandle h = spawn_player(w, ps);
        AiEntity &ae = *ai.at(0);

        PlayerInput in;
        in.forward = true;
        in.look_heading = ae.heading; // walk along the spawn facing

        const int32_t x0 = ae.pos[0], y0 = ae.pos[1];
        for (int i = 0; i < 30; ++i) {
            apply_player_move_order(ae, in); // re-assert each frame (as the controller would)
            TickContext ctx;
            ctx.world = &w;
            ctx.logic_tick = static_cast<uint32_t>(i);
            ctx.is_authority = true;
            ai.tick(w, ctx);
        }

        CHECK(ae.pos[0] != x0 || ae.pos[1] != y0); // moved
        const Entity *e = w.registry.get(h);
        CHECK(e != nullptr);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[0])) - e->position.x) < 0.01f);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[1])) - e->position.y) < 0.01f);
        CHECK(std::fabs(static_cast<float>(from_fixed(ae.pos[2])) - e->position.z) < 0.01f);
    }

    std::printf("player_spawn: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
