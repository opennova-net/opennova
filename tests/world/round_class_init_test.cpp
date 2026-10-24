// A round's TrcrID class binding: the init callback its ai_function row names (only the nade,
// schl and vmne rows spin the round; the clym row zeroes the spins and stores 1 into +0x2A8; a
// row without an init, or no row, leaves the allocation's zeroes) and the rokt row's one-shot
// motor, which the ballistic leg runs and whose bound +452 exempts the round from the tail's
// gravity and drag. The values are the cited stores', the rows and ammo the test's own.
// [orig: Entity_InitFromItemDef @0x49E550 (+0x1C4 @0x49E586..0x49E58C, the init tail-call
//  @0x49E5D5..0x49E5EA); Entity_InitThrowableSpin_Nade @0x4435A0, Entity_InitThrowableSpin_Satchel
//  @0x4435C0, Entity_InitThrowableSpin_AVMine @0x443610, Entity_InitThrowableSpin_Claymore
//  @0x4435E0; CEntityManager_AllocateSlot @0x4EAAD0 (the memset @0x4EABC8); the rokt motor
//  Entity_UpdateRocketFlag @0x443DD0; Projectile_UpdatePhysics @0x4E9D70 (@0x4EA06A..0x4EA085, @0x4EAA34..0x4EAA5A)]
#include <cstdio>
#include <memory>
#include <vector>

#include <runtime/terrain_query/height_field.h>
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

constexpr int32_t kOneDegree = 11930464; // the init's 0xB60B60
constexpr uint32_t kForceTracer = 0x8000u;
constexpr int kAmmoRocket = 1;
constexpr int kAmmoBullet = 2;
constexpr int kItemRocket = 3200;

// One init per class row, over a slot as the allocation zeroed it.
void test_only_the_spin_rows_spin() {
    auto world = std::make_unique<World>();
    struct Case { ThrowClass think; int32_t spin; };
    const Case cases[] = {
        {ThrowClass::kNade, kOneDegree},    // @0x4435A0
        {ThrowClass::kSatchel, kOneDegree}, // @0x4435C0
        {ThrowClass::kAVMine, kOneDegree},  // @0x443610
        {ThrowClass::kNone, 0},             // a row with no init: the allocation's zeroes
        {ThrowClass::kStinger, 0},          // a guided row's init sets no spin
    };
    for (const Case &c : cases) {
        ThrowableClassRow row;
        row.think = c.think;
        LiveRound r;
        r.mass_q16 = 0x123;
        throwable_round_init(*world, r, &row);
        CHECK(r.spin_yaw == c.spin && r.spin_pitch == c.spin && r.spin_roll == c.spin);
        CHECK(r.mass_q16 == 0x123);
    }
    LiveRound none;
    throwable_round_init(*world, none, nullptr);
    CHECK(none.spin_yaw == 0 && none.spin_pitch == 0 && none.spin_roll == 0);
    // The clym init: the three spins 0 and +0x2A8 (the spawn's mass word) := 1 (@0x4435F8).
    ThrowableClassRow clym;
    clym.think = ThrowClass::kClaymore;
    LiveRound c;
    c.spin_yaw = c.spin_pitch = c.spin_roll = 7;
    c.mass_q16 = 0x123;
    throwable_round_init(*world, c, &clym);
    CHECK(c.spin_yaw == 0 && c.spin_pitch == 0 && c.spin_roll == 0);
    CHECK(c.mass_q16 == 1);
}

// The rokt motor stores 1 only over a 0 word (@0x443DD4..0x443DDE).
void test_the_rokt_motor_is_one_shot() {
    LiveRound r;
    r.move_row = PhysicsClass::Rokt;
    CHECK(round_class_motor_bound(r));
    rocket_motor_tick(r);
    CHECK(r.guided.phase == 1);
    rocket_motor_tick(r);
    CHECK(r.guided.phase == 1);
    r.guided.phase = 5;
    rocket_motor_tick(r);
    CHECK(r.guided.phase == 5);
    LiveRound plain;
    CHECK(!round_class_motor_bound(plain));
}

struct FlatField {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;
    FlatField() : heightmap(kDim * kDim, 0), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// A ballistic rokt round runs its motor and keeps its vertical velocity (no -167 step); a plain
// round fired the same way takes the step.
void test_a_rokt_round_flies_without_gravity() {
    auto world = std::make_unique<World>();
    World &w = *world;
    FlatField flat;
    w.tables.terrain = &flat.field;
    w.env.water_z = -(1000 << 16);
    w.tables.ammo.entries.resize(3);
    w.tables.ammo.entries[0].name = "AT_NULL";
    w.tables.ammo.entries[0].valid = true;
    for (int index : {kAmmoRocket, kAmmoBullet}) {
        AmmoTableEntry &ammo = w.tables.ammo.entries[static_cast<size_t>(index)];
        ammo.name = index == kAmmoRocket ? "ROCKET" : "BULLET";
        ammo.valid = true;
        ammo.flags = kForceTracer;
        ammo.velocity = 20;
        ammo.max_age_ticks = 600;
    }
    w.tables.ammo.entries[kAmmoRocket].tracer_item_friendly = kItemRocket;
    ThrowableClassRow rokt;
    rokt.item_id = kItemRocket;
    rokt.move_row = PhysicsClass::Rokt;
    w.throwables.classes.set(rokt);
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 8);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.health = 100;
    seed.position.set_q16({50 << 16, 50 << 16, 30 << 16});
    const EntityHandle source = w.registry.spawn(1, seed);
    auto fire = [&](int ammo) {
        RoundSpawnParams p;
        p.owner = source;
        p.shooter_handle = source.packed;
        p.origin.set_q16({50 << 16, 50 << 16, 30 << 16});
        p.ammo_index = ammo;
        return w.round_sim.spawn(w, p);
    };
    const int rocket = fire(kAmmoRocket);
    const int bullet = fire(kAmmoBullet);
    CHECK(rocket >= 0 && bullet >= 0);
    if (rocket < 0 || bullet < 0) return;
    LiveRound &r = w.round_sim.rounds[static_cast<size_t>(rocket)];
    LiveRound &b = w.round_sim.rounds[static_cast<size_t>(bullet)];
    CHECK(r.move_row == PhysicsClass::Rokt);
    CHECK(r.spin_yaw == 0); // the rokt row's init sets no spin
    const int32_t rocket_vz = r.vel.z.q16;
    const int32_t bullet_vz = b.vel.z.q16;
    w.round_sim.tick(w, &flat.field, nullptr);
    CHECK(r.active && b.active);
    CHECK(r.guided.phase == 1);
    CHECK(r.vel.z.q16 == rocket_vz);
    CHECK(b.vel.z.q16 < bullet_vz);
}

} // namespace

int main() {
    test_only_the_spin_rows_spin();
    test_the_rokt_motor_is_one_shot();
    test_a_rokt_round_flies_without_gravity();
    if (failures == 0) std::printf("round_class_init: all passed\n");
    return failures == 0 ? 0 : 1;
}
