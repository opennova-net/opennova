// The vehicle-brain state rows against their retail witnesses: what each ground
// and aircraft row reads off the entity record and writes into the brain and slot.
// Driven through the public row table (AiSystem::row) and the class dispatchers.
#include <cstdio>
#include <memory>

#include <formats/aip/aip.h>
#include <runtime/mission/promote.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

// One pool-1 hull with a brain; the registry record carries the live words.
struct Hull {
    std::unique_ptr<World> owned = std::make_unique<World>();
    World &w = *owned;
    EntityHandle handle;
    AiEntity *ai = nullptr;

    explicit Hull(const Entity &seed) {
        w.registry.configure_pool(1, 2);
        handle = w.registry.spawn(1, seed);
        ai = w.ai.at(w.ai.attach(handle));
        ai->brain.f[AiBrain::kOwner] = 1;
    }
    Entity &record() { return *w.registry.get(handle); }
    AiThinkCtx ctx() { return AiThinkCtx{&w.ai, ai, &w, nullptr}; }
};

// The ground rows read the hull's live entity words, never the AiEntity mirrors:
// the health word +0x11E and the velocity pair +0x98/+0x9C.
// [orig: AI_HandleEvent_HelicopterCombatD @0x467747 / @0x467954..0x46795A;
//  AI_UpdatePatrolBehavior @0x457D87 / @0x457E23..0x457E29;
//  AI_EnterState_GroundEvade @0x4674A5 / @0x4674B2..0x4674B8;
//  AI_TransitionToDeath_GroundVehicle @0x467C09..0x467C0F;
//  AI_TickState_VehicleDying @0x467CED..0x467CF3]
void test_ground_rows_read_the_live_hull_words() {
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.health = 0;       // the record says dead ...
    seed.veh.vel_x = 3000; // ... and still moving: |(3000, 4000)| = 5000 >= 1057
    seed.veh.vel_y = 4000;
    Hull hull(seed);
    AiEntity &e = *hull.ai;
    e.health = 100; // the stale mirrors say alive and parked
    e.vel_x = 0;
    e.vel_z = 0;
    hull.w.ai.is_authority = true;
    AiThinkCtx ctx = hull.ctx();

    // Rows 16/19 (follow-waypoint) and 18 (evade) tick the death leg: a crash (3).
    for (int32_t state : {kAiGroundFollowWp, kAiGroundEvade, kAiGroundFormation}) {
        const int before = hull.w.ai.events.count();
        hull.w.ai.row(state).tick(ctx);
        CHECK(hull.w.ai.events.count() == before + 1);
        if (hull.w.ai.events.count() == before + 1) CHECK(hull.w.ai.events.at(before).f[0] == 3);
    }
    // The evade enter's dead leg (no FOLLOW_WP / FLEE flag) queues the same crash.
    {
        e.profile.flags96 = 0;
        const int before = hull.w.ai.events.count();
        hull.w.ai.row(kAiGroundEvade).enter(ctx);
        CHECK(hull.w.ai.events.count() == before + 1);
        if (hull.w.ai.events.count() == before + 1) CHECK(hull.w.ai.events.at(before).f[0] == 3);
    }
    // The dying enter and tick hold a moving hull in state 21: no destroy event
    // while the record's speed is >= 1057, whatever the mirrors say.
    {
        e.net_saved_live_pose[0] = e.pos[0] + 0x10000; // not "still" for the dying tick
        const int before = hull.w.ai.events.count();
        hull.w.ai.row(21).enter(ctx);
        CHECK(hull.w.ai.events.count() == before);
        e.net_saved_live_pose[0] = e.pos[0] + 0x10000;
        hull.w.ai.row(21).tick(ctx);
        CHECK(hull.w.ai.events.count() == before);
    }
    // A live record takes the alive legs even when the mirror reads dead.
    {
        hull.record().health = 100;
        e.health = 0;
        const int before = hull.w.ai.events.count();
        hull.w.ai.row(kAiGroundEvade).tick(ctx);
        CHECK(hull.w.ai.events.count() == before);
    }
}

// Every alert-family enter raises the entity's own AiSlot alert byte (+0x88) to
// red first and unconditionally; the ally wake only adds to it. A lone hull
// with no ally in range is therefore red after combat, evade and death enters.
// [orig: AI_EnterState_GroundCombat @0x467665; AI_EnterState_GroundEvade
//  @0x46741F; AI_TransitionToDeath_Vehicle @0x46696E;
//  Entity_ProcessVehicleDestruction @0x466B4C; AI_TransitionToDeath_GroundVehicle
//  @0x467BD8; AI_TransitionToDestroyed_Vehicle @0x467DF1]
void test_alert_enters_raise_the_own_slot_alert() {
    for (int32_t state : {int32_t(kAiGroundCombat), int32_t(kAiGroundEvade), 13, 15, 21, 23}) {
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.health = 100;
        seed.team = 1;
        Hull hull(seed);
        AiEntity &e = *hull.ai;
        e.team = 1;
        e.profile.flags96 = 4; // evade routes FLEE -> combat: still the alert family
        e.slot.bytes()[AiSlot::kAlertByte] = 0;
        AiThinkCtx ctx = hull.ctx();
        hull.w.ai.row(state).enter(ctx);
        CHECK(e.slot.bytes()[AiSlot::kAlertByte] == 2);
        CHECK(e.brain.f[AiBrain::kAlert] == 2 && e.brain.f[AiBrain::kPrevAlert] == 2);
    }
}

// The evade enter's flee leg (alive, EVADE_FLAGS without FOLLOW_WP and FLEE):
// the controller triple {1, 0x7FFFFFFF, 1}, the working heading turned about
// (entity heading + controller[1]), working pitch/roll zero, the mover output
// at combat speed brain[49], step 16. The working position is not written.
// [orig: AI_EnterState_GroundEvade @0x46756D..0x4675B1]
void test_evade_flee_leg_turns_about_at_combat_speed() {
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.health = 100;
    Hull hull(seed);
    AiEntity &e = *hull.ai;
    e.profile.flags96 = 0;
    e.heading = 0x12345678;
    e.brain.f[AiBrain::kSpeedA] = 4321;
    e.brain.f[AiBrain::kWorkPosX] = 7;
    e.brain.f[AiBrain::kWorkPosY] = 8;
    e.brain.f[AiBrain::kWorkPosZ] = 9;
    e.brain.f[AiBrain::kWorkPitch] = 11;
    e.brain.f[AiBrain::kWorkRoll] = 12;
    AiThinkCtx ctx = hull.ctx();
    hull.w.ai.row(kAiGroundEvade).enter(ctx);
    CHECK(e.patrol_f0 == 1 && e.patrol_delta == 0x7FFFFFFF && e.patrol_goal == 1);
    CHECK(e.brain.f[AiBrain::kWorkHeading] ==
          static_cast<int32_t>(0x12345678u + 0x7FFFFFFFu));
    CHECK(e.brain.f[AiBrain::kWorkPitch] == 0 && e.brain.f[AiBrain::kWorkRoll] == 0);
    CHECK(e.brain.f[AiBrain::kOutSpeed] == 4321);
    CHECK(e.brain.f[AiBrain::kStep] == 16);
    CHECK(e.brain.f[AiBrain::kWorkPosX] == 7 && e.brain.f[AiBrain::kWorkPosY] == 8 &&
          e.brain.f[AiBrain::kWorkPosZ] == 9);
}

// PLAYPARTANIM's rate divides the single-precision flt_7C3B40 (0.016f) by the
// ANIMTIME seconds: ANIMTIME 1 (1/65536 s) is 68719480, not the double's
// 68719476. Rates from the retail instructions executed for each ANIMTIME.
// [orig: Entity_ApplyCommand `fdivr ds:flt_7C3B40` @0x43B1D8, ftol @0x43B1EB]
void test_part_anim_rate_uses_the_single_precision_tick() {
    const struct { int32_t time; int32_t rate; } cases[] = {
        {1, 68719480}, {2, 34359740}, {3, 22906493}, {4, 17179870}, {5, 13743896},
        {8, 8589935}, {10, 6871948}, {20, 3435974}, {65536, 1048}, {131072, 524},
        {0, static_cast<int32_t>(0x80000000)}};
    for (const auto &c : cases) {
        AiBrain b;
        ai_apply_command(b, 0x22, /*channel=*/1, /*play_type=*/1, c.time);
        CHECK(b.f[AiBrain::kPartAnimRate0] == c.rate);
    }
}

// The profile loader's class-walk order against the retail CRT qsort run on
// every {0..3}^4 priority tuple plus wrapping keys: the unstable selection
// shortsort, stored reversed. Only HELO/GROUND profiles load their keys.
// [orig: AIProfile_LoadOrFind `call _qsort` @0x45FECA -> _qsort @0x76D6A0,
//  CompareFunction @0x455D90]
void test_class_walk_matches_the_retail_qsort() {
    static const int32_t cases[][8] = {
#include "fixtures/ai_class_walk_vectors.inc"
    };
    auto owned = std::make_unique<World>();
    for (const auto &v : cases) {
        AiEntity ae;
        opennova::aip::Profile p;
        p.type = 2;
        p.priority_air = v[0];
        p.priority_ground = v[1];
        p.priority_organics = v[2];
        p.priority_decorations = v[3];
        opennova::mission::initialize_ai_profile(ae, p, owned->ai, EntityKind::Item);
        for (int i = 0; i < 4; ++i) CHECK(ae.profile.slot_class[i] == v[4 + i]);
    }
    // Any other profile type sorts four zero keys, whatever it authored.
    AiEntity other;
    opennova::aip::Profile organic;
    organic.type = 3;
    organic.priority_air = 7;
    opennova::mission::initialize_ai_profile(other, organic, owned->ai, EntityKind::Item);
    CHECK(other.profile.slot_class[0] == 0 && other.profile.slot_class[1] == 3 &&
          other.profile.slot_class[2] == 2 && other.profile.slot_class[3] == 1);
}

} // namespace

int main() {
    test_ground_rows_read_the_live_hull_words();
    test_class_walk_matches_the_retail_qsort();
    test_alert_enters_raise_the_own_slot_alert();
    test_evade_flee_leg_turns_about_at_combat_speed();
    test_part_anim_rate_uses_the_single_precision_tick();
    std::printf("ai_brain_rows: %d failures\n", failures);
    return failures ? 1 : 0;
}
