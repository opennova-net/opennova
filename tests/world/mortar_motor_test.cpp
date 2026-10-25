// The arti row's motor, the shell a mortar fires: a useownmove round whose TrcrID item binds
// move_function arti runs it from the update's useownmove leg. The drag first, then the
// words advance with the -167 gravity step on Z; the water crossing splashes (tag 11),
// quarters the horizontal words and arms the 0x1000 bit; under the terrain the shell arms
// where it stands; the update head then releases the armed shell. The values are the cited
// arithmetic's over the test's own words.
// [orig: Entity_UpdateArtiShell @0x445500 (the drag @0x445530, the advance
//  @0x445535..0x44557D, the speed floor @0x4455AE..0x4455BE, the water arm
//  @0x4455C0..0x44566C, the terrain arm @0x445686..0x4456CA); the physics row "arti"
//  @0x82AC7C; Projectile_UpdatePhysics @0x4E9D70 (the useownmove +452 call @0x4E9F1E, the
//  head's 0x1000 test @0x4E9DC6)]
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

constexpr uint32_t kForceTracer = 0x8000u;
constexpr uint32_t kUseOwnMove = 0x2000u;
constexpr int kAmmoShell = 1;
constexpr int kItemShell = 1878; // a TrcrID item whose move_function is arti

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

struct Rig {
    std::unique_ptr<World> owner = std::make_unique<World>();
    World &w = *owner;
    FlatField flat;
    EntityHandle source;
    explicit Rig(int32_t drag_fp16) {
        w.tables.terrain = &flat.field;
        w.env.water_z = -(1000 << 16);
        w.tables.ammo.entries.resize(2);
        w.tables.ammo.entries[0].name = "AT_NULL";
        w.tables.ammo.entries[0].valid = true;
        AmmoTableEntry &shell = w.tables.ammo.entries[kAmmoShell];
        shell.name = "SHELL";
        shell.valid = true;
        shell.flags = kForceTracer | kUseOwnMove;
        shell.velocity = 20;
        shell.max_age_ticks = 600;
        shell.drag_fp16 = drag_fp16;
        shell.tracer_item_friendly = kItemShell;
        ThrowableClassRow row;
        row.item_id = kItemShell;
        row.move_row = PhysicsClass::Arti;
        w.throwables.classes.set(row);
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.health = 100;
        seed.position.set_q16({50 << 16, 50 << 16, 30 << 16});
        source = w.registry.spawn(1, seed);
    }
    // A shell at the given words; the spawn's own velocity is replaced.
    LiveRound *fire(FixedVec3 pos, FixedVec3 vel) {
        RoundSpawnParams p;
        p.owner = source;
        p.shooter_handle = source.packed;
        p.origin.set_q16(pos);
        p.ammo_index = kAmmoShell;
        const int slot = w.round_sim.spawn(w, p);
        if (slot < 0) return nullptr;
        LiveRound &r = w.round_sim.rounds[static_cast<size_t>(slot)];
        r.pos.set_q16(pos);
        r.vel.set_q16(vel);
        return &r;
    }
    void tick() { w.round_sim.tick(w, &flat.field, nullptr); }
};

// In flight: the drag over the words as they stand, then X and Y advance by the dragged
// words and Z by the dragged Z word less 167. Nothing arms.
void test_the_shell_flies_drag_first() {
    Rig rig(3277); // the stock shell's drag, 0.05
    const FixedVec3 p0{60 << 16, 70 << 16, 30 << 16};
    const FixedVec3 v0{0x20000, 0x10000, 0x8000};
    LiveRound *r = rig.fire(p0, v0);
    CHECK(r != nullptr);
    if (r == nullptr) return;
    CHECK(r->move_row == PhysicsClass::Arti);
    CHECK(r->spin_yaw == 0 && r->spin_pitch == 0); // the arti name binds no class init
    FixedVec3 v = v0;
    projectile_apply_drag(v, rig.w.tables.ammo.entries[kAmmoShell], p0.z, rig.w.env.water_z);
    CHECK(v.x != v0.x); // the drag moved the words this pin rides on
    rig.tick();
    CHECK(r->active);
    CHECK(!r->det_at_expiry);
    CHECK(r->pos.x.q16 == p0.x + v.x);
    CHECK(r->pos.y.q16 == p0.y + v.y);
    CHECK(r->vel.z.q16 == v.z - 167);
    CHECK(r->pos.z.q16 == p0.z + (v.z - 167));
    CHECK(r->vel.x.q16 == v.x && r->vel.y.q16 == v.y);
}

// Under the terrain: armed where it stands (no clamp to the ground), then the head releases
// it on the next update.
void test_the_shell_arms_under_the_terrain() {
    Rig rig(0);
    const int32_t ground =
            opennova::terrain::height_field_height_bilinear_fixed(rig.flat.field, 60 << 16, 70 << 16);
    const FixedVec3 p0{60 << 16, 70 << 16, ground + 0x8000};
    LiveRound *r = rig.fire(p0, FixedVec3{0, 0, -0x10000});
    CHECK(r != nullptr);
    if (r == nullptr) return;
    rig.tick();
    CHECK(r->det_at_expiry);
    CHECK(r->pos.z.q16 == p0.z - 0x10000 - 167);
    CHECK(r->pos.z.q16 < ground);
    rig.tick();
    CHECK(!r->active);
}

// Into the water: the splash at the water word (tag 11), the horizontal words at a quarter
// with the +0x8000 rounding, Z as moved; armed.
void test_the_shell_arms_in_the_water() {
    Rig rig(0);
    const int32_t ground =
            opennova::terrain::height_field_height_bilinear_fixed(rig.flat.field, 60 << 16, 70 << 16);
    rig.w.env.water_z = ground + (10 << 16);
    const FixedVec3 p0{60 << 16, 70 << 16, rig.w.env.water_z + 0x4000};
    LiveRound *r = rig.fire(p0, FixedVec3{0x20001, -0x6000, -0x10000});
    CHECK(r != nullptr);
    if (r == nullptr) return;
    const size_t impacts_before = rig.w.round_sim.impacts.size();
    rig.tick();
    CHECK(r->det_at_expiry);
    CHECK(r->vel.x.q16 == 0x8000);  // (0x20001 * 0x4000 + 0x8000) >> 16
    CHECK(r->vel.y.q16 == -0x1800); // (-0x6000 * 0x4000 + 0x8000) >> 16
    CHECK(r->vel.z.q16 == -0x10000 - 167);
    CHECK(r->pos.x.q16 == p0.x + 0x20001);
    CHECK(r->pos.z.q16 == p0.z - 0x10000 - 167);
    bool splash = false;
    for (size_t i = impacts_before; i < rig.w.round_sim.impacts.size(); ++i)
        if (rig.w.round_sim.impacts[i].effect_tag == 11) splash = true;
    CHECK(splash);
}

// A shell that starts under the water word takes no splash: the crossing needs the saved Z
// above it.
void test_a_submerged_shell_takes_no_splash() {
    Rig rig(0);
    const int32_t ground =
            opennova::terrain::height_field_height_bilinear_fixed(rig.flat.field, 60 << 16, 70 << 16);
    rig.w.env.water_z = ground + (20 << 16);
    const FixedVec3 p0{60 << 16, 70 << 16, ground + (10 << 16)};
    LiveRound *r = rig.fire(p0, FixedVec3{0x10000, 0, 0});
    CHECK(r != nullptr);
    if (r == nullptr) return;
    rig.tick();
    CHECK(!r->det_at_expiry);
    CHECK(r->vel.x.q16 == 0x10000);
}

} // namespace

int main() {
    test_the_shell_flies_drag_first();
    test_the_shell_arms_under_the_terrain();
    test_the_shell_arms_in_the_water();
    test_a_submerged_shell_takes_no_splash();
    if (failures == 0) std::printf("mortar_motor: all passed\n");
    return failures == 0 ? 0 : 1;
}
