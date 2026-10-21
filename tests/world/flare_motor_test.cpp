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

#include <runtime/terrain_query/height_field.h>
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
};

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

} // namespace

int main() {
    test_the_ring_records();
    test_the_air_flare_init_and_motor();
    test_the_ground_flare_init();
    test_the_bounces_the_eighth_and_the_release();
    test_a_ballistic_flare_takes_no_tail_gravity();
    if (failures == 0) std::printf("PASS flare_motor\n");
    return failures == 0 ? 0 : 1;
}
