// The SM target feed and its engage bookkeeping against the retail witnesses:
// the class walk's candidate facts [orig: AI_FindBestTargetB @0x466F60] and
// the state-16 engage [orig: AI_HandleEvent_HelicopterCombatD @0x467730].
#include <cstdio>
#include <memory>

#include <runtime/world/ai.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// One SM ground brain (pool 1, team 1) at the origin facing +X, with wide
// arcs, 1000 u engage ranges and a nonzero engage fire delay.
struct Scanner {
    std::unique_ptr<World> w = std::make_unique<World>();
    int index = -1;

    AiEntity &e() { return *w->ai.at(index); }

    Scanner() {
        w->registry.configure_pool(0, 8);
        w->registry.configure_pool(1, 8);
        Entity self{};
        self.alive = true;
        self.health = 100;
        self.team = 1;
        const EntityHandle h = w->registry.spawn(1, self);
        w->ai.is_authority = true;
        index = w->ai.attach(h);
        AiEntity &ai = e();
        ai.team = 1;
        ai.heading = 0;
        ai.profile.fov_primary = 0xFF;
        ai.profile.fov_secondary = 0xFF;
        ai.profile.range_primary = 1000;
        ai.profile.range_secondary = 1000;
        ai.profile.field104 = 31;
    }

    // A live team-2 candidate 20 u ahead, inside both of its own signature caps.
    EntityHandle spawn_enemy(int pool, uint32_t engine_flags, uint32_t owner_connection_id) {
        Entity seed{};
        seed.alive = true;
        seed.health = 100;
        seed.team = 2;
        seed.radar_sig = 1000;
        seed.heat_sig = 1000;
        seed.engine_flags = engine_flags;
        seed.owner_connection_id = owner_connection_id;
        seed.position = Vec3{20.0f, 0.0f, 0.0f};
        return w->registry.spawn(pool, seed);
    }
};

} // namespace

// R2-6: the engage picks its fire-delay jitter by the target's SM brain pointer
// (entity+0x64), not by a network owner. A brained vehicle takes the guarded
// inline-LCG branch A (dword_31BFBB8); a brainless remote player takes the
// PRNG_Next16 branch B, whatever its connection.
// [orig: AI_HandleEvent_HelicopterCombatD `cmp [ebx+64h],ebp; jz loc_4678C2`
//  @0x4677EA..0x4677EF; branch A @0x467803..0x467866; branch B
//  `call PRNG_Next16` @0x4678D9]
static void test_engage_jitter_keys_on_the_target_brain() {
    for (const bool brained : {true, false}) {
        Scanner s;
        World &w = *s.w;
        EntityHandle target;
        if (brained) {
            // A crewless SM vehicle: class 1 walks pool 1's non-helo brains.
            s.e().profile.class_priority[1] = 1;
            target = s.spawn_enemy(1, 0, 0);
            w.ai.attach(target); // an SM brain, profile type 0
        } else {
            // A remote player's body: class 0 ends on pool 0's Player-flagged rows.
            s.e().profile.class_priority[0] = 1;
            target = s.spawn_enemy(0, kEntityFlagPlayer, 3);
        }
        AiTarget found{};
        CHECK(w.ai.acquire_target(w, s.e(), found));
        CHECK(found.handle == target);
        CHECK(found.has_brain == brained);
        const uint32_t stream_a = w.ai.prng_a;
        const uint32_t stream_b = w.prng16_state;
        w.ai.engage_target(w, s.e(), found);
        CHECK((w.ai.prng_a != stream_a) == brained);
        CHECK((w.prng16_state != stream_b) == !brained);
        CHECK(s.e().brain.f[AiBrain::kFireDelay] >= 31);
    }
}

int main() {
    test_engage_jitter_keys_on_the_target_brain();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ai_targeting: all passed\n");
    return 0;
}
