// The vehicle brain's combat ticks, driven through their dispatch rows on a
// two-entity world. The state-17 GROUND_COMBAT tick [orig:
// AIEntity_ProcessWeaponFire @0x472E00] leg by leg: the processed fire checks
// LOS while the between-tick continuation defers it, a failed rescan drops the
// target, WEAPON_TURRET blocks stage between processed ticks, the no-ammo
// latch forces the processed leg, the primary continuation solves from a level
// pitch, the stationary RC_FIRE leg, the ATEAM sweep, the ATEAM_LOCK replay
// and the chase.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// A 2 u ridge across field columns 8..12: it blocks every ground-level ray
// between the shooter (x = 2) and the target (x = 20).
struct RidgeField {
    enum { kDim = 512 };
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    opennova::terrain::TerrainHeightField field;
    RidgeField() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
        for (int z = 0; z < kDim; ++z)
            for (int x = 8; x <= 12; ++x)
                heightmap[z * kDim + x] = static_cast<uint16_t>(2.0 * 256.0);
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

const opennova::terrain::TerrainHeightField *ridge() {
    static const RidgeField ridge_field;
    return &ridge_field.field;
}

// One SM ground vehicle (pool 1) on a level hull facing a live target 18 u
// ahead on +X. The primary block is armed (5 rounds, a one-tick interval, the
// full cone) and the scatter modulus is 1, so every shot leaves along its
// solved direction. The brain holds the target through Entity_SetAITarget,
// so the refcount and AiSlot[3] are live. No terrain is wired unless a case
// adds the ridge, so every ray is clear by default.
struct Rig {
    World w;
    int index = -1;
    EntityHandle target;

    AiEntity &e() { return *w.ai.at(index); }
    AiBrain &brain() { return e().brain; }
    Entity &target_entity() { return *w.registry.get(target); }
    int rounds() const { return static_cast<int>(w.out.rounds.count); }
    const RoundEvent &round(int i) const { return w.out.rounds.records[i]; }
    // The next visit reaches the 16-step accumulator: a processed tick.
    void next_processed() {
        brain().f[AiBrain::kStep] = 16;
        brain().f[AiBrain::kTickAccum] = 0;
    }
    // The next visit stays below it: a between-tick visit (state 17 steps 1).
    void next_between() {
        brain().f[AiBrain::kStep] = 1;
        brain().f[AiBrain::kTickAccum] = 0;
    }
    void tick() {
        AiThinkCtx ctx{&w.ai, &e(), &w, nullptr};
        w.ai.row(kAiGroundCombat).tick(ctx);
    }
};

std::unique_ptr<Rig> make_rig() {
    auto rig = std::make_unique<Rig>();
    World &w = rig->w;
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 8);
    w.tables.ammo.entries.resize(2);
    w.tables.ammo.entries[1].valid = true;
    w.tables.ammo.entries[1].velocity = 620;
    w.tables.ammo.entries[1].max_age_ticks = 100;

    Entity shooter{};
    shooter.alive = true;
    shooter.health = 100;
    shooter.team = 1;
    shooter.position = Vec3{2.0f, 100.0f, 0.0f};
    const EntityHandle shooter_h = w.registry.spawn(1, shooter);
    Entity target{};
    target.alive = true;
    target.health = 100;
    target.team = 2;
    target.position = Vec3{20.0f, 100.0f, 0.0f};
    rig->target = w.registry.spawn(0, target);

    w.ai.is_authority = true;
    rig->index = w.ai.attach(shooter_h);
    AiEntity &e = rig->e();
    e.team = 1;
    e.health = 100;
    e.pos[0] = 2 << 16;
    e.pos[1] = 100 << 16;
    e.pos[2] = 0;
    e.heading = 0;
    AiBrain &b = e.brain;
    b.f[AiBrain::kCurState] = kAiGroundCombat;
    b.f[AiBrain::kPendState] = kAiGroundCombat;
    b.f[AiBrain::kFallback] = kAiGroundFormation;
    b.f[AiBrain::kAmmoA] = 5;
    b.f[AiBrain::kAccuracy] = 5; // scatter modulus 6 - 5 = 1: every draw scales to 0
    b.f[AiBrain::kSpeedA] = 10 << 16;
    AiProfile &p = e.profile;
    p.flags96 = 2;
    p.fov_secondary = 0xFF; // the widest heading gate
    p.fire_interval_a = 1;
    p.fire_a.ammo_index = 1;
    p.fire_a.cone_bam = 0x7FFFFFFF;
    p.approach_cap = 1000 << 16;
    p.radar_fov_bam = 0x7FFFFFFF; // the radar arm validates any bearing
    w.ai.ai_set_target(w, e, rig->target);
    rig->next_processed();
    return rig;
}

} // namespace

// R2-1: the processed-tick fire validates the target WITH its line of sight
// (arg7 = 0) and the between-tick continuation of the last weapon defers it
// (arg7 = 1): behind a ridge the processed shot holds and records no volley,
// while a running volley keeps firing through the same ridge.
// [orig: AIEntity_ProcessWeaponFire processed `push 0` @0x473D3C (solve
//  @0x473D57), continuation `push 1` @0x473850 (solve @0x47386B);
//  Entity_ComputeWeaponFireTransform_0 ctx 0x6F / 0x806F @0x456BB6..0x456BD5]
static void test_processed_fire_checks_los_and_the_continuation_defers_it() {
    {
        auto rig = make_rig();
        rig->w.ai.terrain = ridge();
        rig->tick(); // processed, behind the ridge
        CHECK(rig->rounds() == 0);
        CHECK(rig->brain().f[AiBrain::kLastWeapon] == 0);
        CHECK(rig->brain().f[AiBrain::kAmmoA] == 5);
        rig->w.ai.terrain = nullptr; // the same tick with a clear ray fires
        rig->next_processed();
        rig->tick();
        CHECK(rig->rounds() == 1);
        CHECK(rig->brain().f[AiBrain::kLastWeapon] == 1);
    }
    {
        auto rig = make_rig();
        rig->w.ai.terrain = ridge();
        rig->brain().f[AiBrain::kLastWeapon] = 1; // a volley on record
        rig->next_between();
        rig->tick();
        CHECK(rig->rounds() == 1);
        CHECK(rig->brain().f[AiBrain::kAmmoA] == 4);
        CHECK(rig->brain().f[AiBrain::kLastWeapon] == 1);
    }
}

// R2-2: every rescan feeds its result to Entity_SetAITarget, so a rescan past
// the 248-tick cadence that finds nothing clears brain[38] and AiSlot[3] and
// releases the old target's refcount. The tick still bears on the old
// pointer, but the solver reads brain[38] itself and has nothing to fire at.
// [orig: AIEntity_TryAcquireTarget @0x4716B0, `call Entity_SetAITarget`
//  @0x4716F0 unconditional; caller `jz` @0x473A41..0x473A45; solver
//  `mov edi,[esi+98h]` @0x4569A4, head gate @0x4569B2]
static void test_failed_rescan_drops_the_target() {
    auto rig = make_rig();
    // The scan walks pool 0 (class 2), but the target has moved beyond both
    // authored ranges: 18 u against 10 u.
    AiProfile &p = rig->e().profile;
    p.class_priority[2] = 1;
    p.range_primary = 10;
    p.range_secondary = 10;
    rig->target_entity().radar_sig = 1000;
    rig->target_entity().heat_sig = 1000;
    AiBrain &b = rig->brain();
    b.f[AiBrain::kRetargetTimer] = 240; // + 16 this tick = 256 > 248
    b.f[AiBrain::kWorkHeading] = 12345;
    rig->e().heading = 0x10000000; // the hull is 22.5 deg off the bearing (0)
    CHECK(rig->target_entity().ai_target_refcount == 1);
    const int calls = rig->w.ai.find_target_calls;
    rig->tick();
    CHECK(rig->w.ai.find_target_calls == calls + 1);
    CHECK(b.f[AiBrain::kRetargetTimer] == 0);
    CHECK(b.f[AiBrain::kTargetSlot] == 0);
    CHECK(rig->e().slot.f[3] == 0);
    CHECK(rig->target_entity().ai_target_refcount == 0);
    CHECK(b.f[AiBrain::kWorkHeading] == 0); // the chase still bears on the old target
    CHECK(rig->rounds() == 0);
}

// R2-3: with no volley on record a WEAPON_TURRET block re-solves on every
// between-tick visit (LOS deferred, no fire), so a SLOW turret slews one step
// per visit, not one per processed tick; and a processed tick on which
// neither weapon fires clears brain[106], so the next visit stages instead of
// continuing a stale volley.
// [orig: `test byte [ebp+88h],1` @0x47332D -> solve @0x4733A4, `mov
//  [esi+310h],bl` @0x473402; both-fail `mov [esi+1A8h],0` @0x4741CC;
//  Entity_ComputeWeaponFireTransform_0 SLOW slew +-0x2108421 @0x456EC3]
static void test_turret_stages_between_processed_ticks() {
    constexpr int32_t kSlowStep = 0x2108421;
    {
        auto rig = make_rig();
        rig->e().heading = -0x20000000;             // the target sits 45 deg off the hull
        rig->e().profile.fire_a.flags = 0x1 | 0x2;  // WEAPON_TURRET | WEAPON_SLOW
        rig->next_between();
        rig->tick();
        CHECK(rig->brain().f[AiBrain::kActiveYaw] == -kSlowStep);
        rig->next_between();
        rig->tick();
        CHECK(rig->brain().f[AiBrain::kActiveYaw] == -2 * kSlowStep);
        CHECK(rig->rounds() == 0);
        CHECK(rig->brain().bytes()[AiBrain::kBoneFlagByte] == 0);
    }
    {
        auto rig = make_rig();
        rig->e().heading = -0x20000000;
        rig->e().profile.fire_a.flags = 0x1 | 0x2;
        rig->brain().f[AiBrain::kLastWeapon] = 1; // a stale volley
        rig->tick(); // processed: the solve slews and fails, the secondary is dry
        CHECK(rig->rounds() == 0);
        CHECK(rig->brain().f[AiBrain::kLastWeapon] == 0);
        CHECK(rig->brain().f[AiBrain::kActiveYaw] == -kSlowStep);
        rig->next_between();
        rig->tick(); // stages: one more step, no fire
        CHECK(rig->brain().f[AiBrain::kActiveYaw] == -2 * kSlowStep);
        CHECK(rig->rounds() == 0);
    }
}

// R2-3: brain[48], the dispatcher's no-ammo latch, sends every visit down the
// processed leg, where the chase becomes the half-turn flee heading; the
// common tail then clamps the speed into [0, speedA] and zeroes the work X/Y.
// [orig: `cmp [esi+0C0h],ebx; jnz loc_473A2C` @0x473309..0x47330F; `cmp
//  dword ptr [esi+0C0h],0; jnz loc_473C08` @0x473AB1..0x473AB8; `lea
//  eax,[ebx+7FFFFF80h]` @0x473C08; tail @0x473C14..0x473C50]
static void test_no_ammo_latch_forces_the_processed_leg() {
    auto rig = make_rig();
    AiBrain &b = rig->brain();
    b.f[AiBrain::kNoTargetIdle] = 1;
    b.f[AiBrain::kAmmoA] = 0;
    b.f[AiBrain::kOutSpeed] = -5;
    b.f[AiBrain::kWorkPosX] = 77;
    b.f[AiBrain::kWorkPosY] = 77;
    rig->next_between();
    rig->tick();
    CHECK(b.f[AiBrain::kWorkHeading] == 0x7FFFFF80); // bearing 0 plus the half turn
    CHECK(b.f[AiBrain::kOutSpeed] == 0);
    CHECK(b.f[AiBrain::kWorkPosX] == 0 && b.f[AiBrain::kWorkPosY] == 0);
    CHECK(rig->rounds() == 0);
}

// R2-3: the primary continuation zeroes the pre-seeded pitch before its solve,
// so a pitched hull validates the target in a level frame and composes that
// solution onto its real pitch: the shot leaves exactly one hull pitch above
// a level hull's. The processed tick solves in the pitched frame and still
// dips toward the target, whose origin sits 2 u below the muzzle 18 u away.
// [orig: pre-seed @0x4737F7..0x47381D, `mov [esp+40h+var_8],ebx` @0x473821
//  (ebx = 0), solve @0x47386B]
static void test_primary_continuation_solves_from_a_level_pitch() {
    constexpr int32_t kHullPitch = 0x08000000; // 11.25 deg, nose up
    const auto shot_pitch = [](int32_t hull_pitch, bool continuation) {
        auto rig = make_rig();
        rig->e().pitch = hull_pitch;
        if (continuation) {
            rig->brain().f[AiBrain::kLastWeapon] = 1;
            rig->next_between();
        }
        rig->tick();
        CHECK(rig->rounds() == 1);
        return rig->rounds() == 1 ? rig->round(0).dir_pitch : 0;
    };
    const int32_t level = shot_pitch(0, false);
    const int32_t processed = shot_pitch(kHullPitch, false);
    const int32_t continued = shot_pitch(kHullPitch, true);
    CHECK(level < 0);
    CHECK(processed < 0);
    CHECK(std::abs(processed - level) < 0x01000000); // within 1.4 deg of the level solution
    CHECK(std::abs(continued - (level + kHullPitch)) < (1 << 16));
}

// R2-4 (R5-13): the stationary RC_FIRE leg. Weapons free rests the give-up
// timer and fires at brain[38] with LOS checked; the no-ammo latch holds the
// guns; weapons held give up to the fallback at 620 itself; a processed visit
// drops the target and moves through AI_UpdateMovementTarget, whose tail
// writes the profile+220 climb into brain[138] (the waypoint mover never
// touches brain[138]).
// [orig: guard byte @0x472EDE, `mov [esi+0A0h],ebx` @0x472EF2, brain+0xC0
//  @0x472EEB; `cmp dword ptr [esi+0A0h],26Ch; jl` @0x47309E..0x4730AF;
//  Entity_SetAITarget(0) @0x4730C5; `call AI_UpdateMovementTarget` @0x4730D9]
static void test_rc_fire_stationary_leg() {
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x80; // RC_FIRE
        AiBrain &b = rig->brain();
        b.bytes()[AiBrain::kGuardFireByte] = 1; // weapons free
        b.f[AiBrain::kCombatTimer] = 700;
        rig->tick(); // processed
        CHECK(rig->rounds() == 1);
        CHECK(b.f[AiBrain::kCombatTimer] == 0);
        CHECK(b.f[AiBrain::kPendState] == kAiGroundCombat);
        CHECK(b.f[AiBrain::kLastWeapon] == 1);
        CHECK((b.bytes()[AiBrain::kBoneFlagByte] & 0x40) != 0);
        CHECK(b.f[AiBrain::kTargetSlot] == 0);
    }
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x80;
        rig->brain().bytes()[AiBrain::kGuardFireByte] = 1;
        rig->brain().f[AiBrain::kNoTargetIdle] = 1;
        rig->tick();
        CHECK(rig->rounds() == 0);
    }
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x80; // weapons held
        AiBrain &b = rig->brain();
        b.f[AiBrain::kCombatTimer] = 620;
        rig->next_between();
        rig->tick();
        CHECK(rig->rounds() == 0);
        CHECK(b.f[AiBrain::kPendState] == kAiGroundFormation);
    }
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x80 | 0x1; // RC_FIRE | FOLLOW_WP
        rig->e().profile.field220 = 0x1234;
        rig->w.ai.nav.nodes.resize(1);
        rig->w.ai.nav.nodes[0] = NavEntry{{0, 100, 0, 0, 0}};
        AiBrain &b = rig->brain();
        b.f[AiBrain::kWpType] = 3; // a literal coordinate toward (100, 0)
        b.f[AiBrain::kWpCoordX] = 100;
        b.f[AiBrain::kWpResolved] = 0;
        rig->tick();
        CHECK(b.f[138] == 0x1234);
    }
}

// R2-4: the ATEAM sweep steps 0x2AA6 per SHOT, not per processed tick, and
// past +3.0 u wraps to -3.0 u dropping brain[38] by a direct write, so
// AiSlot[3] and the refcount keep the old target.
// [orig: `add dword ptr [esi+2D0h],2AA6h` .. `mov dword ptr [esi+98h],0`
//  continuation @0x473737 / @0x4739DA, processed @0x474093 / @0x474175]
static void test_ateam_sweep_steps_per_shot() {
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x20; // ATEAM
        AiBrain &b = rig->brain();
        b.f[AiBrain::kSweepPhase] = 0x30000 - 0x2AA6 + 1; // one step from the wrap
        rig->tick(); // processed: fires, then wraps
        CHECK(rig->rounds() == 1);
        CHECK(b.f[AiBrain::kSweepPhase] == static_cast<int32_t>(0xFFFD0000u));
        CHECK(b.f[AiBrain::kTargetSlot] == 0);
        CHECK(rig->e().slot.f[3] == static_cast<int32_t>(rig->target.packed) + 1);
        CHECK(rig->target_entity().ai_target_refcount == 1);
    }
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x20;
        rig->e().profile.fire_interval_a = 1000; // the cooldown is not reached: no shot
        rig->brain().f[AiBrain::kSweepPhase] = 0;
        rig->tick();
        CHECK(rig->rounds() == 0);
        CHECK(rig->brain().f[AiBrain::kSweepPhase] == 0);
    }
}

// R2-4: under ATEAM_LOCK an armed burst window refires the last weapon at the
// hull pose plus its saved deltas, all six components: no solve (no target
// needed) and no scatter; the primary marks the bone byte and clears its
// cooldown word. The no-ammo latch holds the replay and clears the bone byte.
// [orig: @0x4730E9..0x4732D9; primary @0x473208..0x4732AA; latch @0x4730FE]
static void test_ateam_lock_replays_the_saved_pose() {
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x40; // ATEAM_LOCK
        rig->w.ai.ai_set_target(rig->w, rig->e(), EntityHandle{}); // no target at all
        AiBrain &b = rig->brain();
        b.f[AiBrain::kBurstWindow] = 1;
        b.f[AiBrain::kLastWeapon] = 1;
        const int32_t delta[6] = {0, 0, 0x20000, 0x01000000, 0x00800000, 0};
        for (int axis = 0; axis < 6; ++axis) b.f[AiBrain::kSavedDeltaA + axis] = delta[axis];
        rig->next_between();
        rig->tick();
        CHECK(rig->rounds() == 1);
        if (rig->rounds() == 1) {
            CHECK(rig->round(0).origin_x == (2 << 16));
            CHECK(rig->round(0).origin_z == 0x20000);
            CHECK(rig->round(0).dir_yaw == 0x01000000);
            CHECK(rig->round(0).dir_pitch == 0x00800000);
        }
        CHECK(b.f[AiBrain::kAmmoA] == 4);
        CHECK(b.f[AiBrain::kBurstWindow] == 2); // += step while armed
        CHECK((b.bytes()[AiBrain::kBoneFlagByte] & 0x40) != 0);
        CHECK((static_cast<uint32_t>(b.f[AiBrain::kCooldownPair]) & 0xFFFFu) == 0);
    }
    {
        auto rig = make_rig();
        rig->e().profile.flags100 = 0x40;
        AiBrain &b = rig->brain();
        b.f[AiBrain::kBurstWindow] = 1;
        b.f[AiBrain::kLastWeapon] = 1;
        b.f[AiBrain::kNoTargetIdle] = 1;
        b.bytes()[AiBrain::kBoneFlagByte] = 0x40;
        rig->next_between();
        rig->tick();
        CHECK(rig->rounds() == 0);
        CHECK(b.bytes()[AiBrain::kBoneFlagByte] == 0);
    }
}

// R2-4: the chase. A planar 16.16 distance against the approach cap; inside
// it the give-up timer rests only while AI_GetSuspensionFirePoint validates
// the target (a ridge in the way keeps it running); closer than min_chase
// the speed matches the target (its SM brain's speed word, else its planar
// velocity), from min_chase on it is the combat speed; the tail zeroes the
// work X/Y.
// [orig: @0x473ABE..0x473C06: `cmp eax,[ecx+4Ch]` @0x473AF3, `cmp
//  eax,[ebp+0BCh]` @0x473B29, AI_GetSuspensionFirePoint @0x473B36, `cmp
//  edx,[ecx+0B8h]` @0x473B53, brain+0x220 @0x473B72, |vel xy|
//  @0x473B83..0x473BCE; tail @0x473C14..0x473C50]
static void test_chase_matches_the_target_inside_min_chase() {
    const auto configure = [](Rig &rig) {
        AiProfile &p = rig.e().profile;
        p.min_chase = 30 << 16;
        p.max_chase = 50 << 16;
        rig.target_entity().veh.vel_x = 3 << 16;
        rig.target_entity().veh.vel_y = 4 << 16;
        rig.brain().f[AiBrain::kCombatTimer] = 100;
        rig.brain().f[AiBrain::kWorkPosX] = 77;
    };
    {
        auto rig = make_rig();
        configure(*rig);
        rig->w.ai.terrain = ridge(); // out of sight: the timer runs on
        rig->tick();
        AiBrain &b = rig->brain();
        CHECK(b.f[AiBrain::kCombatTimer] == 116);
        CHECK(b.f[AiBrain::kOutSpeed] == (5 << 16)); // |(3, 4)| u per tick
        CHECK(b.f[AiBrain::kWorkPosX] == 0);
        CHECK(b.f[AiBrain::kWorkHeading] == 0);
    }
    {
        auto rig = make_rig();
        configure(*rig);
        rig->tick(); // in sight: the timer rests
        CHECK(rig->brain().f[AiBrain::kCombatTimer] == 0);
    }
    {
        auto rig = make_rig();
        configure(*rig);
        rig->w.ai.at(rig->w.ai.attach(rig->target))->brain.f[136] = 2 << 16;
        rig->tick(); // a target with an SM brain lends its speed word
        CHECK(rig->brain().f[AiBrain::kOutSpeed] == (2 << 16));
    }
    {
        auto rig = make_rig();
        configure(*rig);
        rig->e().profile.min_chase = 10 << 16;
        rig->tick(); // from min_chase on: the combat speed
        CHECK(rig->brain().f[AiBrain::kOutSpeed] == (10 << 16));
    }
}

int main() {
    test_processed_fire_checks_los_and_the_continuation_defers_it();
    test_failed_rescan_drops_the_target();
    test_turret_stages_between_processed_ticks();
    test_no_ammo_latch_forces_the_processed_leg();
    test_primary_continuation_solves_from_a_level_pitch();
    test_rc_fire_stationary_leg();
    test_ateam_sweep_steps_per_shot();
    test_ateam_lock_replays_the_saved_pose();
    test_chase_matches_the_target_inside_min_chase();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ai_vehicle_combat: all passed\n");
    return 0;
}
