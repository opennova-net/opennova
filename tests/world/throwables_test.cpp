// Throwable tests (world/throwables.h; docs/world/world-wac-ai-re.md §26):
// the PowerThrow charge curve + spawn speed scale, the grenade motor's bounce
// and fuse, the satchel stick/convert chain, the detonator, the claymore cone
// trigger + shrapnel fan, AV-mine data-dead proximity, and the owner-death
// cleanup. Driven by a manual World, the destruction-test harness pattern.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "terrain/height_field.h"
#include "world/collision.h"
#include "world/destruction.h"
#include "world/round_sim.h"
#include "world/throwables.h"
#include "world/world.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr uint32_t kUseOwnMove = 0x2000u;
constexpr uint32_t kNoAge = 0x4000u;
constexpr uint32_t kForceTracer = 0x8000u;
constexpr uint32_t kInstantKillzone = 0x400u;
constexpr uint32_t kDetonateSatchels = 0x20u;
constexpr uint32_t kClaymoreFan = 0x20000u;

struct FlatField {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;
    explicit FlatField(uint16_t raw16)
            : heightmap(kDim * kDim, raw16), sector_grid(256, 1) {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

// The JO throwable ammo family, minimally: indices are stable for the checks.
enum : int {
    kAmmoNull = 0,
    kAmmoGrenade = 1,     // grenadehe-style: useownmove, 30 u/s, kz
    kAmmoSatchel = 2,     // satchel: useownmove + noage, 6 u/s
    kAmmoSatchelBoom = 3, // satchelboom: instantkillzone
    kAmmoDetonator = 4,   // Detonatesatchels
    kAmmoClaymore = 5,    // claymore: useownmove + noage, pieslice
    kAmmoClayKz = 6,      // claymorekillzone: instantkillzone + pieslice
    kAmmoClayShrap = 7,   // claymoreshrapnel: fan
    kAmmoAvMine = 8,      // AV_Mine: useownmove + noage, no pieslice
    kAmmoAvMineKz = 9,    // AV_Minekillzone
    kAmmoCount = 10,
};

// items.def type ids for the TrcrID models (JO: 1883 frag, 1891 satchel,
// 1895 claymore, 368 AT mine).
enum : int {
    kItemFrag = 1883,
    kItemSatchel = 1891,
    kItemClaymore = 1895,
    kItemAvMine = 368,
};

void seed_ammo(World &w) {
    w.ammo.entries.resize(kAmmoCount);
    auto &null_e = w.ammo.entries[kAmmoNull];
    null_e.name = "AT_NULL";
    null_e.valid = true;

    auto &nade = w.ammo.entries[kAmmoGrenade];
    nade.name = "grenadehe";
    nade.valid = true;
    nade.flags = kUseOwnMove | kForceTracer;
    nade.velocity = 30;
    nade.max_age_ticks = 248; // 4 s fuse
    nade.drag_fp16 = 0x10000; // drag 1
    nade.kztype = ammo_kz::kC4;
    nade.kz_damage = 205;
    nade.kz_minradius = 6.0f;
    nade.kz_maxradius = 12.0f;
    nade.tracer_item_friendly = kItemFrag;
    nade.tracer_item_enemy = kItemFrag;

    auto &satchel = w.ammo.entries[kAmmoSatchel];
    satchel.name = "satchel";
    satchel.valid = true;
    satchel.flags = kUseOwnMove | kNoAge | kForceTracer;
    satchel.velocity = 6;
    satchel.max_age_ticks = 62; // the 1 s arm delay
    satchel.drag_fp16 = 0x10000;
    satchel.tracer_item_friendly = kItemSatchel;
    satchel.tracer_item_enemy = 0; // invisible to enemies (retail data)

    auto &boom = w.ammo.entries[kAmmoSatchelBoom];
    boom.name = "satchelboom";
    boom.valid = true;
    boom.flags = kInstantKillzone;
    boom.velocity = 6;
    boom.kztype = ammo_kz::kStandard;
    boom.kz_damage = 20000;
    boom.kz_minradius = 5.0f;
    boom.kz_maxradius = 15.0f;

    auto &det = w.ammo.entries[kAmmoDetonator];
    det.name = "AMMO_DETONATOR";
    det.valid = true;
    det.flags = kDetonateSatchels;
    det.velocity = 0;

    auto &clay = w.ammo.entries[kAmmoClaymore];
    clay.name = "claymore";
    clay.valid = true;
    clay.flags = kUseOwnMove | kNoAge | kForceTracer;
    clay.velocity = 2;
    clay.max_age_ticks = 62;
    clay.drag_fp16 = 0x10000;
    clay.kztype = ammo_kz::kC4;
    clay.kz_minradius = 15.0f;
    clay.kz_maxradius = 30.0f;
    clay.kz_pieslice_bam = 12 * 11930464; // authored 24 -> half-angle 12 deg
    clay.tracer_item_friendly = kItemClaymore;
    clay.tracer_item_enemy = kItemClaymore;

    auto &claykz = w.ammo.entries[kAmmoClayKz];
    claykz.name = "claymorekillzone";
    claykz.valid = true;
    claykz.flags = kInstantKillzone;
    claykz.velocity = 2;
    claykz.kztype = ammo_kz::kC4;
    claykz.kz_damage = 205;
    claykz.kz_minradius = 15.0f;
    claykz.kz_maxradius = 30.0f;
    claykz.kz_pieslice_bam = 12 * 11930464;

    auto &shrap = w.ammo.entries[kAmmoClayShrap];
    shrap.name = "claymoreshrapnel";
    shrap.valid = true;
    shrap.flags = kClaymoreFan;
    shrap.velocity = 388;
    shrap.max_age_ticks = 31; // 0.5 s
    shrap.spread_count = 16;
    shrap.kz_pieslice_bam = 24 * 11930464; // authored 48
    shrap.min_damage = 55;
    shrap.max_damage = 55;

    auto &avmine = w.ammo.entries[kAmmoAvMine];
    avmine.name = "AV_Mine";
    avmine.valid = true;
    avmine.flags = kUseOwnMove | kNoAge | kForceTracer;
    avmine.velocity = 6;
    avmine.max_age_ticks = 62;
    avmine.drag_fp16 = 0x10000;
    avmine.kz_minradius = 5.0f;
    avmine.kz_maxradius = 5.0f;
    avmine.kz_pieslice_bam = 0; // retail data authors none — proximity is dead
    avmine.tracer_item_friendly = kItemAvMine;
    avmine.tracer_item_enemy = kItemAvMine;

    auto &avkz = w.ammo.entries[kAmmoAvMineKz];
    avkz.name = "AV_Minekillzone";
    avkz.valid = true;
    avkz.flags = kInstantKillzone;
    avkz.velocity = 2;
    avkz.kztype = ammo_kz::kC4;
    avkz.kz_damage = 30000;
    avkz.kz_minradius = 5.0f;
    avkz.kz_maxradius = 15.0f;
}

// The items.def ai_function/move_function bindings (the host feed).
void seed_classes(World &w) {
    w.throwables.classes.set({kItemFrag, ThrowClass::kNade, ThrowClass::kNade, 5, 0, 0});
    w.throwables.classes.set({kItemSatchel, ThrowClass::kSatchel, ThrowClass::kSatchel, 5, 0, 0});
    w.throwables.classes.set({kItemClaymore, ThrowClass::kClaymore, ThrowClass::kClaymore, 5, 0, 0});
    // the AT mine thinks as a mine but flies as a satchel (items.def: ai vmne,
    // move schl)
    w.throwables.classes.set({kItemAvMine, ThrowClass::kAVMine, ThrowClass::kSatchel, 5, 0, 0});
}

struct Rig {
    std::unique_ptr<World> w_heap;
    World &w;
    FlatField flat;
    EntityHandle thrower;

    explicit Rig(uint16_t ground_raw16 = 0)
            : w_heap(std::make_unique<World>()), w(*w_heap), flat(ground_raw16) {
        seed_ammo(w);
        seed_classes(w);
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 16);
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.team = 0;
        seed.health = 100;
        seed.position = Vec3{50.0f, 50.0f, float(ground_raw16) / 256.0f};
        thrower = w.registry.spawn(0, seed);
    }

    int throw_ammo(int ammo, const Vec3 &from, int32_t yaw_bam, int32_t pitch_bam,
                   uint8_t charge = 0) {
        RoundSpawnParams p;
        p.owner = thrower;
        p.shooter_handle = thrower.packed;
        p.origin = from;
        p.dir_yaw_bam = yaw_bam;
        p.dir_pitch_bam = pitch_bam;
        p.ammo_index = ammo;
        p.charge = charge;
        return w.round_sim.spawn(w, p);
    }

    void tick(int n) {
        for (int i = 0; i < n; ++i) {
            w.round_sim.tick(w, &flat.field, nullptr);
            w.throwables.tick(w, nullptr, &flat.field);
        }
    }
};

// ---------------------------------------------------------------------------
// The PowerThrow charge curve [orig: @ 0x4e07e9 — tap < 31 ticks = 255 (full),
// then clamp((held - 31)/93, 0.1, 1.0) * 255].
// ---------------------------------------------------------------------------
void test_power_throw_charge() {
    CHECK(power_throw_charge_from_hold(0) == 255);
    CHECK(power_throw_charge_from_hold(30) == 255);
    CHECK(power_throw_charge_from_hold(31) == 25);      // the 0.1 floor
    CHECK(power_throw_charge_from_hold(31 + 46) == 126); // 46/93 * 255
    CHECK(power_throw_charge_from_hold(31 + 93) == 255); // full at ~2.5 s
    CHECK(power_throw_charge_from_hold(1000) == 255);
}

// The charge byte scales the spawn speed for 1..254; 0/255 full
// [orig: @ 0x4ec5bb].
void test_charge_scales_spawn_speed() {
    Rig rig;
    const int full = rig.throw_ammo(kAmmoGrenade, Vec3{10, 10, 5}, 0, 0, 0);
    const int half = rig.throw_ammo(kAmmoGrenade, Vec3{10, 10, 5}, 0, 0, 128);
    CHECK(full >= 0 && half >= 0);
    const LiveRound &rf = rig.w.round_sim.rounds[size_t(full)];
    const LiveRound &rh = rig.w.round_sim.rounds[size_t(half)];
    const double vf = std::sqrt(double(rf.vel.x) * rf.vel.x + double(rf.vel.y) * rf.vel.y);
    const double vh = std::sqrt(double(rh.vel.x) * rh.vel.x + double(rh.vel.y) * rh.vel.y);
    CHECK(std::fabs(vf - 30.0 / 62.0) < 1e-3);
    CHECK(std::fabs(vh - (30.0 / 62.0) * (128.0 / 256.0)) < 1e-3);
    // the class/model bind rode the spawn
    CHECK(rf.item_type_id == kItemFrag);
    CHECK(rf.motor == ThrowClass::kNade);
    CHECK(rf.spin_yaw == 11930464); // 1 deg/tick init
}

// The grenade motor: gravity arc, terrain bounce (velZ * -0.2 + spin kicks),
// and the fuse queuing the kill zone at expiry [orig: @ 0x443F50 + the 0x1000
// expiry head].
void test_grenade_bounce_and_fuse() {
    Rig rig(0); // ground at 0
    rig.w.ammo.entries[kAmmoGrenade].max_age_ticks = 150; // short fuse for the test
    const int slot = rig.throw_ammo(kAmmoGrenade, Vec3{10, 10, 2}, 0, 0);
    CHECK(slot >= 0);
    // fly until the first ground contact (~40 ticks of -167 Q16 gravity from 2 u)
    rig.tick(80);
    const LiveRound &r = rig.w.round_sim.rounds[size_t(slot)];
    CHECK(r.active);
    CHECK(r.bounce_count >= 1);        // it bounced
    CHECK(r.pos.z >= -0.01f);          // clamped at the ground
    // the fuse: run past max_age — the kill zone queues exactly once
    rig.tick(120);
    CHECK(!rig.w.round_sim.rounds[size_t(slot)].active);
    CHECK(rig.w.explosions.queue.size() == 1);
    CHECK(rig.w.explosions.queue[0].ammo_index == kAmmoGrenade);
    // the detonation obj-row effect (tag 4) landed for the present pass
    bool saw_obj = false;
    for (const RoundImpact &imp : rig.w.round_sim.impacts)
        if (imp.effect_tag == 4 && imp.ammo_index == kAmmoGrenade) saw_obj = true;
    CHECK(saw_obj);
}

// A ballistic (non-motor) explosive round expiring mid-air vanishes silently
// [orig: the expiry head requires the motor-armed 0x1000 flag].
void test_ballistic_expiry_is_silent() {
    Rig rig(0);
    auto &m203 = rig.w.ammo.entries[kAmmoGrenade];
    m203.flags = 0; // plain ballistic kz round
    m203.tracer_item_friendly = 0;
    m203.tracer_item_enemy = 0;
    m203.max_age_ticks = 5;
    const int slot = rig.throw_ammo(kAmmoGrenade, Vec3{10, 10, 50}, 0, 0);
    CHECK(slot >= 0);
    rig.tick(10);
    CHECK(!rig.w.round_sim.rounds[size_t(slot)].active);
    CHECK(rig.w.explosions.queue.empty());
}

// The satchel: plops onto terrain, converts into a pool-1 device entity with
// the arm delay, and the source round releases [orig: @ 0x4482A0 rest leg ->
// Entity_ConvertRoundToPlacedEntity @ 0x5455B0].
void test_satchel_places_device() {
    Rig rig(0);
    const int slot = rig.throw_ammo(kAmmoSatchel, Vec3{20, 20, 1.5f}, 0, 0);
    CHECK(slot >= 0);
    rig.tick(120); // the -55 Q16 gravity drop takes ~60 ticks from 1.5 u
    CHECK(!rig.w.round_sim.rounds[size_t(slot)].active); // converted
    CHECK(rig.w.throwables.devices.size() == 1);
    if (rig.w.throwables.devices.empty()) return;
    const PlacedDevice &d = rig.w.throwables.devices[0];
    CHECK(d.think == ThrowClass::kSatchel);
    CHECK(d.entity.pool() == 1);
    CHECK(d.owner.packed == rig.thrower.packed);
    CHECK(d.think_delay_ticks > 0); // still arming
    const Entity *e = rig.w.registry.get(d.entity);
    CHECK(e != nullptr);
    CHECK(e->item_id == kItemSatchel);
    CHECK(e->health == 5); // the class row hp
    CHECK(rig.w.throwables.events.spawns.size() == 1);
    CHECK(rig.w.throwables.events.spawns[0].item_enemy == 0); // foe id absent
}

// The detonator: firing it marks the owner's satchels Health = -1; the armed
// think then detonates through satchelboom's instantkillzone
// [orig: @ 0x4ec234 -> @ 0x546ed0; think @ 0x443670].
void test_detonator_chain() {
    Rig rig(0);
    rig.throw_ammo(kAmmoSatchel, Vec3{20, 20, 1.5f}, 0, 0);
    rig.tick(120);
    CHECK(rig.w.throwables.devices.size() == 1);
    if (rig.w.throwables.devices.empty()) return;
    // still arming: the detonator marks it, the think waits for the arm delay
    rig.throw_ammo(kAmmoDetonator, Vec3{50, 50, 1}, 0, 0);
    const Entity *e = rig.w.registry.get(rig.w.throwables.devices[0].entity);
    CHECK(e != nullptr && e->health == -1);
    rig.tick(80); // arm delay expires -> the think sees health <= 0
    CHECK(rig.w.throwables.devices.empty());
    bool saw_boom = false;
    for (const ExplosionEntry &q : rig.w.explosions.queue)
        if (q.ammo_index == kAmmoSatchelBoom) saw_boom = true;
    CHECK(saw_boom);
    CHECK(rig.w.throwables.events.removes.size() == 1);
}

// Placed-claymore proximity: an enemy inside kz_minradius AND the pieslice
// cone (with LOS) trips it — the shrapnel fan + the kill zone spawn with the
// owner credited [orig: think @ 0x4438C0 + Entity_FindEnemyInCone @ 0x43cba0].
void test_claymore_cone_trigger() {
    Rig rig(0);
    // face +X: mission bearing 0
    const int slot = rig.throw_ammo(kAmmoClaymore, Vec3{20.0f, 20.0f, 0.8f}, 0, 0);
    CHECK(slot >= 0);
    rig.tick(160); // drop + convert + the 62-tick arm delay
    CHECK(rig.w.throwables.devices.size() == 1);
    if (rig.w.throwables.devices.empty()) return;
    // The cone axis is the device's LANDED facing — the witnessed 1 deg/tick
    // init spin rotates the claymore during its drop [orig: the clym motor's
    // Yaw -= spin @ 0x447377 over Entity_InitThrowableSpin_* rates], so the
    // test derives placement from the device yaw rather than the throw yaw.
    const PlacedDevice &placed = rig.w.throwables.devices[0];
    const double axis = -double(placed.yaw_bam) * (2.0 * 3.14159265358979323846 / 4294967296.0);
    const Vec3 ahead{placed.pos.x + 8.0f * float(std::cos(axis)),
                     placed.pos.y + 8.0f * float(std::sin(axis)), 0.0f};
    const Vec3 rear{placed.pos.x - 8.0f * float(std::cos(axis)),
                    placed.pos.y - 8.0f * float(std::sin(axis)), 0.0f};

    // behind the claymore: no trigger
    Entity enemy_seed;
    enemy_seed.kind = EntityKind::Organic;
    enemy_seed.team = 1;
    enemy_seed.health = 100;
    enemy_seed.position = rear;
    const EntityHandle behind = rig.w.registry.spawn(0, enemy_seed);
    rig.tick(4);
    CHECK(rig.w.throwables.devices.size() == 1);
    rig.w.registry.despawn(behind);

    // same team in front: no trigger (TeamTriggerClaymore off)
    enemy_seed.team = 0;
    enemy_seed.position = ahead;
    const EntityHandle mate = rig.w.registry.spawn(0, enemy_seed);
    rig.tick(4);
    CHECK(rig.w.throwables.devices.size() == 1);
    rig.w.registry.despawn(mate);

    // an enemy dead ahead inside the cone: boom
    enemy_seed.team = 1;
    const EntityHandle enemy = rig.w.registry.spawn(0, enemy_seed);
    rig.tick(4);
    CHECK(rig.w.throwables.devices.empty());
    bool saw_kz = false;
    for (const ExplosionEntry &q : rig.w.explosions.queue)
        if (q.ammo_index == kAmmoClayKz) saw_kz = true;
    CHECK(saw_kz);
    // the 16-pellet fan flew (ballistic rounds owned by the placer)
    int pellets = 0;
    for (const LiveRound &r : rig.w.round_sim.rounds)
        if (r.active && r.ammo_index == kAmmoClayShrap) ++pellets;
    CHECK(pellets == 16);
    for (const LiveRound &r : rig.w.round_sim.rounds)
        if (r.active && r.ammo_index == kAmmoClayShrap)
            CHECK(r.owner.packed == rig.thrower.packed);
    (void)enemy;
}

// AV mine: the retail data authors no kz_pieslice, so the vehicle cone can
// never pass — only damage detonates it [orig: think @ 0x443BB0; the 0-angle
// gate in Entity_FindEnemyVehicleInCone @ 0x43c9f0].
void test_avmine_proximity_is_data_dead() {
    Rig rig(0);
    const int slot = rig.throw_ammo(kAmmoAvMine, Vec3{20.0f, 20.0f, 0.8f}, 0, 0);
    CHECK(slot >= 0);
    rig.tick(160);
    CHECK(rig.w.throwables.devices.size() == 1);
    if (rig.w.throwables.devices.empty()) return;
    CHECK(rig.w.throwables.devices[0].think == ThrowClass::kAVMine);

    Entity veh_seed;
    veh_seed.kind = EntityKind::Item;
    veh_seed.item_type = 1; // vehicle def kind
    veh_seed.team = 1;
    veh_seed.health = 500;
    veh_seed.position = Vec3{23.0f, 20.0f, 0.0f};
    veh_seed.veh.speed = 20000; // moving
    rig.w.registry.spawn(1, veh_seed);
    rig.tick(8);
    CHECK(rig.w.throwables.devices.size() == 1); // never trips

    // shooting it (health <= 0) detonates
    Entity *dev = rig.w.registry.get(rig.w.throwables.devices[0].entity);
    CHECK(dev != nullptr);
    dev->health = 0;
    rig.tick(2);
    CHECK(rig.w.throwables.devices.empty());
    bool saw_kz = false;
    for (const ExplosionEntry &q : rig.w.explosions.queue)
        if (q.ammo_index == kAmmoAvMineKz) saw_kz = true;
    CHECK(saw_kz);
}

// Owner death removes the devices silently — no detonation
// [orig: Server_ProcessPlayerDeath -> @ 0x546e00 -> @ 0x50a270].
void test_owner_death_removes_devices() {
    Rig rig(0);
    rig.throw_ammo(kAmmoSatchel, Vec3{20, 20, 1.5f}, 0, 0);
    rig.tick(160); // place + arm
    CHECK(rig.w.throwables.devices.size() == 1);
    Entity *owner = rig.w.registry.get(rig.thrower);
    CHECK(owner != nullptr);
    owner->health = 0;
    rig.tick(2);
    CHECK(rig.w.throwables.devices.empty());
    CHECK(rig.w.explosions.queue.empty()); // removed, never detonated
    CHECK(rig.w.throwables.events.removes.size() == 1);
}

// TeamTriggerClaymore host rule: same-team actors trip claymores when set
// [orig: dword_24D1E34 & 0x8000 in the cone gates].
void test_team_trigger_claymore_rule() {
    Rig rig(0);
    rig.w.throwables.team_trigger_claymore = true;
    rig.throw_ammo(kAmmoClaymore, Vec3{20.0f, 20.0f, 0.8f}, 0, 0);
    rig.tick(160);
    CHECK(rig.w.throwables.devices.size() == 1);
    if (rig.w.throwables.devices.empty()) return;
    const PlacedDevice &placed = rig.w.throwables.devices[0];
    const double axis = -double(placed.yaw_bam) * (2.0 * 3.14159265358979323846 / 4294967296.0);
    Entity mate_seed;
    mate_seed.kind = EntityKind::Organic;
    mate_seed.team = 0; // the owner's own team
    mate_seed.health = 100;
    mate_seed.position = Vec3{placed.pos.x + 8.0f * float(std::cos(axis)),
                              placed.pos.y + 8.0f * float(std::sin(axis)), 0.0f};
    rig.w.registry.spawn(0, mate_seed);
    rig.tick(4);
    CHECK(rig.w.throwables.devices.empty());
}

} // namespace

int main() {
    test_power_throw_charge();
    test_charge_scales_spawn_speed();
    test_grenade_bounce_and_fuse();
    test_ballistic_expiry_is_silent();
    test_satchel_places_device();
    test_detonator_chain();
    test_claymore_cone_trigger();
    test_avmine_proximity_is_data_dead();
    test_owner_death_removes_devices();
    test_team_trigger_claymore_rule();
    if (failures == 0) std::printf("throwables tests passed\n");
    return failures == 0 ? 0 : 1;
}
