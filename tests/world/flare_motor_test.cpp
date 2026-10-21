// D-NET-64: the flare rounds and the projectile target ring. The ring's records (its
// pre-incremented index, its saturating count, a record another round took, the release,
// the pass's countdown over the counted records); the aflr and gflr inits (the source's
// last move or velocity words, the heat word, the ground under the round); the shell-bounce
// motor's words (drag, gravity, the move, the heat decay, the ground clamp and bounce, the
// eighth bounce, the water plane) on the useownmove leg and ahead of the ballistic sweep,
// whose gravity and drag it suppresses; a released flare frees its record. The values are
// the cited arithmetic's, the rows and ammo the test's own.
// [orig: ProjectileTargetRing_Update @0x4E70F0; Weapon_UpdateAllProjectiles @0x4EC020
//  (@0x4EC0A1..0x4EC0CF); Projectile_ReleaseEffects @0x4E8280 (@0x4E82E3..0x4E82F2);
//  Entity_UpdateShellBounce @0x443DF0; the aflr init @0x4434B0; the gflr init @0x443510;
//  Projectile_UpdatePhysics @0x4E9D70 (@0x4EA06A..0x4EA085, @0x4EAA34..0x4EAA4C)]
#include <cstdio>
#include <memory>
#include <vector>

#include <base/io/rotating_prng.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/projectile_target_ring.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/throwables.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr uint32_t kUseOwnMove = 0x2000u;
constexpr uint32_t kForceTracer = 0x8000u;
constexpr int kAmmoFlare = 1;
constexpr int kItemAirFlare = 3100;
constexpr int kItemGroundFlare = 3101;
constexpr int kAmmoStinger = 2;
constexpr int kAmmoBullet = 3;
constexpr int kItemStinger = 3102;

struct FlatField {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;
    explicit FlatField(uint16_t raw16) : heightmap(kDim * kDim, raw16), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

int32_t q16_mul(int32_t value, int32_t factor) {
    return static_cast<int32_t>(static_cast<uint32_t>(
            static_cast<uint64_t>(static_cast<int64_t>(value) * factor + 0x8000) >> 16));
}

struct Rig {
    std::unique_ptr<World> w_heap = std::make_unique<World>();
    World &w = *w_heap;
    FlatField flat{0};
    EntityHandle source;

    explicit Rig(uint32_t ammo_flags = kUseOwnMove | kForceTracer) {
        w.tables.ammo.entries.resize(2);
        w.tables.ammo.entries[0].name = "AT_NULL";
        w.tables.ammo.entries[0].valid = true;
        AmmoTableEntry &flare = w.tables.ammo.entries[kAmmoFlare];
        flare.name = "FLARE";
        flare.valid = true;
        flare.flags = ammo_flags;
        flare.velocity = 20;
        flare.max_age_ticks = 600;
        flare.tracer_item_friendly = kItemAirFlare;
        ThrowableClassRow air{kItemAirFlare, ThrowClass::kAirFlare, ThrowClass::kAirFlare, 5, 0, 0, 1};
        air.heat_sig = 0x1234;
        w.throwables.classes.set(air);
        ThrowableClassRow ground{kItemGroundFlare, ThrowClass::kGroundFlare,
                ThrowClass::kGroundFlare, 5, 0, 0, 2};
        ground.heat_sig = 0x0800;
        w.throwables.classes.set(ground);
        w.tables.terrain = &flat.field;
        w.env.water_z = -(1000 << 16);
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.health = 100;
        seed.position.set_q16({50 << 16, 50 << 16, 30 << 16});
        source = w.registry.spawn(1, seed);
    }
    int fire() {
        RoundSpawnParams p;
        p.owner = source;
        p.shooter_handle = source.packed;
        p.origin.set_q16({50 << 16, 50 << 16, 30 << 16});
        p.ammo_index = kAmmoFlare;
        return w.round_sim.spawn(w, p);
    }
    LiveRound &round(int slot) { return w.round_sim.rounds[static_cast<size_t>(slot)]; }
    void tick() { w.round_sim.tick(w, &flat.field, nullptr); }
    // A Stinger round (its TrcrID item binds the stng class) and a plain bullet, and an AI
    // shooter with its slot (the stng init queries over it), facing +X at (50, 50, 30).
    EntityHandle add_stinger() {
        w.tables.ammo.entries.resize(4);
        AmmoTableEntry &stinger = w.tables.ammo.entries[kAmmoStinger];
        stinger.name = "STINGER";
        stinger.valid = true;
        stinger.velocity = 40;
        stinger.max_age_ticks = 600;
        stinger.heat_det_range = 3000;
        stinger.boresight_maxang = 0x10000000;
        stinger.tracer_item_friendly = kItemStinger;
        ThrowableClassRow row{kItemStinger, ThrowClass::kNone, ThrowClass::kStinger};
        w.throwables.classes.set(row);
        AmmoTableEntry &bullet = w.tables.ammo.entries[kAmmoBullet];
        bullet.name = "BULLET";
        bullet.valid = true;
        bullet.velocity = 100;
        bullet.max_age_ticks = 600;
        Entity person;
        person.kind = EntityKind::Organic;
        person.team = 1;
        person.health = 100;
        person.alive = true;
        person.position.set_q16({50 << 16, 50 << 16, 30 << 16});
        const EntityHandle shooter = w.registry.spawn(0, person);
        w.ai.attach(shooter);
        AiEntity &body = *w.ai.for_handle(shooter);
        body.inf.active = true;
        body.self->position.set_q16({50 << 16, 50 << 16, 30 << 16});
        return shooter;
    }
    int fire_from(EntityHandle shooter, int ammo) {
        RoundSpawnParams p;
        p.owner = shooter;
        p.shooter_handle = shooter.packed;
        p.origin.set_q16({50 << 16, 50 << 16, 30 << 16});
        p.ammo_index = ammo;
        return w.round_sim.spawn(w, p);
    }
    // Two ring records: the first lands in slot 1, which [0, count) reads once a second exists.
    void ring_pair(int seen, int other) {
        ProjectileTargetRing &ring = w.round_sim.target_ring;
        round(seen).target_ring_slot = ring.update(seen, -1, 10);
        round(other).target_ring_slot = ring.update(other, -1, 10);
    }
};
bool same_pos(const int32_t steer[3], const LiveRound &r) {
    return steer[0] == r.pos.x.q16 && steer[1] == r.pos.y.q16 && steer[2] == r.pos.z.q16;
}

void test_the_ring_records() {
    auto ring = std::make_unique<ProjectileTargetRing>();
    // The index is incremented ahead of the store: the first record is slot 1.
    CHECK(ring->update(7, -1, 10) == 1 && ring->count == 1);
    CHECK(ring->records[1].round == 7 && ring->records[1].ttl == 10);
    // The pass counts [0, count) alone: record 1 waits for a second record.
    ring->count_down();
    CHECK(ring->records[1].ttl == 10);
    CHECK(ring->update(8, -1, 3) == 2 && ring->count == 2);
    ring->count_down();
    CHECK(ring->records[1].ttl == 9 && ring->records[2].ttl == 3);
    // A refresh keeps the record while it is still the round's.
    CHECK(ring->update(7, 1, 10) == 1 && ring->records[1].ttl == 10);
    CHECK(ring->update(9, 1, 10) == -1);
    // A release frees it; the countdown frees a counted record at zero.
    CHECK(ring->update(7, 1, 0) == 1 && ring->records[1].round == -1);
    CHECK(ring->update(10, -1, 5) == 3 && ring->count == 3);
    ring->count_down();
    CHECK(ring->records[2].ttl == 2 && ring->records[3].ttl == 5);
    ring->count_down();
    ring->count_down();
    CHECK(ring->records[2].round == -1);
    // The count saturates at 128 and the index wraps to 0: later rounds take slots 0, 1.
    for (int i = 0; i < 130; ++i) (void)ring->update(100 + i, -1, 10);
    CHECK(ring->count == 128 && ring->index == 5);
    CHECK(ring->records[0].round == 224 && ring->records[1].round == 225);
}

void test_the_air_flare_init_and_motor() {
    Rig still;
    const int a = still.fire();
    CHECK(a >= 0);
    if (a < 0) return;
    Rig moving;
    Entity &src = *moving.w.registry.get(moving.source);
    src.saved_live_valid = true;
    src.saved_live_pos[0] = (50 << 16) - 0x18000; // the source moved +1.5, -0.25 last tick
    src.saved_live_pos[1] = (50 << 16) + 0x4000;
    src.saved_live_pos[2] = 30 << 16;
    const int b = moving.fire();
    CHECK(b == a);
    const LiveRound &r0 = still.round(a);
    LiveRound &r = moving.round(b);
    CHECK(r.motor == ThrowClass::kAirFlare && r.think == ThrowClass::kAirFlare);
    CHECK(r.vel.x.q16 == r0.vel.x.q16 + 0x18000 && r.vel.y.q16 == r0.vel.y.q16 - 0x4000);
    CHECK(r.vel.z.q16 == r0.vel.z.q16);
    CHECK(r.heat_sig == 0x1234 && r0.heat_sig == 0x1234);
    CHECK(r.prev_z_q16 == 0);            // the flat ground under it
    CHECK(r.spin_yaw == 0);              // the flare init binds no spin
    CHECK(r.target_ring_slot == -1);
    // One motor tick, the words as the shell bounce computes them.
    const int32_t x0 = r.pos.x.q16, y0 = r.pos.y.q16, z0 = r.pos.z.q16;
    const int32_t vx = q16_mul(r.vel.x.q16, 0xFC28);
    const int32_t vy = q16_mul(r.vel.y.q16, 0xFC28);
    const int32_t vz = r.vel.z.q16 - 167;
    moving.tick();
    CHECK(r.active);
    CHECK(r.vel.x.q16 == vx && r.vel.y.q16 == vy && r.vel.z.q16 == vz);
    CHECK(r.pos.x.q16 == x0 + vx && r.pos.y.q16 == y0 + vy && r.pos.z.q16 == z0 + vz);
    CHECK(r.heat_sig == static_cast<uint16_t>(q16_mul(0x1234 << 16, 0xFD70) >> 16));
    CHECK(r.target_ring_slot == 1);
    const ProjectileTargetRing &ring = moving.w.round_sim.target_ring;
    CHECK(ring.records[1].round == b && ring.records[1].ttl == 10 && ring.count == 1);
}

void test_the_ground_flare_init() {
    Rig rig;
    rig.w.tables.ammo.entries[kAmmoFlare].tracer_item_friendly = kItemGroundFlare;
    Entity &src = *rig.w.registry.get(rig.source);
    src.veh.vel_x = 0x1000;
    src.veh.vel_y = -0x2000;
    src.veh.slide_z = 0x300;
    const int slot = rig.fire();
    CHECK(slot >= 0);
    if (slot < 0) return;
    const LiveRound &r = rig.round(slot);
    CHECK(r.vel.x.q16 == 0x1000 && r.vel.y.q16 == -0x2000 && r.vel.z.q16 == 0x300 + 0x538);
    CHECK(r.heat_sig == 0x0800);
    // Without a source: no drift, 0x538 of rise, no heat word.
    Rig bare;
    bare.w.tables.ammo.entries[kAmmoFlare].tracer_item_friendly = kItemGroundFlare;
    RoundSpawnParams p;
    p.origin.set_q16({50 << 16, 50 << 16, 30 << 16});
    p.ammo_index = kAmmoFlare;
    const int s2 = bare.w.round_sim.spawn(bare.w, p);
    CHECK(s2 >= 0);
    if (s2 < 0) return;
    const LiveRound &r2 = bare.round(s2);
    CHECK(r2.vel.x.q16 == 0 && r2.vel.y.q16 == 0 && r2.vel.z.q16 == 0x538 && r2.heat_sig == 0);
}

void test_the_bounces_the_eighth_and_the_release() {
    Rig rig;
    const int slot = rig.fire();
    CHECK(slot >= 0);
    if (slot < 0) return;
    LiveRound &r = rig.round(slot);
    // Just over the ground, falling.
    r.pos.set_q16({50 << 16, 50 << 16, 0x100});
    r.vel.set_q16({0x10000, -0x8000, -0x20000});
    rig.tick();
    const int32_t vx = q16_mul(0x10000, 0xFC28), vy = q16_mul(-0x8000, 0xFC28);
    CHECK(r.pos.z.q16 == 0 && r.bounce_count == 1);
    CHECK(r.vel.x.q16 == (vx >> 1) && r.vel.y.q16 == (vy >> 1));
    CHECK(r.vel.z.q16 == (-(-0x20000 - 167) >> 2));
    CHECK(r.active);
    // The eighth bounce ends it: life -1, released by the next head with its record.
    r.bounce_count = 8;
    r.pos.set_q16({60 << 16, 60 << 16, 0x100});
    r.vel.set_q16({0x10000, 0, -0x20000});
    rig.tick();
    CHECK(r.active && r.max_age_ticks == r.age_ticks);
    const int32_t record = r.target_ring_slot;
    CHECK(record >= 0);
    rig.tick();
    CHECK(!r.active);
    CHECK(rig.w.round_sim.target_ring.records[static_cast<size_t>(record)].round == -1);
    // Under the water plane ends it at once.
    Rig wet;
    wet.w.env.water_z = 10 << 16;
    const int s = wet.fire();
    CHECK(s >= 0);
    if (s < 0) return;
    LiveRound &w = wet.round(s);
    w.pos.set_q16({50 << 16, 50 << 16, (10 << 16) + 0x10});
    w.vel.set_q16({0x10000, 0, -0x10000});
    wet.tick();
    CHECK(w.max_age_ticks == w.age_ticks);
}

void test_a_ballistic_flare_takes_no_tail_gravity() {
    Rig rig(kForceTracer); // no useownmove: the motor ahead of the stock sweep
    const int slot = rig.fire();
    CHECK(slot >= 0);
    if (slot < 0) return;
    LiveRound &r = rig.round(slot);
    r.vel.set_q16({0x10000, 0, 0x8000});
    const int32_t vz = 0x8000 - 167;
    rig.tick();
    // The motor's fall alone: the tail's gravity and drag are skipped for a bound motor.
    CHECK(r.vel.z.q16 == vz);
    CHECK(r.vel.x.q16 == q16_mul(0x10000, 0xFC28));
}


// The Stinger's in-flight query walks the ring's counted records: an air flare ahead of the
// missile wins over no entity, past the MP roll (a player's missile, a flare an AI source
// fired): chance 100 above three deployed players draws nothing; at two the chance is 50 and
// one rotate-LCG draw, (low 16 >> 2) % 101 under it, admits it. A missile no player fired
// draws nothing.
// [orig: Entity_FindTargets @0x53A610 (@0x53ACF0..0x53ADE0); Weapon_CalcDamageByType
//  @0x539140 (@0x539385..0x539433); Weapon_GetFlareDeflectChance @0x5382A0]
void test_the_in_flight_query_takes_an_air_flare() {
    Rig rig;
    rig.w.rules.mp_session = true;
    const int a = rig.fire();
    const int b = rig.fire();
    CHECK(a >= 0 && b >= 0 && a != b);
    if (a < 0 || b < 0) return;
    LiveRound &flare = rig.round(a);
    flare.pos.set_q16({150 << 16, 50 << 16, 30 << 16});
    // The first record lands in slot 1 and the count reads [0, count): two records make the
    // first one visible.
    ProjectileTargetRing &ring = rig.w.round_sim.target_ring;
    flare.target_ring_slot = ring.update(a, -1, 10);
    rig.round(b).target_ring_slot = ring.update(b, -1, 10);
    CHECK(ring.count == 2 && ring.records[1].round == a);
    AmmoTableEntry stinger;
    stinger.heat_det_range = 500;
    stinger.boresight_maxang = 0x10000000;
    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.team = 1;
    shooter.flags = kEntityFlagPlayer;
    shooter.health = 100;
    shooter.position.set_q16({50 << 16, 50 << 16, 30 << 16});
    Entity seeker = shooter;
    AiEntity pose;
    pose.self = &seeker;
    pose.heading = 0;
    rig.w.server_num_players = 4;
    rig.w.ai.prng_a = 0x1234567u;
    TargetRef pick = guided_heat_target(rig.w, seeker, pose, nullptr, stinger, &shooter);
    CHECK(pick.round == a && !pick.entity.valid());
    CHECK(rig.w.ai.prng_a == 0x1234567u);
    // Two players: one draw against 50.
    rig.w.server_num_players = 2;
    for (uint32_t seed : {0x1234567u, 0x89ABCDEFu, 0x0F0F0F0Fu}) {
        rig.w.ai.prng_a = seed;
        uint32_t state = seed;
        const uint16_t draw = opennova::io::rotating_prng_next16(state);
        const bool kept = ((draw >> 2) % 101) < 50;
        pick = guided_heat_target(rig.w, seeker, pose, nullptr, stinger, &shooter);
        CHECK(rig.w.ai.prng_a == state);
        CHECK(pick.round == (kept ? a : -1));
    }
    // An AI's missile: no roll.
    Entity ai_shooter = shooter;
    ai_shooter.flags = 0;
    rig.w.ai.prng_a = 0x1234567u;
    CHECK(guided_heat_target(rig.w, seeker, pose, nullptr, stinger, &ai_shooter).round == a);
    CHECK(rig.w.ai.prng_a == 0x1234567u);
    // A team-0 query: the head exit needs the query entity's AI slot, so the missile
    // round (no slot) still walks; a slot without the 0x200 bit finds nothing
    // [orig: Entity_FindTargets @0x53A610 (@0x53A6BF..0x53A6E1)].
    Entity neutral = seeker;
    neutral.team = 0;
    CHECK(guided_heat_target(rig.w, neutral, pose, nullptr, stinger, &ai_shooter).round == a);
    AiSlot slot{};
    CHECK(!guided_heat_target(rig.w, neutral, pose, &slot, stinger, &ai_shooter).valid());
    slot.f[1] = 0x200;
    CHECK(guided_heat_target(rig.w, neutral, pose, &slot, stinger, &ai_shooter).round == a);
}


// The +0xC word as one word: an entity's handle + 1, a round's -(slot + 1), 0 for none.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B520A; the stng init @0x445F10]
void test_the_target_word() {
    const EntityHandle h = EntityHandle::make(1, 5);
    CHECK(AiSlot::target_word(TargetRef{h}) == int32_t(h.packed) + 1);
    CHECK(AiSlot::target_word(TargetRef::of_round(7)) == -8);
    CHECK(AiSlot::target_word(TargetRef{}) == 0);
    CHECK(AiSlot::target_of(int32_t(h.packed) + 1) == TargetRef{h});
    const TargetRef round = AiSlot::target_of(-8);
    CHECK(round.round == 7 && !round.entity.valid() && round.entity.packed == 0xFFFF);
    CHECK(!AiSlot::target_of(0).valid());
}

// The stng init's query walks the ring, so an air flare ahead is its winner: the shooter's
// +0xC and the missile's +0x2D4 hold the round (the launch has no shell-projectile test),
// the first tick's owner callback takes it again and sets flag 2, and each tick samples
// its Position. Its release clears neither its Health nor its Position, so the missile
// keeps the frozen point; a bullet spawned into the slot reads Health 0 and the missile
// drops it.
// [orig: the stng init @0x445EF0 (@0x445F02..0x445F22); PersonClass_GetTarget @0x4B0DD0;
//  Entity_UpdateGuidedMissile_0 @0x446060 (the first tick @0x4460FD..0x44612C, the
//  liveness @0x4461A6..0x4461E6, the sample @0x4463A3); Projectile_ReleaseEffects
//  @0x4E8280; CEntityManager_AllocateSlot @0x4EAAD0]
void test_the_launch_holds_a_flare() {
    Rig rig;
    const EntityHandle shooter = rig.add_stinger();
    const int a = rig.fire();
    const int b = rig.fire();
    CHECK(a == 0 && b == 1);
    if (a != 0 || b != 1) return;
    rig.round(a).pos.set_q16({150 << 16, 50 << 16, 30 << 16});
    rig.round(b).pos.set_q16({-150 << 16, 50 << 16, 30 << 16}); // behind: outside the bore
    rig.ring_pair(a, b);
    const int m = rig.fire_from(shooter, kAmmoStinger);
    CHECK(m == 2);
    if (m != 2) return;
    LiveRound &missile = rig.round(m);
    CHECK(missile.guided_family == GuidedFamily::Stinger);
    const AiEntity &body = *rig.w.ai.for_handle(shooter);
    CHECK(body.slot.f[3] == -(a + 1));
    CHECK(rig.w.registry.get(shooter)->last_fire_target == TargetRef::of_round(a));
    CHECK(missile.guided.target == 0xFFFF && missile.guided.target_round == a);
    rig.tick(); // the first tick: the owner's word, flag 2; the flare moved first (slot 0)
    CHECK((missile.guided.flags & 2) != 0 && missile.guided.target_round == a);
    CHECK(same_pos(missile.guided.steer, rig.round(a)));
    rig.tick();
    rig.tick();
    CHECK((missile.guided.flags & 2) != 0);
    CHECK(same_pos(missile.guided.steer, rig.round(a)));
    // The flare ends; the next head releases it.
    LiveRound &flare = rig.round(a);
    flare.max_age_ticks = flare.age_ticks;
    rig.tick();
    CHECK(!flare.active);
    const int32_t frozen[3] = {flare.pos.x.q16, flare.pos.y.q16, flare.pos.z.q16};
    rig.tick();
    CHECK((missile.guided.flags & 2) != 0 && missile.guided.target_round == a);
    CHECK(missile.guided.steer[0] == frozen[0] && missile.guided.steer[1] == frozen[1] &&
          missile.guided.steer[2] == frozen[2]);
    // A bullet takes the slot: Health 0, the target goes.
    CHECK(rig.fire_from(shooter, kAmmoBullet) == a);
    rig.tick();
    CHECK((missile.guided.flags & 2) == 0 && missile.guided.target_round == -1);
    CHECK(missile.guided.target == 0xFFFF);
}

// On a session's authority a dead target with the timer above 1 is cleared (group 2)
// ahead of the acquisition, which then runs in the same tick: a ground flare (its motor
// the jump thunk, no shell projectile) is kept as the target (group 3, its word 0xFFFF)
// and the steer samples it, not the target it replaced.
// [orig: Entity_UpdateGuidedMissile_0 @0x446060 - the liveness @0x4461A6..0x4461E6 (group
//  2 @0x4461E6), the acquisition @0x4461F2..0x446345 (the kept target and group 3
//  @0x44632B..0x446345), the sample @0x4463A3; Entity_SerializeGuidedMissileState
//  @0x447C50 (the walk's miss @0x447CD2)]
void test_a_dead_target_gives_way_to_a_ground_flare() {
    Rig rig;
    rig.w.rules.mp_session = true;
    rig.w.logic_tick = 100;
    const EntityHandle shooter = rig.add_stinger();
    Entity hot;
    hot.kind = EntityKind::Item;
    hot.item_id = 2;
    hot.item_type = 1;
    hot.team = 2;
    hot.health = 100;
    hot.alive = true;
    hot.heat_sig = 500;
    hot.position.set_q16({250 << 16, 50 << 16, 30 << 16});
    const EntityHandle target = rig.w.registry.spawn(1, hot);
    const int m = rig.fire_from(shooter, kAmmoStinger);
    CHECK(m == 0);
    if (m != 0) return;
    LiveRound &missile = rig.round(m);
    CHECK(missile.guided.target == target.packed && missile.guided.target_round == -1);
    rig.tick();
    rig.tick();
    rig.tick();
    CHECK((missile.guided.flags & 2) != 0 && missile.guided.timer > 1);
    rig.w.tables.ammo.entries[kAmmoFlare].tracer_item_friendly = kItemGroundFlare;
    const int g = rig.fire();
    const int b = rig.fire();
    CHECK(g == 1 && b == 2);
    if (g != 1 || b != 2) return;
    rig.round(g).pos.set_q16({150 << 16, 50 << 16, 30 << 16});
    rig.round(b).pos.set_q16({-150 << 16, 50 << 16, 30 << 16});
    rig.ring_pair(g, b);
    rig.w.registry.get(target)->health = 0;
    rig.w.round_sim.guided_updates.clear();
    const int32_t at[3] = {rig.round(g).pos.x.q16, rig.round(g).pos.y.q16, rig.round(g).pos.z.q16};
    rig.tick(); // the missile (slot 0) ticks ahead of the flare
    CHECK(missile.guided.target == 0xFFFF && missile.guided.target_round == g);
    CHECK((missile.guided.flags & 2) != 0 && missile.guided.timer == 62);
    CHECK(missile.guided.steer[0] == at[0] && missile.guided.steer[1] == at[1] &&
          missile.guided.steer[2] == at[2]);
    CHECK(!rig.w.round_sim.guided_updates.empty());
    if (rig.w.round_sim.guided_updates.empty()) return;
    const auto &update = rig.w.round_sim.guided_updates.back();
    CHECK(update.groups == ((1u << 2) | (1u << 3)));
    CHECK(update.state.target == 0xFFFF && update.state.target_round == g);
}

// Inside 400 u of the steer point the authority queues event 12 for the held target's
// vehicle brain (timer 0, channel 0, its brain index), which AI_HandleCommand's case 0x0C
// turns into the evade (a ground brain with the profile's 0x10 bit: pending 18, alert 2).
// A person target carries no brain at +0x64 and takes none.
// [orig: Entity_UpdateGuidedMissile_0 @0x446060 (@0x44657F..0x4465D3); AIEvent_QueueEntry
//  @0x455DA0; AI_HandleCommand @0x465770 (@0x465906..0x4659A6)]
void test_the_proximity_notice_reaches_the_brain() {
    for (const bool vehicle : {true, false}) {
        Rig rig;
        const EntityHandle shooter = rig.add_stinger();
        Entity hot;
        hot.kind = vehicle ? EntityKind::Item : EntityKind::Organic;
        hot.item_id = 2;
        hot.item_type = vehicle ? 1 : 3;
        hot.team = 2;
        hot.health = 100;
        hot.alive = true;
        hot.heat_sig = 500;
        hot.position.set_q16({150 << 16, 50 << 16, 30 << 16});
        const EntityHandle target = rig.w.registry.spawn(1, hot);
        const int index = rig.w.ai.attach(target);
        AiEntity &brain = *rig.w.ai.for_handle(target);
        brain.inf.active = !vehicle;
        brain.profile.type = 2;
        brain.profile.flags96 = 0x10;
        brain.brain.f[AiBrain::kCurState] = 16;
        const int m = rig.fire_from(shooter, kAmmoStinger);
        CHECK(m == 0);
        if (m != 0) return;
        CHECK(rig.round(m).guided.target == target.packed);
        rig.w.ai.events.clear();
        rig.tick();
        rig.tick();
        rig.tick();
        if (!vehicle) {
            CHECK(rig.w.ai.events.count() == 0);
            continue;
        }
        CHECK(rig.w.ai.events.count() >= 1);
        if (rig.w.ai.events.count() < 1) return;
        const AiEventEntry &notice = rig.w.ai.events.at(0);
        CHECK(notice.type() == 12 && notice.channel() == 0 && notice.entity_index() == index);
        CHECK(notice.timer() == 0.0f);
        rig.w.ai.events.process_timed(rig.w.ai, rig.w);
        CHECK(brain.brain.f[AiBrain::kCurState] == 18);
        CHECK(brain.brain.f[AiBrain::kAlert] == 2);
    }
}

} // namespace

int main() {
    test_the_ring_records();
    test_the_air_flare_init_and_motor();
    test_the_ground_flare_init();
    test_the_bounces_the_eighth_and_the_release();
    test_a_ballistic_flare_takes_no_tail_gravity();
    test_the_in_flight_query_takes_an_air_flare();
    test_the_target_word();
    test_the_launch_holds_a_flare();
    test_a_dead_target_gives_way_to_a_ground_flare();
    test_the_proximity_notice_reaches_the_brain();
    if (failures == 0) std::printf("PASS flare_motor\n");
    return failures == 0 ? 0 : 1;
}
