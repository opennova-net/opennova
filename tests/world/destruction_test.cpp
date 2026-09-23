// Item destruction tests (world/destruction.h; docs/world/world-wac-ai-re.md
// §24): the explosion queue + AoE gates, the bullet armor gates, the
// destructible death chain (husk flags + events + the kz chain blast), the
// death-piece pool, and the dead-item settle. Driven by a manual World.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <base/crt/crt_rng.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/infantry.h>
#include <runtime/world/vehicle_collision_damage.h>
#include <runtime/world/world.h>

using namespace opennova::world;
using namespace opennova::crt;
using opennova::terrain::TerrainHeightField;

static int failures = 0;

// One entity-update step of an item row's pool: every pool-1 row's own visit
// (World::update_pool1_slot), or the pool-2/3 cohort walk.
// [orig: Entity_UpdatePool1Slot @0x4B8DD0; Entity_UpdateAllEntities @0x4C2244 /
//  @0x4C230C]
static void step_item_pool(World &w, int pool) {
    if (pool != 1) {
        tick_item_event_pool(w, pool);
        return;
    }
    TickContext ctx;
    ctx.world = &w;
    ctx.is_authority = true;
    ctx.logic_tick = w.logic_tick;
    for (size_t slot = 0; slot < w.registry.pool_capacity(1); ++slot)
        if (Entity *row = w.registry.get(EntityHandle::make(1, static_cast<int>(slot))))
            w.update_pool1_slot(*row, ctx);
}

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

void tick_dead_items(World &world, const TerrainHeightField *terrain,
                     float water_height, DestructionEvents &events) {
    for (int pool = 1; pool <= 2; ++pool) {
        world.registry.for_each_in_pool(pool, [&](const Entity &row) {
            tick_item_death_motion(world, *world.registry.get(row.handle), terrain, water_height, events);
        });
    }
}

// A minimal ammo table: [0] null, [1] a grenade-style kz round, then the two
// named landing blasts resolved by the retail post-death callbacks.
void seed_ammo(World &w) {
    w.tables.ammo.entries.resize(4);
    AmmoTableEntry &null_e = w.tables.ammo.entries[0];
    null_e.name = "AT_NULL";
    null_e.valid = true;
    AmmoTableEntry &kz = w.tables.ammo.entries[1];
    kz.name = "40mm_HE";
    kz.valid = true;
    kz.kztype = ammo_kz::kStandard;
    kz.kz_damage = 100;
    kz.kz_minradius = 1.0f;
    kz.kz_maxradius = 8.0f;
    kz.penetration_kz = 50;
    AmmoTableEntry &organic = w.tables.ammo.entries[2];
    organic.name = "kz_OrganicBlast";
    organic.valid = true;
    organic.kztype = ammo_kz::kStandard;
    organic.kz_damage = 40;
    organic.kz_minradius = 0.5f;
    organic.kz_maxradius = 5.0f;
    // The death blast spares M/D items (the chain-explosion control the ammo
    // authors via NoMItems/NoDItems) — organics only.
    organic.flags = kAmmoFlagNoMItems | kAmmoFlagNoDItems;
    AmmoTableEntry &mitem = w.tables.ammo.entries[3];
    mitem.name = "kz_MItemBlast";
    mitem.valid = true;
    mitem.kztype = ammo_kz::kStandard;
    mitem.kz_damage = 40;
    mitem.kz_minradius = 0.5f;
    mitem.kz_maxradius = 5.0f;
}

ItemDeathTraits barrel_traits() {
    ItemDeathTraits t;
    t.unit_type = 0;
    t.kz = 4.0f;
    t.armor_impact = 5;
    t.armor_blast = 10;
    t.has_husk = true;
    t.husk_model_loaded = true;
    t.husk_sub_part_count = 4;
    t.husk_sub_part_types[1] = 3; // CHUNK_M
    t.husk_sub_part_types[2] = 3;
    t.husk_sub_part_types[3] = 2; // CHUNK_S
    t.sound_death = "EXPLO_BARREL";
    t.particledeath = "Effect_LrgOrdExp";
    t.particlefire = "Effect_Fire";
	t.effect_banks[1].mask = 1;
	t.effect_banks[1].points.push_back({ {}, { 0, 0, 1 } });
	return t;
}

int32_t fixed16(double value) {
    return static_cast<int32_t>(value * 65536.0);
}

// A type-1 solid box whose origin lies on the bottom face. The blast LOS test
// starts inside this volume exactly as a live pool-1 victim does.
CollisionModel solid_box_model(double half_extent, double height) {
    CollisionModel model;
    auto add_plane = [&](int nx, int ny, int nz, double distance) {
        CollisionPlane plane;
        plane.nx = static_cast<int16_t>(nx);
        plane.ny = static_cast<int16_t>(ny);
        plane.nz = static_cast<int16_t>(nz);
        plane.dist = fixed16(distance);
        model.planes.push_back(plane);
    };
    add_plane(16384, 0, 0, -half_extent);
    add_plane(-16384, 0, 0, -half_extent);
    add_plane(0, 16384, 0, -half_extent);
    add_plane(0, -16384, 0, -half_extent);
    add_plane(0, 0, 16384, -height);
    add_plane(0, 0, -16384, 0.0);

    CollisionVolume volume;
    volume.type = 1;
    volume.min_x = fixed16(-half_extent);
    volume.max_x = fixed16(half_extent);
    volume.min_y = fixed16(-half_extent);
    volume.max_y = fixed16(half_extent);
    volume.min_z = 0;
    volume.max_z = fixed16(height);
    volume.plane_count = 6;
    model.volumes.push_back(volume);

    CollisionSection section;
    section.volume_count = 1;
    model.sections.push_back(section);
    return model;
}

// A one-section collision shell with an authored z range: the model-level
// floor (`CollisionModel::min[2]`) stands in for the retail collision block's
// +0x28 CMDL bbox z-lo that Entity_CalcSlopeForces reads off the picked model.
CollisionModel wreck_shell_model(double z_lo, double z_hi) {
    CollisionModel model;
    CollisionSection section;
    section.min_x = fixed16(-1.0);
    section.max_x = fixed16(1.0);
    section.min_y = fixed16(-1.0);
    section.max_y = fixed16(1.0);
    section.min_z = fixed16(z_lo);
    section.max_z = fixed16(z_hi);
    section.radius = fixed16(2.0);
    section.authored_bounds = true;
    model.sections.push_back(section);
    return model;
}

// Bind an intact shell plus a husk-stage shell to one entity, the pair retail
// keeps at entity+0x30/+0x34 and picks between by Flags & 4.
void attach_wreck_shells(World &w, CollisionWorld &collision, EntityHandle h,
                         double graphic_z_lo, double husk_z_lo) {
    collision.assign_entity(h,
            collision.add_model(wreck_shell_model(graphic_z_lo, 2.0)));
    collision.assign_entity_husk(h,
            collision.add_model(wreck_shell_model(husk_z_lo, 1.0)));
    w.collision = &collision;
}

// One authored CFAC face for projectile fixtures. Pool-1 items reach this
// polygon walk only after their model is initialized.
// [orig: Entity_InitFromModel @0x40DC30;
// Physics_RaycastAgainstBoneCollision @0x4E4CB0]
CollisionModel projectile_wall_model(double half_extent) {
    CollisionModel model;
    auto vertex = [&](double y, double z) {
        CollisionVertex value;
        value.p[1] = fixed16(y);
        value.p[2] = fixed16(z);
        model.vertices.push_back(value);
    };
    vertex(-half_extent, -half_extent);
    vertex(half_extent, -half_extent);
    vertex(0.0, half_extent);

    CollisionNormal normal;
    normal.n[0] = -16384;
    normal.dominant_axis = 4;
    model.normals.push_back(normal);

    CollisionFace face;
    face.vertex_index[0] = 0;
    face.vertex_index[1] = 1;
    face.vertex_index[2] = 2;
    face.normal_index = 0;
    face.min[1] = face.min[2] = fixed16(-half_extent);
    face.max[1] = face.max[2] = fixed16(half_extent);
    model.faces.push_back(face);

    CollisionSection section;
    section.vertex_count = 3;
    section.normal_count = 1;
    section.face_count = 1;
    model.sections.push_back(section);
    return model;
}

CollisionModel section_debris_model() {
    CollisionModel model;
    CollisionSection first;
    first.face_start = 0;
    first.face_count = 1;
    first.face_vertex_start = 0;
    first.face_vertex_count = 3;
    model.sections.push_back(first);
    CollisionSection second;
    second.face_start = 1;
    second.face_count = 149;
    second.face_vertex_start = 3;
    second.face_vertex_count = 6;
    model.sections.push_back(second);
    model.face_vertices = {
        CollisionFaceVertex{-1, 0, 0},
        CollisionFaceVertex{0, 0, 0},
        CollisionFaceVertex{0, 0, 0},
        CollisionFaceVertex{0, 0, 0},
        CollisionFaceVertex{768, 0, 0},
        CollisionFaceVertex{0, 768, 0},
        CollisionFaceVertex{0, 0, 0},
        CollisionFaceVertex{1536, 0, 0},
        CollisionFaceVertex{0, 0, 0},
    };
    model.faces.resize(150);
    for (size_t i = 0; i < model.faces.size(); ++i) {
        CollisionFace &face = model.faces[i];
        const int16_t base = i < 2 ? 0 : 3;
        face.vertex_index[0] = base;
        face.vertex_index[1] = static_cast<int16_t>(base + 1);
        face.vertex_index[2] = static_cast<int16_t>(base + 2);
        face.material = i == 0 ? 17 : 4;
    }
    return model;
}

} // namespace

// The AoE damage applicator gates [orig: Entity_ApplyWeaponDamage @0x4e6820].
void test_explosion_damage_gates() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 8);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 500;
    seed.health = 120;
    seed.health_max = 120;
    seed.position = Vec3{10.0f, 0.0f, 0.0f};
    seed.bound_radius = 1.0f;
    const EntityHandle barrel = w.registry.spawn(1, seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    // In blast range, armor passed (penetration_kz 50 >= armor_blast 10):
    // full kz_damage at the center band, linear falloff outside kz_minradius.
    ExplosionEntry e;
    e.pos = Vec3{10.0f, 0.0f, 0.0f}; // dead center — surface distance 0
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    Entity *b = w.registry.get(barrel);
    CHECK(b != nullptr);
    CHECK(b->health == 20); // 120 - 100 (no falloff inside kz_minradius)
    CHECK(w.out.destruction.explosions_processed == 1);

    // The falloff is 16.16 with a truncating quotient and a rounded multiply
    // [orig: @0x4e6943-0x4e699c]: surface 3.5 u of the 1..8 band is t16 = 23405
    // (0.357132), so 77 -> (77 * 42131 + 0x8000) >> 16 = 50 where the float form
    // 77 * (1 - 0.35714287) + 0.5 truncates to 49.
    b->health = 300;
    w.tables.ammo.entries[1].kz_damage = 77;
    ExplosionEntry far = e;
    far.pos = Vec3{5.5f, 0.0f, 0.0f}; // center distance 4.5, bound 1.0 -> surface 3.5
    w.explosions.queue_explosion(w, far);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(barrel)->health == 300 - 50);
    w.tables.ammo.entries[1].kz_damage = 100;

    // Blast armor gate: an ammo whose penetration_kz is below the armor word
    // zeroes its damage [orig: @0x4e69b0] -- but the non-person tail runs for
    // 0 damage too, so a 0-damage blast still rebinds the attacker
    // [orig: the tail gates @0x4E6EFD / @0x4E6F0A, the +0x178 store @0x4E6F18].
    b->health = 120;
    b->last_attacker = EntityHandle{};
    ItemDeathTraits armored = barrel_traits();
    armored.armor_blast = 60; // > penetration_kz 50
    w.tables.item_death_traits.set(500, armored);
    Entity attacker_seed;
    attacker_seed.kind = EntityKind::Organic;
    attacker_seed.health = 100;
    attacker_seed.position = Vec3{50.0f, 0.0f, 0.0f};
    w.registry.configure_pool(0, 4);
    const EntityHandle attacker = w.registry.spawn(0, attacker_seed);
    e.owner = attacker;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(barrel)->health == 120);
    CHECK(w.registry.get(barrel)->last_attacker == attacker);
    e.owner = EntityHandle{};

    // Invulnerable word (-1 = 0xFFFF) [orig: @0x4e68aa].
    armored.armor_blast = -1;
    w.tables.item_death_traits.set(500, armored);
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(barrel)->health == 120);

    // The NoMItems ammo flag skips the whole pool-1 sweep [orig: & 0x100000
    // @0x4eb378].
    w.tables.item_death_traits.set(500, barrel_traits());
    w.tables.ammo.entries[1].flags = kAmmoFlagNoMItems;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(barrel)->health == 120);
    w.tables.ammo.entries[1].flags = 0;
}

// `destroybuild` gates only Building targets in a network session. Offline
// damage ignores the setting, and enabled multiplayer uses the ordinary blast
// path. No other retail consumer reads the option.
// [orig: Entity_ApplyWeaponDamage @0x4E682E..0x4E6860]
void test_multiplayer_destroy_buildings_rule() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(2, 2);

    Entity seed;
    seed.kind = EntityKind::Building;
    seed.item_id = 501;
    seed.health = 120;
    seed.health_max = 120;
    seed.position = Vec3{10.0f, 0.0f, 0.0f};
    seed.bound_radius = 1.0f;
    const EntityHandle building = w.registry.spawn(2, seed);
    w.tables.item_death_traits.set(501, barrel_traits());

    ExplosionEntry blast;
    blast.pos = seed.position;
    blast.type = ammo_kz::kStandard;
    blast.ammo_index = 1;
    auto apply_blast = [&] {
        w.explosions.queue_explosion(w, blast);
        w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    };

    w.rules.mp_session = true;
    w.rules.destroy_buildings = false;
    apply_blast();
    CHECK(w.registry.get(building)->health == 120);

    w.rules.destroy_buildings = true;
    apply_blast();
    CHECK(w.registry.get(building)->health == 20);

    w.registry.get(building)->health = 120;
    w.rules.mp_session = false;
    w.rules.destroy_buildings = false;
    apply_blast();
    CHECK(w.registry.get(building)->health == 20);
}

// The pool-1 LOS ray starts at the victim position, inside its own collision
// hull. Retail excludes the endpoint entity from the sector walk; otherwise
// every collision-wired movable item shields itself from blast damage.
void test_explosion_los_excludes_victim_hull() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 500;
    seed.has_item_def = true;
    seed.health = 120;
    seed.health_max = 120;
    seed.position = Vec3{4.0f, 0.0f, 0.0f};
    seed.bound_radius = 1.0f;
    const EntityHandle victim = w.registry.spawn(1, seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    CollisionWorld collision;
    const int32_t model_id = collision.add_model(solid_box_model(1.0, 2.0));
    collision.assign_entity(victim, model_id);
    collision.build_initial_tables(w);

    ExplosionEntry e;
    e.pos = Vec3{0.0f, 0.0f, 0.0f};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(victim)->health < 120);
}

// Pool 1's type-4/direct-hit walk bypasses LOS entirely. A solid wall blocks a
// standard blast, but cannot shield the directly hit movable item.
void test_radius_blast_skips_pool1_los() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);
    w.registry.configure_pool(2, 4);

    Entity victim_seed;
    victim_seed.kind = EntityKind::Item;
    victim_seed.item_id = 500;
    victim_seed.has_item_def = true;
    victim_seed.health = 120;
    victim_seed.health_max = 120;
    victim_seed.position = Vec3{4.0f, 0.0f, 0.5f};
    victim_seed.bound_radius = 1.0f;
    const EntityHandle victim = w.registry.spawn(1, victim_seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    Entity wall_seed;
    wall_seed.kind = EntityKind::Building;
    wall_seed.item_id = 501;
    wall_seed.position = Vec3{2.0f, 0.0f, 0.0f};
    wall_seed.bound_radius = 1.0f;
    const EntityHandle wall = w.registry.spawn(2, wall_seed);
    CollisionWorld collision;
    collision.assign_entity(wall, collision.add_model(solid_box_model(0.5, 2.0)));

    collision.build_initial_tables(w); // blast LOS consumes the victim candidate slice

    ExplosionEntry e;
    e.pos = Vec3{0.0f, 0.0f, 0.5f};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(victim)->health == 120);

    e.type = ammo_kz::kRadiusBlast;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(victim)->health < 120);
}

// Only the movable-item LOS walk lifts both endpoints by 0.25 units. Pool 0
// uses the authored positions. The -0.25 clip radius shrinks the cover to
// Z [0.25, 0.5]: the 0.35 ray blocks, while an incorrect +0.25 lift would miss.
void test_organic_blast_los_is_unlifted() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(2, 4);

    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.item_id = 502;
    victim_seed.health = 120;
    victim_seed.health_max = 120;
    victim_seed.position = Vec3{4.0f, 0.0f, 0.35f};
    victim_seed.bound_radius = 0.6f;
    const EntityHandle victim = w.registry.spawn(0, victim_seed);

    Entity wall_seed;
    wall_seed.kind = EntityKind::Building;
    wall_seed.item_id = 501;
    wall_seed.position = Vec3{2.0f, 0.0f, 0.0f};
    wall_seed.bound_radius = 1.0f;
    const EntityHandle wall = w.registry.spawn(2, wall_seed);
    CollisionWorld collision;
    collision.assign_entity(wall, collision.add_model(solid_box_model(0.5, 0.75)));

    collision.build_initial_tables(w); // blast LOS consumes the victim candidate slice

    ExplosionEntry e;
    e.pos = Vec3{0.0f, 0.0f, 0.35f};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(victim)->health == 120);
}

// The cone is a wrapped BAM interval around the entry direction. The exact
// opposite direction must be rejected, while targets one degree across either
// side of the +/-pi seam remain inside a five-degree cone.
void test_explosion_cone_wrap() {
    auto health_after = [](double direction_deg, double target_deg) {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        seed_ammo(w);
        w.registry.configure_pool(1, 4);
        w.tables.ammo.entries[1].kz_pieslice_bam =
                static_cast<int32_t>(5.0 * kBamPerDegree);

        const double radians = target_deg * 3.14159265358979323846 / 180.0;
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.item_id = 500;
        seed.health = 120;
        seed.health_max = 120;
        seed.position = Vec3{static_cast<float>(4.0 * std::cos(radians)),
                             static_cast<float>(4.0 * std::sin(radians)), 0.0f};
        seed.bound_radius = 1.0f;
        const EntityHandle victim = w.registry.spawn(1, seed);
        w.tables.item_death_traits.set(500, barrel_traits());

        ExplosionEntry e;
        e.pos = Vec3{};
        e.dir_bam = bam_from_degrees_wrapped(direction_deg);
        e.type = ammo_kz::kStandard;
        e.ammo_index = 1;
        w.explosions.queue_explosion(w, e);
        w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
        return w.registry.get(victim)->health;
    };

    CHECK(health_after(0.0, 0.0) < 120);
    CHECK(health_after(0.0, 180.0) == 120);
    CHECK(health_after(179.0, -179.0) < 120);
    CHECK(health_after(-179.0, 179.0) < 120);
}

// Kill credit resolves a dead intermediary's last-attacker chain before the
// pool sweeps. Every emitted hit/death record must use that resolved owner,
// for both organic and non-person victims.
void test_explosion_resolves_attacker_chain_for_events() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(0, 8);
    w.registry.configure_pool(1, 4);

    Entity live_seed;
    live_seed.kind = EntityKind::Organic;
    live_seed.health = 100;
    live_seed.position = Vec3{100.0f, 0.0f, 0.0f};
    const EntityHandle live_attacker = w.registry.spawn(0, live_seed);

    Entity dead_seed = live_seed;
    dead_seed.health = 0;
    dead_seed.engine_flags |= kEntityFlagDead;
    dead_seed.last_attacker = live_attacker;
    const EntityHandle dead_owner = w.registry.spawn(0, dead_seed);

    Entity organic_seed;
    organic_seed.kind = EntityKind::Organic;
    organic_seed.health = 40;
    organic_seed.health_max = 40;
    organic_seed.position = Vec3{};
    organic_seed.bound_radius = 0.6f;
    const EntityHandle organic = w.registry.spawn(0, organic_seed);

    Entity item_seed;
    item_seed.kind = EntityKind::Item;
    item_seed.item_id = 500;
    item_seed.health = 40;
    item_seed.health_max = 40;
    item_seed.position = Vec3{1.0f, 0.0f, 0.0f};
    item_seed.bound_radius = 1.0f;
    const EntityHandle item = w.registry.spawn(1, item_seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    e.owner = dead_owner;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);

    bool organic_hit_resolved = false;
    bool organic_death_resolved = false;
    bool item_death_resolved = false;
    for (const RoundHit &hit : w.round_sim.hits)
        if (hit.victim == organic && hit.shooter == live_attacker)
            organic_hit_resolved = true;
    // Both lethal blast edges run the kill accounting
    // [orig: Entity_ApplyWeaponDamage @0x4E6820 (the Score_ProcessKillEvent
    // calls @0x4E6BFE person, @0x4E6FB4 item)].
    for (const RoundDeath &death : w.round_sim.deaths) {
        if (death.victim == organic && death.killer == live_attacker && death.kill_event)
            organic_death_resolved = true;
        if (death.victim == item && death.killer == live_attacker && death.kill_event)
            item_death_resolved = true;
    }
    CHECK(organic_hit_resolved);
    CHECK(organic_death_resolved);
    CHECK(item_death_resolved);
}

// The destructible death chain: health 0 -> Flags|=6 + events + the kz chain
// blast [orig: Entity_HandleDestructibleDeathEvent @0x440210 ->
// Entity_ProcessDestructibleDeath @0x43fbc0 + Entity_InitDeathSounds @0x4939b0].
void test_destructible_death_chain() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 8);

    Entity organic_seed;
    organic_seed.kind = EntityKind::Organic;
    organic_seed.health = 100;
    organic_seed.position = Vec3{12.0f, 0.0f, 0.0f}; // 2 u from the barrel
    const EntityHandle bystander = w.registry.spawn(0, organic_seed);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 500;
    seed.health = 30;
    seed.position = Vec3{10.0f, 0.0f, 0.0f};
    seed.bound_radius = 1.0f;
    const EntityHandle barrel = w.registry.spawn(1, seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    // A bullet-gated hit that kills: the gate clamps to remaining health
    // [orig: @0x4e8064] and the notify destroys on <= 0.
    Entity *b = w.registry.get(barrel);
    int32_t dmg = item_bullet_damage_gate(w, *b, 50, /*penetration_impact=*/10);
    CHECK(dmg == 30);
    b->health -= dmg;
    destruction_notify_item_damage(w, *b, 1);
    CHECK((b->engine_flags & kEntityFlagDead) != 0);
    CHECK((b->engine_flags & kEntityFlagHusk) != 0);
    CHECK(!b->alive);
    CHECK(w.out.destruction.items_destroyed == 1);
    CHECK(w.out.destruction.husk_swaps.size() == 1);
    CHECK(w.out.destruction.debris_triangles == 0);
    bool found_death_sound = false;
    for (const DestructionSoundEvent &s : w.out.destruction.sounds)
        if (s.sound == "EXPLO_BARREL") found_death_sound = true;
    CHECK(found_death_sound);
    bool found_death_fx = false;
    bool found_fire_fx = false;
    for (const DestructionEffectEvent &fx : w.out.destruction.effects) {
        if (fx.effect == "Effect_LrgOrdExp" && fx.family == 1) found_death_fx = true;
        if (fx.effect == "Effect_Fire" && fx.family == 2) found_fire_fx = true;
    }
    CHECK(found_death_fx);
    CHECK(found_fire_fx);
    // A second notify never re-husks [orig: the Flags&4 resend leg @0x440272].
    destruction_notify_item_damage(w, *b, 2);
    CHECK(w.out.destruction.items_destroyed == 1);

    // The death queued a kz_OrganicBlast (r = def kz 4.0) — the chain blast.
    CHECK(w.explosions.queue.size() == 1);
    CHECK(w.explosions.queue[0].ammo_index == 2);
    CHECK(w.explosions.queue[0].radius_override == 4.0f);
    // Draining it hurts the 2 u bystander but NOT another item (NoMItems).
    Entity item2_seed;
    item2_seed.kind = EntityKind::Item;
    item2_seed.item_id = 500;
    item2_seed.health = 100;
    item2_seed.position = Vec3{11.5f, 0.0f, 0.0f};
    item2_seed.bound_radius = 1.0f;
    const EntityHandle item2 = w.registry.spawn(1, item2_seed);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    const Entity *by = w.registry.get(bystander);
    CHECK(by->health < 100); // organics in range take the death blast
    CHECK(w.registry.get(item2)->health == 100); // M-items are spared by the flag
    CHECK(!w.round_sim.hits.empty()); // the AI reaction stamp fed
}

// Section debris is resolved in the simulation from the intact collision
// triangles before the husk flag changes the active model. A 150-face model
// yields stride 256, so each face appears exactly once. The first centroid's
// signed -1/3 Q8 coordinate truncates toward zero.
// [orig: Entity_SpawnSectionDebris @0x43f580]
void test_section_debris_samples_collision_faces() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(1, 4);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 500;
    seed.health = 0;
    seed.position = Vec3{10.0f, 20.0f, 30.0f};
    seed.yaw = 90.0f; // identity mission placement rotation
    seed.death_blast_center = Vec3{9.0f, 20.0f, 30.0f};
    const EntityHandle target = w.registry.spawn(1, seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    CollisionWorld collision;
    collision.assign_entity(target,
            collision.add_model(section_debris_model()));
    const int32_t entity_pos[3] = {
            to_fixed(seed.position.x), to_fixed(seed.position.y),
            to_fixed(seed.position.z)};
    const CollisionMatrix first_matrix =
            collision_matrix_from_heading(0, entity_pos);
    int32_t displaced_pos[3] = {
            entity_pos[0] + to_fixed(100.0f), entity_pos[1], entity_pos[2]};
    const CollisionMatrix second_matrix =
            collision_matrix_from_heading(0, displaced_pos);
    CHECK(collision.publish_entity_section_matrices(
            target, {first_matrix, second_matrix}));
    w.collision = &collision;
    process_destructible_death(w, *w.registry.get(target));

    std::vector<DestructionEffectEvent> debris;
    for (const DestructionEffectEvent &effect : w.out.destruction.effects)
        if (effect.effect == "Effect_TreeFoliageExp" ||
                effect.effect == "Effect_TreeWoodExp")
            debris.push_back(effect);
    CHECK(debris.size() == 150);
    CHECK(w.out.destruction.debris_triangles == 150);
    if (debris.size() >= 3) {
        CHECK(debris[0].effect == "Effect_TreeFoliageExp");
        CHECK(std::abs(debris[0].pos.x - 10.0f) < 1.0e-6f);
        CHECK(std::abs(debris[0].pos.y - 20.0f) < 1.0e-6f);
        CHECK(std::abs(debris[0].pos.z - 30.0f) < 1.0e-6f);
        CHECK(std::abs(debris[0].dir.x - 1.0f) < 1.0e-6f);
        CHECK(std::abs(debris[0].dir.y) < 1.0e-6f);
        CHECK(std::abs(debris[0].dir.z) < 1.0e-6f);
        CHECK(debris[1].effect == "Effect_TreeWoodExp");
        CHECK(std::abs(debris[1].pos.x - 11.0f) < 1.0e-6f);
        CHECK(std::abs(debris[1].pos.y - 21.0f) < 1.0e-6f);
        CHECK(std::abs(debris[1].pos.z - 30.0f) < 1.0e-6f);
        CHECK(std::abs(debris[1].dir.x - 0.8944272f) < 1.0e-5f);
        CHECK(std::abs(debris[1].dir.y - 0.4472136f) < 1.0e-5f);
        CHECK(std::abs(debris[2].pos.x - 12.0f) < 1.0e-6f);
        CHECK(std::abs(debris[2].pos.y - 20.0f) < 1.0e-6f);
    }
}

// Static-building glass is a model-authored point, not a coarse building
// burst. The point uses the entity's complete placement pose, tests against
// the AMMO'S authored maximum radius (not the queue override), breaks once,
// and consumes two retail PRNG draws for each of the four probabilistic effect
// slots. Seed 0x200 yields visible Glass/Paper/Dust rows and no Fire row.
// [orig: Projectile_ProcessExplosionQueue @0x4eb814-0x4eb84d;
// Terrain_SpawnEffectsAtUserPoint @0x5cee20]
void test_glass_userpoints_shatter_once_at_authored_radius() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(2, 4);

    ItemDeathTraits traits = barrel_traits();
    traits.glass_points = {
            GlassPointTrait{Vec3{1.5f, -0.5f, 0.75f}, Vec3{0.0f, 1.0f, 0.0f}},
            GlassPointTrait{Vec3{20.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}}};
    w.tables.item_death_traits.set(900, traits);

    Entity seed;
    seed.kind = EntityKind::Building;
    seed.item_id = 900;
    seed.health = 1000;
    seed.health_max = 1000;
    seed.position = Vec3{4.0f, 6.0f, 10.0f};
    seed.bound_radius = 2.0f;
    seed.yaw = 0.0f;
    seed.pitch = 90.0f;
    seed.roll = 90.0f;
    const EntityHandle building = w.registry.spawn(2, seed);

    // Rz(90-yaw)*Ry(-pitch)*Rx(roll) maps local (x,y,z) to
    // (z,-y,x): the first point lands at (4.75,6.5,11.5), and its local +Y
    // direction becomes world -Y.
    ExplosionEntry explosion;
    explosion.pos = Vec3{4.75f, 6.5f, 11.5f};
    explosion.type = ammo_kz::kStandard;
    explosion.ammo_index = 1;
    explosion.radius_override = 30.0f;
    w.destruction_rng.state = 0x200u;
    w.explosions.queue_explosion(w, explosion);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);

    CHECK(w.out.destruction.glass_points == 1);
    CHECK(w.registry.get(building)->broken_glass_point_bits.size() == 1);
    CHECK((w.registry.get(building)->broken_glass_point_bits[0] & 1u) != 0);
    CHECK((w.registry.get(building)->broken_glass_point_bits[0] & 2u) == 0);
    CHECK(w.out.destruction.effects.size() == 3);
    if (w.out.destruction.effects.size() == 3) {
        CHECK(w.out.destruction.effects[0].effect == "Effect_BldGlassExp");
        CHECK(w.out.destruction.effects[1].effect == "Effect_BldPaperExp");
        CHECK(w.out.destruction.effects[2].effect == "Effect_BldDustExp");
        for (const DestructionEffectEvent &effect : w.out.destruction.effects) {
            CHECK(std::abs(effect.pos.x - 4.75f) < 1.0e-4f);
            CHECK(std::abs(effect.pos.y - 6.5f) < 1.0e-4f);
            CHECK(std::abs(effect.pos.z - 11.5f) < 1.0e-4f);
            CHECK(std::abs(effect.dir.x) < 1.0e-4f);
            CHECK(std::abs(effect.dir.y + 1.0f) < 1.0e-4f);
            CHECK(std::abs(effect.dir.z) < 1.0e-4f);
        }
    }
    CHECK(w.destruction_rng.state == 0xd8651b5fu);

    // Replaying the same blast neither emits nor advances the stream: the
    // first exact point is already broken and the second remains outside the
    // authored kz_maxradius=8 despite radius_override=30.
    w.out.destruction.effects.clear();
    const uint32_t state_after_first = w.destruction_rng.state;
    w.explosions.queue_explosion(w, explosion);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.out.destruction.glass_points == 1);
    CHECK(w.out.destruction.effects.empty());
    CHECK(w.destruction_rng.state == state_after_first);
    CHECK((w.registry.get(building)->broken_glass_point_bits[0] & 2u) == 0);
}

void test_synthetic_husk_events_preserve_distinct_handles() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(1, 4);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 500;
    seed.bms_id = 0;
    seed.spawn_origin = 0xFFFFFFFFu;
    const EntityHandle first = w.registry.spawn(1, seed);
    const EntityHandle second = w.registry.spawn(1, seed);
    CHECK(first != second);
    ItemDeathTraits traits = barrel_traits();
    traits.particleother = "Effect_Smoke";
	traits.effect_banks[2].mask = 1;
	traits.effect_banks[2].points.push_back({ {}, { 0, 0, 1 } });
	w.tables.item_death_traits.set(500, traits);

	process_destructible_death(w, *w.registry.get(first));
    process_destructible_death(w, *w.registry.get(second));

    CHECK(w.out.destruction.husk_swaps.size() == 2);
    CHECK(w.out.destruction.husk_swaps[0].wire_handle == first.packed);
    CHECK(w.out.destruction.husk_swaps[1].wire_handle == second.packed);
    CHECK(w.out.destruction.husk_swaps[0].wire_handle !=
          w.out.destruction.husk_swaps[1].wire_handle);

    // The three attached banks must carry the same incarnation identity as
    // the husk event. Without it, both runtime-only children collapse onto
    // (net=0,bms=0,origin=sentinel) in the Godot presenter.
    CHECK(w.out.destruction.effects.size() == 6);
    for (size_t i = 0; i < w.out.destruction.effects.size(); ++i) {
        const DestructionEffectEvent &fx = w.out.destruction.effects[i];
        const EntityHandle expected = i < 3 ? first : second;
        CHECK(fx.family == static_cast<uint8_t>((i % 3) + 1));
        CHECK(fx.attach_net_id == 0);
        CHECK(fx.attach_bms_id == 0);
        CHECK(fx.attach_wire_handle == expected.packed);
        CHECK(fx.attach_spawn_origin == 0xFFFFFFFFu);
    }
}

// Retail transforms each husk-model KZ user point through the item's complete
// authored Euler pose before adding its world position:
// Rz(90-yaw) * Ry(-pitch) * Rx(roll).  These three +90 degree rotations make
// the expected result order- and sign-sensitive: (x,y,z) -> (z,-y,x).
void test_kz_point_full_euler() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 500;
    seed.health = 0;
    seed.position = Vec3{4.0f, 6.0f, 10.0f};
    seed.bound_radius = 1.0f;
    seed.yaw = 0;
    seed.pitch = 90;
    seed.roll = 90;
    const EntityHandle h = w.registry.spawn(1, seed);

    ItemDeathTraits traits = barrel_traits();
    traits.kz_points = {Vec3{1.5f, -0.5f, 0.75f}};
    w.tables.item_death_traits.set(500, traits);
    Entity *e = w.registry.get(h);
    destruction_notify_item_damage(w, *e, 1);

    CHECK(w.explosions.queue.size() == 1);
    if (w.explosions.queue.size() == 1) {
        const ExplosionEntry &blast = w.explosions.queue[0];
        const Vec3 &pos = blast.pos;
        CHECK(std::abs(pos.x - 4.75f) < 1.0e-4f);
        CHECK(std::abs(pos.y - 6.5f) < 1.0e-4f);
        CHECK(std::abs(pos.z - 11.5f) < 1.0e-4f);
        CHECK(std::abs(blast.radius_override - 5.0f) < 1.0e-6f);
    }
}

// UnitType 11 transforms every first-husk DEAD point through the complete
// authored pose, but retail pins the effect Z to the RAW water plane. These
// rotations map (x,y,z) -> (z,-y,x), making both axis order and signs visible.
void test_bridge_dead_points_emit_water_shocks() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(1, 8);
    w.env.water_z = fixed16(3.25);

    ItemDeathTraits bridge = barrel_traits();
    bridge.unit_type = 11;
    bridge.bridge_dead_points = {
            Vec3{1.5f, -0.5f, 0.75f}, Vec3{-2.0f, 1.0f, 0.25f}};
    w.tables.item_death_traits.set(800, bridge);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 800;
    seed.health = 0;
    seed.position = Vec3{4.0f, 6.0f, 10.0f};
    seed.bound_radius = 1.0f;
    seed.yaw = 0;
    seed.pitch = 90;
    seed.roll = 90;
    const EntityHandle h = w.registry.spawn(1, seed);
    entity_update_death_transforms(w, *w.registry.get(h), true);

    std::vector<DestructionEffectEvent> shocks;
    for (const DestructionEffectEvent &effect : w.out.destruction.effects)
        if (effect.effect == "Effect_ShockWaterBrdg") shocks.push_back(effect);
    CHECK(shocks.size() == 2);
    if (shocks.size() == 2) {
        CHECK(std::abs(shocks[0].pos.x - 4.75f) < 1.0e-4f);
        CHECK(std::abs(shocks[0].pos.y - 6.5f) < 1.0e-4f);
        CHECK(std::abs(shocks[0].pos.z - 3.25f) < 1.0e-6f);
        CHECK(std::abs(shocks[1].pos.x - 4.25f) < 1.0e-4f);
        CHECK(std::abs(shocks[1].pos.y - 5.0f) < 1.0e-4f);
        CHECK(std::abs(shocks[1].pos.z - 3.25f) < 1.0e-6f);
        CHECK(shocks[0].attach_net_id == 0);
        CHECK(shocks[0].family == 0);
    }

    // Zero is still the raw bridge plane, and no DEAD bank means no fallback.
    w.out.destruction.effects.clear();
    w.env.water_z = 0;
    ItemDeathTraits no_points = bridge;
    no_points.bridge_dead_points.clear();
    w.tables.item_death_traits.set(801, no_points);
    seed.item_id = 801;
    const EntityHandle empty_h = w.registry.spawn(1, seed);
    entity_update_death_transforms(w, *w.registry.get(empty_h), true);
    bool saw_empty_fallback = false;
    for (const DestructionEffectEvent &effect : w.out.destruction.effects)
        if (effect.effect == "Effect_ShockWaterBrdg") saw_empty_fallback = true;
    CHECK(!saw_empty_fallback);

    // The metadata is inert on every non-bridge dispatch row.
    w.out.destruction.effects.clear();
    ItemDeathTraits non_bridge = bridge;
    non_bridge.unit_type = 10;
    w.tables.item_death_traits.set(802, non_bridge);
    seed.item_id = 802;
    const EntityHandle other_h = w.registry.spawn(1, seed);
    entity_update_death_transforms(w, *w.registry.get(other_h), true);
    bool saw_other = false;
    for (const DestructionEffectEvent &effect : w.out.destruction.effects)
        if (effect.effect == "Effect_ShockWaterBrdg") saw_other = true;
    CHECK(!saw_other);

    // A bridge point at a raw zero water plane emits at z=0 exactly.
    w.out.destruction.effects.clear();
    w.tables.item_death_traits.set(803, bridge);
    seed.item_id = 803;
    const EntityHandle zero_h = w.registry.spawn(1, seed);
    entity_update_death_transforms(w, *w.registry.get(zero_h), true);
    CHECK(!w.out.destruction.effects.empty());
    if (!w.out.destruction.effects.empty())
        CHECK(std::abs(w.out.destruction.effects[0].pos.z) < 1.0e-6f);
}

// Death pieces: the per-section spawn distribution + the pool tick
// [orig: Entity_SpawnDeathPieces @0x493400 / Entity_ProcessDeathPiecePhysics
// @0x492dd0].
void test_death_pieces() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 700;
    seed.health = 0;
    seed.position = Vec3{0.0f, 0.0f, 10.0f};
    seed.bound_radius = 2.0f;
    const EntityHandle h = w.registry.spawn(1, seed);
    ItemDeathTraits t = barrel_traits();
    t.unit_type = 1; // vehicle-family dispatch row
    t.husk_sub_part_count = 4;
    w.tables.item_death_traits.set(700, t);

    Entity *e = w.registry.get(h);
    entity_update_death_transforms(w, *e, /*silent=*/false);
    CHECK((e->engine_flags & kEntityFlagHusk) != 0);
    // CHUNK_M/CHUNK_S rows have probability 1.0 -> sections 1..3 all spawn.
    CHECK(e->spawned_piece_mask == 0b1110u);
    int live = 0;
    for (const DeathPiece &p : w.death_pieces.pieces)
        if (p.active) ++live;
    CHECK(live == 3);
    // The hull (section 0) never leaves [orig: the loop starts at 1 @0x493606].
    CHECK((e->spawned_piece_mask & 1u) == 0);
    // Pieces fall under gravity with no terrain (never land, stay active).
    const float z_before = [&] {
        for (const DeathPiece &p : w.death_pieces.pieces)
            if (p.active) return p.pos.z;
        return 0.0f;
    }();
    for (int i = 0; i < 10; ++i)
        w.death_pieces.tick(w, nullptr, -1.0e9f, w.out.destruction);
    for (const DeathPiece &p : w.death_pieces.pieces)
        if (p.active) { CHECK(p.pos.z != z_before); break; }
}

void test_death_piece_ring_generation() {
    DeathPieceSim sim;
    DeathPiece *slot0 = &sim.alloc();
    const uint64_t generation1 = slot0->generation;
    CHECK(generation1 != 0);
    slot0->active = true;
    for (int i = 1; i < DeathPieceSim::kCapacity; ++i) sim.alloc();

    DeathPiece &active_reuse = sim.alloc();
    CHECK(&active_reuse == slot0);
    CHECK(active_reuse.generation == generation1 + 1);
    CHECK(!active_reuse.active);
    const uint64_t generation2 = active_reuse.generation;
    active_reuse.active = true;
    active_reuse.settled = true;
    for (int i = 1; i < DeathPieceSim::kCapacity; ++i) sim.alloc();

    DeathPiece &settled_reuse = sim.alloc();
    CHECK(&settled_reuse == slot0);
    CHECK(settled_reuse.generation == generation2 + 1);
    CHECK(!settled_reuse.active);
    CHECK(!settled_reuse.settled);
    const uint64_t generation3 = settled_reuse.generation;
    sim.reset();
    CHECK(sim.alloc().generation == generation3 + 1);
}

// The witnessed launch build [orig: @0x493718-0x49380e]: the horizontal launch
// speed is EXACTLY vel_scale (two 2D normalizes; the vertical is an independent
// rand in [0, 1.25*vel)), and the spin rates are max*(rand%100)/100 floored at
// min, integrated 1:1 per tick as degrees [orig: @0x57b940 / @0x492db9].
void test_death_piece_launch_and_spin() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 700;
    seed.health = 0;
    seed.position = Vec3{0.0f, 0.0f, 10.0f};
    seed.bound_radius = 2.0f;
    const EntityHandle h = w.registry.spawn(1, seed);
    w.tables.item_death_traits.set(700, barrel_traits());
    Entity *e = w.registry.get(h);
    spawn_death_pieces(w, *e);
    int live = 0;
    const DeathPiece *first = nullptr;
    for (const DeathPiece &p : w.death_pieces.pieces) {
        if (!p.active) continue;
        ++live;
        if (first == nullptr) first = &p;
        const DeathPieceType &tp = death_piece_type(p.type_index);
        const float hspeed = std::sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y);
        CHECK(std::abs(hspeed - tp.vel_scale) < 1.0e-4f);
        CHECK(p.vel.z >= 0.0f);
        CHECK(p.vel.z <= tp.vel_scale * 1.25f);
        CHECK(p.spin_a >= tp.spin_min && p.spin_a <= tp.spin_max);
        CHECK(p.spin_b >= tp.spin_min && p.spin_b <= tp.spin_max);
    }
    CHECK(live == 3);
    CHECK(first != nullptr);
    if (first != nullptr) {
        const float spin = first->spin_a;
        const float h0 = first->heading;
        w.death_pieces.tick(w, nullptr, -1.0e9f, w.out.destruction);
        CHECK(std::abs((first->heading - h0) - spin) < 1.0e-4f);
    }
}

// Destruction PRNG state is world-local: two independent simulations starting
// from the same state produce the same first debris launch regardless of
// construction or test order.
void test_death_piece_rng_is_world_local() {
    auto make_world = [] {
        auto world = std::make_unique<World>();
        seed_ammo(*world);
        world->registry.configure_pool(1, 4);
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.item_id = 700;
        seed.health = 0;
        seed.position = Vec3{0.0f, 0.0f, 10.0f};
        seed.bound_radius = 2.0f;
        world->registry.spawn(1, seed);
        world->tables.item_death_traits.set(700, barrel_traits());
        return world;
    };
    auto a = make_world();
    auto b = make_world();
    spawn_death_pieces(*a, *a->registry.get(EntityHandle::make(1, 0)));
    spawn_death_pieces(*b, *b->registry.get(EntityHandle::make(1, 0)));
    const DeathPiece *pa = nullptr;
    const DeathPiece *pb = nullptr;
    for (const DeathPiece &p : a->death_pieces.pieces)
        if (p.active) { pa = &p; break; }
    for (const DeathPiece &p : b->death_pieces.pieces)
        if (p.active) { pb = &p; break; }
    CHECK(pa != nullptr && pb != nullptr);
    if (pa != nullptr && pb != nullptr) {
        CHECK(std::abs(pa->vel.x - pb->vel.x) < 1.0e-6f);
        CHECK(std::abs(pa->vel.y - pb->vel.y) < 1.0e-6f);
        CHECK(std::abs(pa->vel.z - pb->vel.z) < 1.0e-6f);
        CHECK(std::abs(pa->spin_a - pb->spin_a) < 1.0e-6f);
        CHECK(std::abs(pa->spin_b - pb->spin_b) < 1.0e-6f);
        CHECK(pa->bounces_left == pb->bounces_left);
    }
}

// Retail transforms each section center through the complete authored Euler
// pose [orig: @0x4938bf-0x493900].  As above, the three +90 degree rotations
// make (x,y,z) -> (z,-y,x), witnessing both signs and multiplication order.
void test_death_piece_section_center() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 700;
    seed.health = 0;
    seed.position = Vec3{4.0f, 6.0f, 10.0f};
    seed.bound_radius = 2.0f;
    seed.yaw = 0;
    seed.pitch = 90;
    seed.roll = 90;
    const EntityHandle h = w.registry.spawn(1, seed);
    ItemDeathTraits t = barrel_traits();
    t.husk_section_count = 2; // sections 0..1 -> one piece from section 1
    t.husk_section_centers = {Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.5f, -0.5f, 0.75f}};
    w.tables.item_death_traits.set(700, t);
    Entity *e = w.registry.get(h);
    spawn_death_pieces(w, *e);
    const DeathPiece *p = nullptr;
    for (const DeathPiece &q : w.death_pieces.pieces)
        if (q.active) { p = &q; break; }
    CHECK(p != nullptr);
    if (p != nullptr) {
        CHECK(std::abs(p->pos.x - 4.75f) < 1.0e-4f);
        CHECK(std::abs(p->pos.y - 6.5f) < 1.0e-4f);
        CHECK(std::abs(p->pos.z - 11.5f) < 1.0e-4f);
    }
}

// Underwater pieces: the horizontal halves per tick and the fall pins at
// -4096; the surface crossing splashes once [orig: Entity_ApplyGravitySimple
// @0x492d80 / the water leg @0x492e1b-0x492ebe].
void test_death_piece_water() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    DeathPiece &p = w.death_pieces.alloc();
    p.active = true;
    p.type_index = 2; // CHUNK_M — authors splash slots
    p.pos = Vec3{0.0f, 0.0f, 0.05f};
    p.vel = Vec3{0.8f, 0.0f, -0.2f};
    p.bounces_left = 3;
    // Tick 1: starts above water 0 -> gravity leg, integrates below, splashes.
    w.death_pieces.tick(w, nullptr, 0.0f, w.out.destruction);
    bool splashed = false;
    for (const DestructionSoundEvent &s : w.out.destruction.sounds)
        if (s.sound == "IMP_DEBSML_WATER" || s.sound == "IMP_DEBMED_WATER")
            splashed = true;
    CHECK(splashed);
    // Tick 2: below water -> horizontal halves, vertical pins at -4096.
    const float vx = p.vel.x;
    w.death_pieces.tick(w, nullptr, 0.0f, w.out.destruction);
    CHECK(std::abs(p.vel.x - vx * 0.5f) < 1.0e-5f);
    CHECK(std::abs(p.vel.z + 4096.0f / 65536.0f) < 1.0e-6f);
}

// Exhausted debris marks terrain with standard scorch 7 before its final
// presentation events. The debris-type bit 1 suppresses only CACTUS_, while a
// bounce and the underwater free leg emit nothing.
// [orig: Entity_ProcessDeathPiecePhysics @0x492FDF..0x492FEC]
void test_death_piece_ground_scorch_routing() {
    constexpr int kDim = 512;
    std::vector<uint16_t> heightmap(kDim * kDim, 0);
    std::vector<int> sector_grid(256, 1);
    TerrainHeightField flat;
    flat.heightmap = heightmap.data();
    flat.dim = kDim;
    flat.layout.sector_grid = sector_grid.data();
    flat.layout.origin_x = 0;
    flat.layout.origin_y = 0;
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.logic_tick = 91;
    crt_srand(1);

    auto ground_piece = [&](uint8_t type_index, uint32_t flags,
                            int32_t bounces_left, float x) -> DeathPiece & {
        DeathPiece &p = w.death_pieces.alloc();
        p.active = true;
        p.type_index = type_index;
        p.flags = flags;
        p.pos = Vec3{x, 8.0f, 0.001f};
        p.vel = Vec3{0.0f, 0.0f, -1.0f};
        p.bounces_left = bounces_left;
        return p;
    };

    DeathPiece &exhausted = ground_piece(3, 0, 0, 8.0f); // CHUNK_M
    w.death_pieces.tick(w, &flat, -1.0e9f, w.out.destruction);
    CHECK(!exhausted.active);
    CHECK(w.out.terrain_scorches.pending().size() == 1);
    if (w.out.terrain_scorches.pending().size() == 1) {
        const TerrainScorchEvent &event = w.out.terrain_scorches.pending().front();
        CHECK(event.tick == 91);
        CHECK(event.mission_bounds.texture_index == 2); // srand(1): 41 % 3
        CHECK(event.mission_bounds.minimum_x_q16 == fixed16(4.0));
        CHECK(event.mission_bounds.maximum_x_q16 == fixed16(12.0));
        CHECK(event.mission_bounds.minimum_z_q16 == fixed16(4.0));
        CHECK(event.mission_bounds.maximum_z_q16 == fixed16(12.0));
    }

    DeathPiece &cactus = ground_piece(11, 0x2u, 0, 16.0f);
    w.death_pieces.tick(w, &flat, -1.0e9f, w.out.destruction);
    CHECK(!cactus.active);
    CHECK(w.out.terrain_scorches.pending().size() == 1);

    DeathPiece &bouncing = ground_piece(3, 0, 1, 24.0f);
    w.death_pieces.tick(w, &flat, -1.0e9f, w.out.destruction);
    CHECK(bouncing.active);
    CHECK(bouncing.bounces_left == 0);
    CHECK(w.out.terrain_scorches.pending().size() == 1);

    DeathPiece &underwater = ground_piece(3, 0, 0, 32.0f);
    underwater.pos.z = -0.01f;
    w.death_pieces.tick(w, &flat, 1.0f, w.out.destruction);
    CHECK(!underwater.active);
    CHECK(w.out.terrain_scorches.pending().size() == 1);
}

// The bullet armor gates [orig: Projectile_ProcessDamageOnTarget @0x4e7fb0].
void test_bullet_gates() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 500;
    seed.health = 100;
    const EntityHandle h = w.registry.spawn(1, seed);
    Entity *e = w.registry.get(h);
    ItemDeathTraits t = barrel_traits(); // armor_impact 5
    w.tables.item_death_traits.set(500, t);
    CHECK(item_bullet_damage_gate(w, *e, 40, 10) == 40); // pen 10 >= armor 5
    CHECK(item_bullet_damage_gate(w, *e, 40, 3) == 0);   // pen 3 < armor 5 [orig: @0x4e802a]
    t.armor_impact = -1; // the 0xFFFF invulnerable word [orig: @0x4e8019]
    w.tables.item_death_traits.set(500, t);
    CHECK(item_bullet_damage_gate(w, *e, 40, 100) == 0);
    t.armor_impact = 0;
    t.no_die = true; // clamps to health-1 [orig: @0x4e8074]
    w.tables.item_death_traits.set(500, t);
    CHECK(item_bullet_damage_gate(w, *e, 500, 100) == 99);
    e->engine_flags |= kEntityFlagIndestructible; // [orig: @0x4e7ff6]
    CHECK(item_bullet_damage_gate(w, *e, 40, 100) == 0);
}

// The dead-item settle + the landing kz blast for vehicle-family wrecks
// [orig: Entity_UpdateFallingDeathPhysics @0x493f70 @0x4941be].
void test_dead_item_settle() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 700;
    seed.health = 0;
    seed.position = Vec3{0.0f, 0.0f, 5.0f};
    seed.bound_radius = 2.0f;
    const EntityHandle h = w.registry.spawn(1, seed);
    ItemDeathTraits t = barrel_traits();
    t.unit_type = 1;
    w.tables.item_death_traits.set(700, t);
    Entity *e = w.registry.get(h);
    e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    e->alive = false;
    e->death_motion = DeathMotionMode::Falling;
    e->veh.slide_z = -65536; // falling 1 u/tick
    // No terrain -> ground at -1e9: it keeps falling, gravity accumulating
    // [orig: slideDecay -= 334 @0x493fe5].
    const int32_t sz0 = e->veh.slide_z;
    tick_dead_items(w, nullptr, -1.0e9f, w.out.destruction);
    CHECK(e->veh.slide_z == sz0 - 334);
    CHECK(e->position.z < 5.0f);
}

// ItemDef attrib2 StaticDeath freezes only the generic falling callback.
// Routed Falling and building Static callbacks do not consult this bit.
void test_generic_staticdeath_freezes() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(1, 4);
    ItemDeathTraits traits = barrel_traits();
    traits.static_death = true;
    w.tables.item_death_traits.set(705, traits);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 705;
    seed.health = 0;
    seed.position = Vec3{3.0f, 4.0f, 5.0f};
    const EntityHandle h = w.registry.spawn(1, seed);
    Entity *e = w.registry.get(h);
    e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    e->death_motion = DeathMotionMode::Generic;
    e->veh.vel_x = 65536;
    e->veh.vel_y = -32768;
    e->veh.slide_z = -65536;
    const Vec3 before = e->position;
    tick_dead_items(w, nullptr, -1.0e9f, w.out.destruction);
    CHECK(std::abs(e->position.x - before.x) < 1.0e-6f);
    CHECK(std::abs(e->position.y - before.y) < 1.0e-6f);
    CHECK(std::abs(e->position.z - before.z) < 1.0e-6f);
    CHECK(e->veh.vel_x == 0);
    CHECK(e->veh.vel_y == 0);
    CHECK(e->veh.slide_z == 0);
}

// A flat 512x512 height field (constant raw16) — the same wiring as
// tests/world/infantry_test.cpp.
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

// The unitType-3 husk keeps its own DeathPiece_PhysicsUpdate callback rather
// than borrowing either ordinary falling path. Pin its Q16 air/water motion,
// one PRNG draw, short four-probe slope solve, landing presentation order,
// dual authority blast, standard scorch, and the separate pitch-settle phase.
// [orig: DeathPiece_PhysicsUpdate @0x48F500;
// Entity_CalcSlopeForces @0x4B0B00; DeathPiece_SettlePitch @0x48F0B0]
void test_specialized_piece_physics_callback() {
    FlatField flat(0);
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 8);
    w.logic_tick = 73;
    ItemDeathTraits traits = barrel_traits();
    traits.unit_type = 3;
    traits.kz = 0.0f; // forces integer-truncated bound-radius fallback
	traits.particlefire.clear();
	traits.effect_banks[1] = {}; // keep the callback's one PRNG draw isolated
	traits.particlefinale = "Effect_PieceFinale";
	w.tables.item_death_traits.set(730, traits);
    // The GRAPHIC probe-box floor sits at -2.0: a husked wreck must never read
    // it [orig: Flags & 4 picks entity+0x34 huskModel @ 0x4b0c10..0x4b0c1b].
    VehicleTraits model;
    model.box_z_lo = -2 * 65536;
    model.box_z_hi = 65536;
    w.vehicles.traits.set(730, model);

    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 730;
    seed.health = 0;
    seed.alive = false;
    seed.position = Vec3{8.0f, 8.0f, 1.0f};
    seed.bound_radius = 2.75f;
    seed.yaw = 90; // engine heading BAM 0: forward probe is +mission X
    seed.pitch = 10;
    seed.roll = -3;
    const EntityHandle h = w.registry.spawn(1, seed);
    Entity *piece = w.registry.get(h);
    piece->engine_flags |=
            kEntityFlagDead | kEntityFlagHusk | kEntityFlagBuilding;
    // The HUSK shell floor at -1.0 is the witnessed source for a husked piece.
    CollisionWorld wreck_collision;
    attach_wreck_shells(w, wreck_collision, h, -2.0, -1.0);
    piece->death_motion = DeathMotionMode::PiecePhysics;
    piece->veh.vel_x = 65536;
    piece->veh.vel_y = -32768;
    piece->veh.slide_z = -65536;
    piece->veh.air_pitch_rate = 0x00100000;
    piece->veh.air_roll_rate = 0x00200000;

    DestructionRng expected_rng = w.destruction_rng;
    const uint16_t angle_roll = expected_rng.next16();
    // The piece still holds its placement angles, the spawn form (low half
    // zero). [orig: Entity_SpawnFromBMSRecord @0x40EB69..0x40EBA6]
    const int32_t initial_pitch = 119275520; // ((10 << 16) / 360) << 16
    const int32_t initial_roll = -35782656;  // ((-3 << 16) / 360) << 16
    const int32_t expected_pitch = (angle_roll & 1u) != 0
            ? initial_pitch + 0x00100000
            : initial_pitch;
    const int32_t expected_roll = (angle_roll & 1u) != 0
            ? initial_roll + 0x00200000
            : initial_roll;
    const int32_t expected_yaw = (angle_roll & 1u) != 0
            ? 0
            : 0x02D82D82;
    crt_srand(1);
    tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);

    CHECK(w.destruction_rng.state == expected_rng.state);
    CHECK(piece->veh.air_pitch_bam == expected_pitch);
    CHECK(piece->veh.air_roll_bam == expected_roll);
    CHECK(piece->veh.yaw_bam == expected_yaw);
    CHECK(piece->death_motion == DeathMotionMode::PiecePitchSettle);
    CHECK(piece->veh.air_pitch_rate == 0); // flat four-probe slope target
    CHECK(piece->position.x == 9.0f);
    CHECK(piece->position.y == 7.5f);
    // avg ground 0 - ((240 * HUSK floor -65536) >> 8) = 0xF000, then -0x8000.
    // (The graphic floor -2.0 would have produced 0x16000.)
    CHECK(to_fixed(piece->position.z) == 0x7000);
    CHECK((piece->engine_flags & kEntityFlagBuilding) == 0);

    bool ground_hit = false;
    for (const DestructionEffectEvent &fx : w.out.destruction.effects)
        if (fx.effect == "Effect_HeloGroundHit") ground_hit = true;
    CHECK(ground_hit);
    bool landed_sound = false;
    for (const DestructionSoundEvent &sound : w.out.destruction.sounds)
        if (sound.sound == "EXPLO_VEHCL_LG") landed_sound = true;
    CHECK(landed_sound);
    CHECK(w.out.terrain_scorches.pending().size() == 1);
    if (w.out.terrain_scorches.pending().size() == 1) {
        const auto &bounds =
                w.out.terrain_scorches.pending().front().mission_bounds;
        CHECK(bounds.minimum_x_q16 == fixed16(5.0));
        CHECK(bounds.maximum_x_q16 == fixed16(13.0));
        CHECK(bounds.minimum_z_q16 == fixed16(3.5));
        CHECK(bounds.maximum_z_q16 == fixed16(11.5));
    }
    CHECK(w.explosions.queue.size() == 2);
    if (w.explosions.queue.size() == 2) {
        CHECK(w.explosions.queue[0].ammo_index == 2);
        CHECK(w.explosions.queue[1].ammo_index == 3);
        for (const ExplosionEntry &blast : w.explosions.queue) {
            CHECK(blast.owner == h);
            CHECK(blast.hit_word == 1);
            CHECK(blast.radius_override == 2.0f);
            CHECK(to_fixed(blast.pos.z) == 0x7000);
        }
    }

    // Contact does not transition immediately. The replacement callback steps
    // pitch toward the stored target, snaps inside four degrees, then calls
    // Entity_TransitionToGroundDeath only on the next equal-pitch tick.
    int settle_ticks = 0;
    while (piece->death_motion == DeathMotionMode::PiecePitchSettle &&
           settle_ticks < 12) {
        const int32_t before = piece->veh.air_pitch_bam;
        tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
        ++settle_ticks;
        if (piece->death_motion == DeathMotionMode::PiecePitchSettle) {
            const int32_t moved = std::abs(before - piece->veh.air_pitch_bam);
            CHECK(moved <= 0x016C16C0 || piece->veh.air_pitch_bam == 0);
        }
    }
    CHECK(piece->death_motion == DeathMotionMode::Generic);
    CHECK(piece->veh.air_pitch_bam == 0);
    CHECK(piece->saved_live_valid);
    CHECK(settle_ticks >= 2 && settle_ticks < 12);
    int finale_count = 0;
    for (const DestructionEffectEvent &fx : w.out.destruction.effects)
        if (fx.effect == "Effect_PieceFinale") ++finale_count;
    CHECK(finale_count == 1);
    CHECK(w.out.terrain_scorches.pending().size() == 1);
    CHECK(w.explosions.queue.size() == 2);

    // In the main callback the angle draw precedes the wreck-fire roll. In the
    // equal-pitch settle branch the callback transitions directly and does not
    // call Entity_UpdateDeadWreckEffects at all.
    auto ordered_heap = std::make_unique<World>();
    World &ordered = *ordered_heap;
    ordered.registry.configure_pool(1, 4);
    ItemDeathTraits burning = barrel_traits();
    burning.unit_type = 3;
    ordered.tables.item_death_traits.set(733, burning);
    seed.item_id = 733;
    seed.position = Vec3{20.0f, 20.0f, 10.0f};
    seed.yaw = 90;
    seed.pitch = 0;
    seed.roll = 0;
    const EntityHandle ordered_h = ordered.registry.spawn(1, seed);
    Entity *ordered_piece = ordered.registry.get(ordered_h);
    ordered_piece->engine_flags |= kEntityFlagDead | kEntityFlagHusk;
    ordered_piece->death_motion = DeathMotionMode::PiecePhysics;
    ordered_piece->veh.air_pitch_rate = 0x00100000;
    ordered_piece->veh.air_roll_rate = 0x00200000;
    DestructionRng ordered_expected = ordered.destruction_rng;
    const uint16_t ordered_angle_roll = ordered_expected.next16();
	const uint32_t fire_state_before = ordered.prng16_c_state; // independent retail PRNG_C
	tick_dead_items(ordered, nullptr, -1.0e9f, ordered.out.destruction);
	CHECK(ordered.destruction_rng.state == ordered_expected.state);
	CHECK(ordered.prng16_c_state != fire_state_before);
	CHECK(ordered_piece->veh.yaw_bam == ((ordered_angle_roll & 1u) != 0 ? 0 : 0x02D82D82));
	CHECK(ordered_piece->veh.air_pitch_bam ==
            ((ordered_angle_roll & 1u) != 0 ? 0x00100000 : 0));
    ordered_piece->death_motion = DeathMotionMode::PiecePitchSettle;
    ordered_piece->veh.air_pitch_bam = 0;
    ordered_piece->veh.air_pitch_rate = 0;
    const uint32_t equal_pitch_rng = ordered.destruction_rng.state;
    tick_dead_items(
            ordered, nullptr, -1.0e9f, ordered.out.destruction);
    CHECK(ordered_piece->death_motion == DeathMotionMode::Generic);
    CHECK(ordered.destruction_rng.state == equal_pitch_rng);

    // Below water, a strict surface crossing clears both angular rates,
    // halves signed XY, floors only an overly-negative Z rate at -2048, and
    // emits the resolved global medium splash plus the helo-water fallback.
    auto wet_heap = std::make_unique<World>();
    World &wet = *wet_heap;
    wet.registry.configure_pool(1, 4);
    ItemDeathTraits wet_traits = traits;
    wet.tables.item_death_traits.set(731, wet_traits);
    seed.item_id = 731;
    seed.position = Vec3{2.0f, 3.0f, -0.25f};
    seed.pitch = 4;
    seed.roll = 2;
    const EntityHandle wet_h = wet.registry.spawn(1, seed);
    Entity *submerged = wet.registry.get(wet_h);
    submerged->engine_flags |=
            kEntityFlagDead | kEntityFlagHusk | kEntityFlagBuilding;
    submerged->death_motion = DeathMotionMode::PiecePhysics;
    submerged->veh.vel_x = 65536;
    submerged->veh.vel_y = -65535;
    submerged->veh.slide_z = -65536;
    submerged->veh.air_pitch_rate = 0x01000000;
    submerged->veh.air_roll_rate = -0x01000000;
    const uint32_t wet_rng_before = wet.destruction_rng.state;
    tick_dead_items(wet, nullptr, 0.0f, wet.out.destruction);
    CHECK(wet.destruction_rng.state == wet_rng_before);
    CHECK(submerged->death_motion == DeathMotionMode::PiecePhysics);
    CHECK(submerged->veh.vel_x == 32768);
    CHECK(submerged->veh.vel_y == -32768);
    CHECK(submerged->veh.slide_z == -2048);
    CHECK(submerged->veh.air_pitch_rate == 0);
    CHECK(submerged->veh.air_roll_rate == 0);
    CHECK((submerged->engine_flags & kEntityFlagBuilding) == 0);
    CHECK(to_fixed(submerged->position.z) == fixed16(-0.25) - 2048);
    bool wet_effect = false;
    for (const DestructionEffectEvent &fx : wet.out.destruction.effects) {
        if (fx.effect != "Effect_MedSplash") continue;
        wet_effect = true;
        CHECK(fx.pos.z == 0.0f);
    }
    CHECK(wet_effect);
    bool wet_sound = false;
    for (const DestructionSoundEvent &sound : wet.out.destruction.sounds)
        if (sound.sound == "EXPLO_HELO_WATER") wet_sound = true;
    CHECK(wet_sound);
    CHECK(wet.out.terrain_scorches.pending().empty());
    CHECK(wet.explosions.queue.empty());

    // The same visible landing runs on a non-authority simulation, but neither
    // blast is queued. A shallow X ramp also proves the forward slope output is
    // retained as the pitch target rather than discarded.
    FlatField ramp(0);
    for (int z = 0; z < FlatField::kDim; ++z)
        for (int x = 0; x < FlatField::kDim; ++x)
            ramp.heightmap[static_cast<size_t>(z) * FlatField::kDim + x] =
                    static_cast<uint16_t>(x * 8);
    auto client_heap = std::make_unique<World>();
    World &client = *client_heap;
    seed_ammo(client);
    client.rules.logic_authority = false;
    client.registry.configure_pool(1, 4);
    client.tables.item_death_traits.set(732, traits);
    client.vehicles.traits.set(732, model);
    seed.item_id = 732;
    seed.position = Vec3{64.0f, 8.0f,
            opennova::terrain::height_field_height_world_bilinear(
                    ramp.field, 64.0f, -8.0f) + 1.25f};
    seed.yaw = 90;
    seed.pitch = 0;
    seed.roll = 0;
    const EntityHandle client_h = client.registry.spawn(1, seed);
    Entity *client_piece = client.registry.get(client_h);
    client_piece->engine_flags |= kEntityFlagDead | kEntityFlagHusk;
    client_piece->death_motion = DeathMotionMode::PiecePhysics;
    client_piece->veh.slide_z = -65536;
    CollisionWorld client_collision;
    attach_wreck_shells(client, client_collision, client_h, -2.0, -1.0);
    tick_dead_items(
            client, &ramp.field, -1.0e9f, client.out.destruction);
    CHECK(client_piece->death_motion == DeathMotionMode::PiecePitchSettle);
    CHECK(client_piece->veh.air_pitch_rate > 0);
    CHECK(client.out.terrain_scorches.pending().size() == 1);
    CHECK(client.explosions.queue.empty());

    // A husked piece whose def authors no husk shell has a null model pick:
    // the rest correction is ZERO, never the graphic floor [orig: the null
    // pick keeps ecx = 0 @ 0x4b0c0e..0x4b0c20]. With the graphic floor at
    // -2.0 the old read would have landed this piece 1.875 u above ground.
    auto bare_heap = std::make_unique<World>();
    World &bare = *bare_heap;
    seed_ammo(bare);
    bare.registry.configure_pool(1, 4);
    bare.tables.item_death_traits.set(734, traits);
    bare.vehicles.traits.set(734, model);
    seed.item_id = 734;
    seed.position = Vec3{8.0f, 8.0f, 0.5f};
    seed.yaw = 90;
    seed.pitch = 0;
    seed.roll = 0;
    const EntityHandle bare_h = bare.registry.spawn(1, seed);
    Entity *bare_piece = bare.registry.get(bare_h);
    bare_piece->engine_flags |= kEntityFlagDead | kEntityFlagHusk;
    bare_piece->death_motion = DeathMotionMode::PiecePhysics;
    // A gentle fall: the four probe rays reach 0.25 u above and 2.0 u below
    // the piece [orig: lift 0x4000 / depth 0x20000 @ 0x4b0b73], so a tick
    // that drops it straight through that window would never see ground.
    bare_piece->veh.slide_z = -16384;
    CollisionWorld bare_collision;
    bare_collision.assign_entity(bare_h,
            bare_collision.add_model(wreck_shell_model(-2.0, 2.0)));
    bare.collision = &bare_collision;
    // With a zero rest correction the ground target is the flat probe
    // average itself (0): the piece must fall from 0.5 to below it before it
    // lands; the husk-floor cases above start below their raised target.
    for (int tick = 0; tick < 16 &&
            bare_piece->death_motion == DeathMotionMode::PiecePhysics; ++tick) {
        tick_dead_items(bare, &flat.field, -1.0e9f, bare.out.destruction);
    }
    CHECK(bare_piece->death_motion == DeathMotionMode::PiecePitchSettle);
    // ground = avg 0 - 0, then the -0x8000 landing offset.
    CHECK(to_fixed(bare_piece->position.z) == -0x8000);
}

// The landing split [orig: the generic leg 0x461d30 lands silently with motion
// dead-stopped; the unitType-routed leg 0x493f70 adds the clunk @0x4941af and
// the authority kz @0x4941be], and the section-0 ground-rest offset
// [orig: ground -= |sec0 z min| @0x461e23-0x461e4b].
void test_dead_item_landing_split() {
    FlatField flat(0x4000);
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 8);
    const float ground = opennova::terrain::height_field_height_world_bilinear(
            flat.field, 8.0f, -8.0f);
    auto drop = [&](int32_t id, const ItemDeathTraits &traits) {
        w.tables.item_death_traits.set(id, traits);
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.item_id = id;
        seed.health = 0;
        seed.position = Vec3{8.0f, 8.0f, ground + 0.5f};
        seed.bound_radius = 2.0f;
        const EntityHandle h = w.registry.spawn(1, seed);
        Entity *e = w.registry.get(h);
        e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
        e->alive = false;
        e->death_motion = traits.unit_type == 1
                ? DeathMotionMode::Falling
                : DeathMotionMode::Generic;
        e->veh.slide_z = -65536; // 1 u/tick down -> lands this tick
        e->veh.vel_x = 3277;     // sliding sideways
        return e;
    };
    // Generic item (unit_type 0): silent contact restores the complete pre-move
    // pose. Contact clears slide_z while horizontal velocity remains live.
    ItemDeathTraits t = barrel_traits();
    t.husk_rest_min_z = -0.25f;
    t.particlefinale = "Effect_VehDrtPuftrk";
    Entity *item = drop(700, t);
    const Vec3 item_before = item->position;
    tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
    CHECK(std::abs(item->position.x - item_before.x) < 1.0e-6f);
    CHECK(std::abs(item->position.y - item_before.y) < 1.0e-6f);
    CHECK(std::abs(item->position.z - item_before.z) < 1.0e-6f);
    CHECK(item->veh.slide_z == 0);
    CHECK(item->veh.vel_x == 3277);
    bool clunked = false;
    for (const DestructionSoundEvent &s : w.out.destruction.sounds)
        if (s.sound == "IMP_VCL_DROP") clunked = true;
    CHECK(!clunked);
    CHECK(w.explosions.queue.empty()); // no landing kz for the generic leg
    // The generic leg never runs Entity_TransitionToGroundDeath, so its
    // authored particlefinale stays silent [orig: 0x461d30 has no call].
    for (const DestructionEffectEvent &fx : w.out.destruction.effects)
        CHECK(fx.effect != "Effect_VehDrtPuftrk");
    item->engine_flags &= ~kEntityFlagHusk; // retire from the pass
    // A unitType-routed row (1): the clunk + the landing kz.
    ItemDeathTraits tv = barrel_traits();
    tv.unit_type = 1;
    tv.particlefinale = "Effect_VehDrtPuftrk";
    Entity *wreck = drop(701, tv);
    tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
    const float routed_new_z = ground + 0.5f +
            static_cast<float>(-65536 - 334) / 65536.0f;
    CHECK(std::abs(wreck->position.z - routed_new_z) < 1.0e-3f);
    CHECK(wreck->veh.slide_z == -65536 - 334);
    CHECK(wreck->veh.vel_x == 3277);
    CHECK(wreck->death_motion == DeathMotionMode::Generic);
    for (const DestructionSoundEvent &s : w.out.destruction.sounds) {
        if (s.sound != "IMP_VCL_DROP") continue;
        clunked = true;
        CHECK(std::abs(s.pos.x - 8.0f) < 1.0e-6f);
        CHECK(std::abs(s.pos.z - ground) < 1.0e-3f);
    }
    CHECK(clunked);
    CHECK(!w.explosions.queue.empty());
    CHECK(std::abs(w.explosions.queue.back().pos.x - 8.0f) < 1.0e-6f);
    CHECK(std::abs(w.explosions.queue.back().pos.z - ground) < 1.0e-3f);
    CHECK(wreck->position.x > 8.0f);
    // The routed contact runs the transition: the authored particlefinale
    // plays once at the ground pose [orig: Entity_TransitionToGroundDeath
    // @0x493080 read @0x493088; called @0x494113] and the grounded pose is
    // backed into savedLivePose (@0x4930dd..0x493104).
    bool finale = false;
    for (const DestructionEffectEvent &fx : w.out.destruction.effects) {
        if (fx.effect != "Effect_VehDrtPuftrk") continue;
        finale = true;
        CHECK(std::abs(fx.pos.x - 8.0f) < 1.0e-6f);
        CHECK(std::abs(fx.pos.z - ground) < 1.0e-3f);
    }
    CHECK(finale);
    CHECK(wreck->saved_live_valid);
    CHECK(w.out.terrain_scorches.pending().size() == 1);
    if (w.out.terrain_scorches.pending().size() == 1) {
        const TerrainScorchEvent &event = w.out.terrain_scorches.pending().front();
        // The router reads the entity pose before the common tail commits its
        // integrated x/y, matching [orig: @0x49416B..0x494174].
        CHECK(event.mission_bounds.minimum_x_q16 == fixed16(4.0));
        CHECK(event.mission_bounds.maximum_x_q16 == fixed16(12.0));
        CHECK(event.mission_bounds.minimum_z_q16 == fixed16(4.0));
        CHECK(event.mission_bounds.maximum_z_q16 == fixed16(12.0));
    }

    const size_t authority_blasts = w.explosions.queue.size();
    w.rules.logic_authority = false;
    Entity *client_wreck = drop(702, tv);
    tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
    CHECK(client_wreck->death_motion == DeathMotionMode::Generic);
    CHECK(w.explosions.queue.size() == authority_blasts);
}

// Building-family rows install Entity_UpdateStaticDeathPhysics only after the
// a husk exists. Its half-gravity, horizontal damping, and ground transition
// differ from both falling callbacks.
void test_static_dead_item_settle() {
    FlatField flat(0);
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 32);
    const float ground = opennova::terrain::height_field_height_world_bilinear(
            flat.field, 8.0f, -8.0f);

    int32_t next_id = 720;
    auto spawn_static = [&](int unit_type, const Vec3 &position) {
        const int32_t item_id = next_id++;
        ItemDeathTraits traits = barrel_traits();
        traits.unit_type = unit_type;
        w.tables.item_death_traits.set(item_id, traits);
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.item_id = item_id;
        seed.health = 0;
        seed.position = position;
        seed.bound_radius = 1.0f;
        const EntityHandle h = w.registry.spawn(1, seed);
        Entity *e = w.registry.get(h);
        entity_update_death_transforms(w, *e, true);
        CHECK(e->death_motion == DeathMotionMode::Static);
        CHECK(e->veh.slide_z == 0);
        return e;
    };

    for (int unit_type : {5, 6, 7, 8}) {
        Entity *sliding = spawn_static(
                unit_type, Vec3{8.0f + static_cast<float>(unit_type), 8.0f, 10.0f});
        const float x0 = sliding->position.x;
        sliding->veh.vel_x = 65536;
        sliding->veh.vel_y = 0;
        sliding->veh.slide_z = 0;
        tick_dead_items(w, nullptr, -1.0e9f, w.out.destruction);
        CHECK(std::abs(sliding->position.x - (x0 + 1.0f)) < 1.0e-6f);
        CHECK(sliding->veh.vel_x == 63569);
        CHECK(sliding->veh.slide_z == 0);
        sliding->death_motion = DeathMotionMode::None;

        Entity *stopping = spawn_static(
                unit_type, Vec3{20.0f + static_cast<float>(unit_type), 8.0f, 10.0f});
        stopping->veh.vel_x = 8;
        stopping->veh.vel_y = -8;
        stopping->veh.slide_z = 0;
        tick_dead_items(w, nullptr, -1.0e9f, w.out.destruction);
        CHECK(stopping->veh.vel_x == 0);
        CHECK(stopping->veh.vel_y == 0);
        CHECK(stopping->veh.slide_z == -167);
        stopping->death_motion = DeathMotionMode::None;

        Entity *landing = spawn_static(
                unit_type, Vec3{8.0f, 8.0f, ground + 0.25f});
        landing->veh.vel_x = 0;
        landing->veh.vel_y = 0;
        landing->veh.slide_z = -65536;
        tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
        CHECK(std::abs(landing->position.z - ground) < 1.0e-3f);
        CHECK(landing->veh.vel_x == 0);
        CHECK(landing->veh.vel_y == 0);
        CHECK(landing->veh.slide_z == -65536 - 167);
        CHECK(landing->death_motion == DeathMotionMode::Generic);
        tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
        CHECK(std::abs(landing->position.z - ground) < 1.0e-3f);
        CHECK(landing->veh.slide_z == 0);
    }

    // Production installs this callback on AI-capable entities; the shared
    // post-death pass must advance those entities too.
    Entity *ai_static = spawn_static(5, Vec3{40.0f, 8.0f, 10.0f});
    ai_static->is_ai_capable = true;
    ai_static->veh.vel_x = 0;
    ai_static->veh.vel_y = 0;
    ai_static->veh.slide_z = 0;
    tick_dead_items(w, nullptr, -1.0e9f, w.out.destruction);
    CHECK(ai_static->veh.slide_z == -167);

    Entity *below_sliding = spawn_static(5, Vec3{50.0f, 8.0f, ground - 1.0f});
    below_sliding->veh.vel_x = 65536;
    below_sliding->veh.slide_z = 0;
    tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
    CHECK(below_sliding->death_motion == DeathMotionMode::Static);
    CHECK(std::abs(below_sliding->position.z - (ground - 1.0f)) < 1.0e-6f);

    // StaticDeath's water/ground split is asymmetric. Water below terrain
    // transitions immediately, exactly 10 units still uses the ground line,
    // and only values above 10 move the landing line down by 25 units.
    Entity *water_below = spawn_static(5, Vec3{60.0f, 8.0f, ground + 5.0f});
    water_below->veh.vel_x = 65536;
    tick_dead_items(
            w, &flat.field, ground - 1.0f, w.out.destruction);
    CHECK(water_below->death_motion == DeathMotionMode::Generic);
    CHECK(std::abs(water_below->position.z - ground) < 1.0e-3f);

    Entity *at_ten = spawn_static(5, Vec3{64.0f, 8.0f, ground - 20.0f});
    tick_dead_items(
            w, &flat.field, ground + 10.0f, w.out.destruction);
    CHECK(at_ten->death_motion == DeathMotionMode::Generic);
    CHECK(std::abs(at_ten->position.z - ground) < 1.0e-3f);

    Entity *deep_water_hold = spawn_static(
            5, Vec3{68.0f, 8.0f, ground - 20.0f});
    tick_dead_items(
            w, &flat.field, ground + 11.0f, w.out.destruction);
    CHECK(deep_water_hold->death_motion == DeathMotionMode::Static);
    CHECK(std::abs(deep_water_hold->position.z - (ground - 20.0f)) < 1.0e-3f);

    Entity *deep_water_land = spawn_static(
            5, Vec3{72.0f, 8.0f, ground - 26.0f});
    tick_dead_items(
            w, &flat.field, ground + 11.0f, w.out.destruction);
    CHECK(deep_water_land->death_motion == DeathMotionMode::Generic);
    CHECK(std::abs(deep_water_land->position.z - ground) < 1.0e-3f);
}

// The falling wreck splashes crossing the water line (the bound top passes
// below) [orig: 0x493f70 @0x49409f-0x494100 — the fallback IMP_DEBLRG_WATER].
void test_dead_item_water_splash() {
    FlatField flat(0x4000);
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 4);
    const float ground = opennova::terrain::height_field_height_world_bilinear(
            flat.field, 8.0f, -8.0f);
    const float water = ground + 20.0f;
    ItemDeathTraits t = barrel_traits();
    w.tables.item_death_traits.set(700, t);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 700;
    seed.health = 0;
    seed.position = Vec3{8.0f, 8.0f, water + 4.0f};
    seed.bound_radius = 2.0f;
    const EntityHandle h = w.registry.spawn(1, seed);
    Entity *e = w.registry.get(h);
    e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    e->alive = false;
    e->death_motion = DeathMotionMode::Generic;
    e->veh.slide_z = -8 * 65536; // 8 u/tick: the bound top crosses in one tick
    tick_dead_items(w, &flat.field, water, w.out.destruction);
    bool splashed = false;
    for (const DestructionSoundEvent &s : w.out.destruction.sounds)
        if (s.sound == "IMP_DEBLRG_WATER") splashed = true;
    CHECK(!splashed);
    CHECK(e->position.z > ground); // still sinking, not landed
    e->engine_flags &= ~kEntityFlagHusk;

    ItemDeathTraits routed = barrel_traits();
    routed.unit_type = 1;
    w.tables.item_death_traits.set(701, routed);
    seed.item_id = 701;
    const EntityHandle routed_h = w.registry.spawn(1, seed);
    Entity *routed_e = w.registry.get(routed_h);
    routed_e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    routed_e->alive = false;
    routed_e->death_motion = DeathMotionMode::Falling;
    routed_e->veh.slide_z = -8 * 65536;
    routed_e->veh.vel_x = 65536;
    routed_e->veh.vel_y = -32768;
    tick_dead_items(w, &flat.field, water, w.out.destruction);
    splashed = false;
    for (const DestructionSoundEvent &s : w.out.destruction.sounds) {
        if (s.sound != "IMP_DEBLRG_WATER") continue;
        splashed = true;
        CHECK(std::abs(s.pos.x - 9.0f) < 1.0e-6f);
        CHECK(std::abs(s.pos.y - 7.5f) < 1.0e-6f);
        CHECK(std::abs(s.pos.z - water) < 1.0e-6f);
    }
    CHECK(splashed);
    bool splash_effect = false;
    for (const DestructionEffectEvent &fx : w.out.destruction.effects) {
        if (fx.effect != "Effect_MedSplash") continue;
        splash_effect = true;
        CHECK(std::abs(fx.pos.x - 9.0f) < 1.0e-6f);
        CHECK(std::abs(fx.pos.y - 7.5f) < 1.0e-6f);
        CHECK(std::abs(fx.pos.z - water) < 1.0e-6f);
    }
    CHECK(splash_effect);

	// Authored water impact overrides the fallback and uses the old hull pose,
	// while the effect still uses the integrated crossing point.
	static constexpr char profile[] = "begin \"Wreck\"\nsound_impactwater WRECK_WATER\nend\n";
	CHECK(w.tables.sound_profiles.parse(profile, sizeof(profile) - 1) == 1);
	routed.sound_profile = "Wreck";
	w.tables.item_death_traits.set(701, routed);
	routed_e->position = seed.position;
	routed_e->veh.slide_z = -8 * 65536;
	w.out.destruction.clear();
	tick_dead_items(w, &flat.field, water, w.out.destruction);
	CHECK(w.out.destruction.sounds.size() == 1);
	CHECK(w.out.destruction.sounds[0].sound == "WRECK_WATER");
	CHECK(w.out.destruction.sounds[0].pos.x == seed.position.x);
	CHECK(w.out.destruction.sounds[0].pos.z == seed.position.z);
}

// Matched unitType dispatch rows OR the death bits and preserve the Building
// flag. Only the no-row/default path applies Flags & ~0x20006 | 6.
void test_death_dispatch_flag_routing() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(1, 4);
    auto dispatch = [&](int32_t item_id, int32_t unit_type, bool has_husk) {
        ItemDeathTraits traits;
        traits.unit_type = unit_type;
        traits.has_husk = has_husk;
        w.tables.item_death_traits.set(item_id, traits);
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.item_id = item_id;
        seed.health = 0;
        seed.engine_flags = 0x20000u;
        const EntityHandle h = w.registry.spawn(1, seed);
        Entity *e = w.registry.get(h);
        entity_update_death_transforms(w, *e, true);
        return e;
    };
    const Entity *generic = dispatch(710, 0, false);
    CHECK((generic->engine_flags & 0x20000u) == 0);
    CHECK((generic->engine_flags & 0x6u) == 0x6u);
    const Entity *building = dispatch(711, 5, false);
    CHECK((building->engine_flags & 0x20000u) != 0);
    CHECK((building->engine_flags & 0x6u) == 0x6u);
    Entity *falling = dispatch(712, 1, true);
    CHECK((falling->engine_flags & 0x20000u) != 0);
    falling->veh.slide_z = -65536;
    tick_dead_items(w, nullptr, -1.0e9f, w.out.destruction);
    CHECK((falling->engine_flags & 0x20000u) == 0);
}

// The death kick keys the def TYPE word (+0x5C == 2 = decoration), NOT the
// unitType dispatch word [orig: @0x493969 vs the glow gate @0x4934ee]: a
// unitType-2 (helicopter-row) non-decoration still POPS; a decoration DROPS.
void test_death_kick_field() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(1, 8);
    auto kill = [&](int32_t id, const ItemDeathTraits &traits) {
        w.tables.item_death_traits.set(id, traits);
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.item_id = id;
        seed.health = 0;
        seed.position = Vec3{0.0f, 0.0f, 5.0f};
        seed.bound_radius = 2.0f;
        const EntityHandle h = w.registry.spawn(1, seed);
        Entity *e = w.registry.get(h);
        entity_update_death_transforms(w, *e, /*silent=*/true);
        return e;
    };
    ItemDeathTraits heli = barrel_traits();
    heli.unit_type = 2;
    CHECK(kill(700, heli)->veh.slide_z >= 4096); // pops [orig: @0x493977]
    ItemDeathTraits deco = barrel_traits();
    deco.is_decoration = true;
    CHECK(kill(701, deco)->veh.slide_z == -16182); // drops [orig: @0x493993]

    ItemDeathTraits piece_physics = barrel_traits();
    piece_physics.unit_type = 3;
    CHECK(kill(704, piece_physics)->death_motion == DeathMotionMode::PiecePhysics);

    ItemDeathTraits no_husk = barrel_traits();
    no_husk.unit_type = 5;
    no_husk.has_husk = false;
    no_husk.husk_model_loaded = false;
    Entity *no_husk_building = kill(702, no_husk);
    CHECK(no_husk_building->veh.slide_z == 0);
    CHECK(no_husk_building->death_motion == DeathMotionMode::None);

    w.env.water_z = fixed16(20.0);
    ItemDeathTraits submerged = barrel_traits();
    submerged.unit_type = 5;
    Entity *submerged_building = kill(703, submerged);
    CHECK(submerged_building->position.z + submerged_building->bound_radius < 20.0f);
    CHECK(submerged_building->veh.slide_z == 0);
    CHECK(submerged_building->death_motion == DeathMotionMode::Static);
}

// End to end: a fired round crosses the barrel's bound sphere inside one tick,
// drains it through the armor gates, and the death chain runs — the
// shoot-a-barrel path [orig: Projectile_UpdatePhysics @0x4e9d70 ->
// Projectile_ProcessDamageOnTarget @0x4e7fb0 ->
// Entity_HandleDestructibleDeathEvent @0x440210].
void test_round_destroys_item() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    // A rifle-ish ballistic entry for the fired round.
    AmmoTableEntry rifle;
    rifle.name = "556";
    rifle.valid = true;
    rifle.kztype = ammo_kz::kBullets;
    rifle.velocity = 900;       // u/s -> ~14.5 u/tick
    rifle.max_age_ticks = 124;
    rifle.weight_in_grains = 62;
    rifle.min_damage = 20;
    rifle.max_damage = 45;
    rifle.penetration_impact = 10;
    w.tables.ammo.entries.push_back(rifle);
    const int rifle_idx = static_cast<int>(w.tables.ammo.entries.size()) - 1;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 8);

    Entity shooter_seed;
    shooter_seed.kind = EntityKind::Organic;
    shooter_seed.position = Vec3{0.0f, 0.0f, 1.0f};
    const EntityHandle shooter = w.registry.spawn(0, shooter_seed);

    Entity barrel_seed;
    barrel_seed.kind = EntityKind::Item;
    barrel_seed.item_id = 500;
    barrel_seed.has_item_def = true; // retail damage gate: geometry + ItemDef
    barrel_seed.health = 40;
    barrel_seed.position = Vec3{6.0f, 0.0f, 1.0f};
    barrel_seed.yaw = 90; // identity model placement [orig: entity matrix @0x613F40]
    barrel_seed.bound_radius = 1.0f;
    const EntityHandle barrel = w.registry.spawn(1, barrel_seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    CollisionWorld collision;
    collision.assign_entity(
        barrel, collision.add_model(projectile_wall_model(1.0)));
    collision.build_tick_tables(w);
    w.collision = &collision;

    RoundSpawnParams p;
    p.owner = shooter;
    p.shooter_handle = shooter.packed;
    p.origin = Vec3{0.0f, 0.0f, 1.0f};
    p.dir_yaw_bam = bam_heading_from_mission_yaw_deg(90.0); // mission +X
    p.dir_pitch_bam = 0;
    p.ammo_index = rifle_idx;
    // Two shots: 40 hp, kinetic damage caps at max_damage 45 but clamps to
    // remaining health — one processed hit kills.
    CHECK(w.round_sim.spawn(w, p) >= 0);
    for (int t = 0; t < 30 && w.registry.get(barrel)->health > 0; ++t)
        w.run_logic_tick(true);
    Entity *b = w.registry.get(barrel);
    CHECK(b->health <= 0);
    CHECK((b->engine_flags & kEntityFlagHusk) != 0);
    CHECK(!w.round_sim.deaths.empty());
    bool husk_evented = false;
    for (const HuskSwapEvent &h : w.out.destruction.husk_swaps)
        if (h.item_id == 500) husk_evented = true;
    CHECK(husk_evented);
}

// An authored husk name is not enough to enter the building callback. Retail
// gates the collapse body on the live huskFinal/husk model pointer after asset
// resolution; a missing/corrupt model still receives the table's death flags
// but no pieces, Static motion, or collapse sound
// [orig: Entity_ProcessBuildingDeath @0x49442c].
void test_building_death_requires_loaded_husk_model() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.registry.configure_pool(2, 2);

    ItemDeathTraits traits = barrel_traits();
    traits.unit_type = 5;
    traits.has_husk = true;          // the def authors a husk name
    traits.husk_model_loaded = false; // but asset resolution found no live model
    traits.husk_section_count = 0;   // asset resolution found no live model
    traits.husk_sub_part_count = 0;
    w.tables.item_death_traits.set(990, traits);

    Entity seed;
    seed.kind = EntityKind::Building;
    seed.item_id = 990;
    seed.health = 0;
    const EntityHandle h = w.registry.spawn(2, seed);
    Entity *building = w.registry.get(h);
    CHECK(building != nullptr);

    entity_update_death_transforms(w, *building, false);

    CHECK((building->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
          (kEntityFlagDead | kEntityFlagHusk));
    CHECK(building->death_motion == DeathMotionMode::None);
    size_t active_pieces = 0;
    for (const DeathPiece &piece : w.death_pieces.pieces)
        if (piece.active) ++active_pieces;
    CHECK(active_pieces == 0);
    bool collapse_sound = false;
    for (const DestructionSoundEvent &sound : w.out.destruction.sounds)
        if (sound.sound == "EXPLO_SHIP_TINY") collapse_sound = true;
    CHECK(!collapse_sound);
}

// The MP VISUAL CLIENT runs the destruction chain locally. Retail's client
// receives S2C 0x13, zeroes Health, and runs the class death callback
// (reason 4 = the net kill) — and its per-frame entity update drains the
// explosion queue, dead-item settle, and the death-piece pool UNGATED on every
// peer. A joiner therefore detonates the husk chain's kz blast and flies its
// pieces from its own pools; only the round pool's authority gate keeps
// gameplay consequences host-side. Regression: the drains used to be
// is_authority-only, so a joiner's 0x13-driven death queued a chain blast that
// never processed (no explosion FX, frozen pieces).
// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 — cb(entity, 4, 0) @0x42ebf5;
//  Entity_UpdateAllEntities @0x4c2100 — DeathPiece_TickAll @0x4c221c,
//  Projectile_ProcessExplosionQueue @0x4c223f, unconditional]
void test_net_kill_runs_client_side_death_chain() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(2, 8);
    // The joiner role: an MP session without projectile authority.
    w.rules.mp_session = true;
    w.rules.projectile_authority = false;

    Entity seed;
    seed.kind = EntityKind::Building;
    seed.item_id = 500;
    seed.health = 30;
    seed.position = Vec3{10.0f, 0.0f, 0.0f};
    seed.bound_radius = 1.0f;
    const EntityHandle barrel = w.registry.spawn(2, seed);
    w.tables.item_death_traits.set(500, barrel_traits());

    // The S2C 0x13 fold: Health = 0, then the class death callback (reason 4).
    Entity *b = w.registry.get(barrel);
    b->health = 0;
    b->alive = false;
    destruction_notify_item_damage(w, *b, 4);
    CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
          (kEntityFlagDead | kEntityFlagHusk));
    CHECK(w.out.destruction.husk_swaps.size() == 1);
    CHECK(w.out.destruction.husk_swaps[0].wire_handle == barrel.packed);
    CHECK(w.explosions.queue.size() == 1); // the chain blast is staged

    // The non-authority client tick drains the queue exactly like retail's
    // shared per-frame update; the presentation counters advance.
    w.run_logic_tick(/*is_authority=*/false);
    CHECK(w.explosions.queue.empty());
    CHECK(w.out.destruction.explosions_processed == 1);

    // Control: a plain non-authority world OUTSIDE an MP session (tests,
    // pre-join states) keeps the authority-only drain.
    auto sp_heap = std::make_unique<World>();
    World &sp = *sp_heap;
    seed_ammo(sp);
    sp.registry.configure_pool(2, 8);
    Entity sp_seed = seed;
    const EntityHandle sp_barrel = sp.registry.spawn(2, sp_seed);
    sp.tables.item_death_traits.set(500, barrel_traits());
    Entity *sb = sp.registry.get(sp_barrel);
    sb->health = 0;
    destruction_notify_item_damage(sp, *sb, 4);
    CHECK(sp.explosions.queue.size() == 1);
    sp.run_logic_tick(/*is_authority=*/false);
    CHECK(sp.explosions.queue.size() == 1); // still parked: not a visual client
}

// Four retained effect handles follow independent points on the selected husk.
void test_death_effect_banks_and_water_crossings() {
	auto heap = std::make_unique<World>();
	World &w = *heap;
	Entity e;
	e.position = { 10, 20, 1 };
	e.yaw = 90;
	e.net_id = 8;
	auto traits = barrel_traits();
	traits.effect_banks[0].mask = 3;
	traits.effect_banks[0].points = { { { 2, 0, 1 }, { 0, 0, 1 } },
		{ { -2, 0, -2 }, { 0, 0, 1 } } };
	traits.effect_banks[1].mask = 3;
	traits.effect_banks[1].points = { { { 1, 0, -2 }, { 0, 0, 1 } },
		{ { -1, 0, 2 }, { 0, 0, 1 } } };
	spawn_death_effect_banks(e, traits, false, w.out.destruction);
	CHECK(w.out.destruction.effects.size() == 4);
	CHECK(e.death_effect_active[0] == 3 && e.death_effect_active[1] == 3);
	CHECK(w.out.destruction.effects[0].pos.x == 12);
	CHECK(w.out.destruction.effects[1].bank_slot == 1);
	w.out.destruction.effects.clear();
	update_dead_wreck_effects(w, e, &traits, 0, w.out.destruction);
	CHECK(e.death_effect_active[0] == 1 && e.death_effect_active[1] == 2);
	int steam = 0, stopped = 0;
	for (const auto &event : w.out.destruction.effects) {
		steam += event.effect == "Effect_Boat01Steam";
		stopped += event.release;
		if (event.effect == "Effect_Boat01Steam")
			CHECK(event.pos.z == -1);
	}
	CHECK(steam == 1 && stopped == 2);
	e.position.z = 4;
	w.out.destruction.effects.clear();
	update_dead_wreck_effects(w, e, &traits, 0, w.out.destruction);
	CHECK(e.death_effect_active[0] == 1); // ordinary smoke never restarts
	CHECK(e.death_effect_active[1] == 3); // fire re-emerges independently
	release_death_effect_bank(e, 2, w.out.destruction);
	CHECK(e.death_effect_active[1] == 0);
	traits.effect_banks[0] = {};
	traits.effect_banks[1] = {};
	w.out.destruction.effects.clear();
	spawn_death_effect_banks(e, traits, true, w.out.destruction);
	CHECK(w.out.destruction.effects.size() == 1); // only death has origin fallback
	CHECK(!e.death_effect_underwater); // absent water effect uses the ordinary family
	traits.husk_model_loaded = false;
	w.out.destruction.effects.clear();
	spawn_death_effect_banks(e, traits, false, w.out.destruction);
	CHECK(w.out.destruction.effects.empty());
}

// The wreck-fire random crackle rolls in the SIM per tick on the engine PRNG
// stream (S12b): a burning wreck (husk + authored particlefire) eventually
// crackles — the effect event + the distance-delay-gated sound through the
// fire-sound queue; a fire-less wreck never does; an underwater wreck consumes
// draws (retail's order: PRNG before the water gate) but stays silent.
// [orig: Entity_UpdateDeadWreckEffects @ 0x493140 — the roll @ 0x4932bf,
//  effect @ 0x4932d1, sound @ 0x4932e2]
void test_wreck_fire_crackle_rolls_on_the_engine_prng() {
    FlatField flat(0x4000);
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    w.out.fire_sounds.set_listener(Vec3{8.0f, 8.0f, 0.0f}); // < 30 u: immediate
    w.registry.configure_pool(1, 4);
    ItemDeathTraits t = barrel_traits(); // authors particlefire ("Effect_Fire")
    t.static_death = true;
    w.tables.item_death_traits.set(700, t);
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.item_id = 700;
    seed.health = 0;
    seed.position = Vec3{8.0f, 8.0f, 4.0f};
    const EntityHandle h = w.registry.spawn(1, seed);
    Entity *e = w.registry.get(h);
    e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    e->alive = false;

    int ticks_to_first = 0;
    for (int i = 0; i < 20000 && w.out.destruction.crackles == 0; ++i) {
        tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
        ++ticks_to_first;
    }
    CHECK(w.out.destruction.crackles > 0);
    bool crackle_effect = false;
    for (const DestructionEffectEvent &fx : w.out.destruction.effects)
        if (fx.effect == kFireCrackleEffect) crackle_effect = true;
    CHECK(crackle_effect);
    const std::vector<ReadyFireSound> sounds = w.out.fire_sounds.drain();
    bool crackle_sound = false;
    for (const ReadyFireSound &s : sounds)
        if (s.set_name == kFireCrackleSound) crackle_sound = true;
    CHECK(crackle_sound);

    // A fire-less wreck never crackles over the same span.
    auto w2_heap = std::make_unique<World>();
    World &w2 = *w2_heap;
    w2.out.fire_sounds.set_listener(Vec3{8.0f, 8.0f, 0.0f});
    w2.registry.configure_pool(1, 4);
    ItemDeathTraits quiet = barrel_traits();
    quiet.particlefire.clear();
	quiet.effect_banks[1] = {};
	quiet.effect_banks[1] = {};
	quiet.static_death = true;
	w2.tables.item_death_traits.set(700, quiet);
    const EntityHandle h2 = w2.registry.spawn(1, seed);
    Entity *e2 = w2.registry.get(h2);
    e2->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    e2->alive = false;
    for (int i = 0; i < ticks_to_first + 100; ++i)
        tick_dead_items(w2, &flat.field, -1.0e9f, w2.out.destruction);
    CHECK(w2.out.destruction.crackles == 0);

    // Underwater: the draw is consumed BEFORE the water gate — the stream
    // advances, the crackle never fires [orig: the && order @ 0x4932bf].
    auto w3_heap = std::make_unique<World>();
    World &w3 = *w3_heap;
    w3.out.fire_sounds.set_listener(Vec3{8.0f, 8.0f, 0.0f});
    w3.registry.configure_pool(1, 4);
    ItemDeathTraits wet = barrel_traits();
    wet.static_death = true;
    w3.tables.item_death_traits.set(700, wet);
    const EntityHandle h3 = w3.registry.spawn(1, seed);
    Entity *e3 = w3.registry.get(h3);
    e3->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    e3->alive = false;
	const uint32_t state_before = w3.prng16_c_state;
	for (int i = 0; i < 64; ++i)
		tick_dead_items(w3, &flat.field, 100.0f, w3.out.destruction);
    CHECK(w3.out.destruction.crackles == 0);
	CHECK(w3.prng16_c_state != state_before);
}

// The debris-type trail column reads the ONE native table row
// [orig: g_death_piece_types @ 0x8404f0 +0x2C].
void test_death_piece_trail_effect_rows() {
    CHECK(std::string(death_piece_trail_effect(0)).empty());       // HULL
    CHECK(std::string(death_piece_trail_effect(1)) == "Effect_VexpM");
    CHECK(std::string(death_piece_trail_effect(2)) == "Effect_VexpS");
    CHECK(std::string(death_piece_trail_effect(5)) == "Effect_PDust_S");
    CHECK(std::string(death_piece_trail_effect(12)) == "Effect_VexpSL");
    CHECK(std::string(death_piece_trail_effect(13)).empty());      // out of range
    CHECK(std::string(death_piece_trail_effect(255)).empty());
}

// [orig: Entity_ProcessFallingDeathPhysics @0x461D30]
void test_generic_wreck_zero_velocity_and_inverted_rest() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 4);
	FlatField flat(0x4000);
	const float ground = opennova::terrain::height_field_height_world_bilinear(flat.field, 8, -8);
	Entity seed;
	seed.item_id = 705;
	seed.position = { 8, 8, ground + 10 };
	seed.engine_flags = kEntityFlagHusk | kEntityFlagDead;
	seed.death_motion = DeathMotionMode::Generic;
	auto h = w.registry.spawn(1, seed);
	Entity &e = *w.registry.get(h);
	tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
	CHECK(e.veh.slide_z == -334);
	CHECK(e.position.z < seed.position.z); // zero velocity does not suppress gravity
	ItemDeathTraits t;
	t.husk_rest_min_z = -0.25f;
	t.husk_rest_max_z = 2.0f;
	w.tables.item_death_traits.set(e.item_id, t);
	e.position = { 8, 8, ground + 1 };
	e.roll = 180;
	e.veh.yaw_seeded = false;
	e.veh.slide_z = -65536;
	e.veh.vel_x = 65536;
	tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
	CHECK(e.position.x == 8 && e.position.z == ground + 1);
	CHECK(e.roll == 180 && e.veh.slide_z == 0 && e.veh.vel_x == 65536);
	e.roll = 0;
	e.veh.slide_z = -65536;
	tick_dead_items(w, &flat.field, -1.0e9f, w.out.destruction);
	CHECK(e.position.x == 9 && e.position.z < ground + 1); // upright lower bound permits this step
}

// State-23 boundary and full live-state restoration through the public system.
static void test_vehicle_respawn_lifecycle() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 2);
	w.registry.configure_pool(2, 2);
	w.env.water_z = INT32_MIN;
	Entity seed;
	seed.item_id = 42;
	seed.position = { 10, 20, 30 };
	seed.yaw = 90;
	seed.team = 2;
	seed.health_max = 500;
	seed.item_attrib = kItemAttribPlayerControl;
	auto h = w.registry.spawn(1, seed);
	Entity &e = *w.registry.get(h);
	w.ai.attach(h);
	AiEntity &ai = *w.ai.for_handle(h);
	ItemDeathTraits death;
	death.static_death = true;
	w.tables.item_death_traits.set(seed.item_id, death);
	w.vehicles.capture_spawn_pose(e);
	e.health = 0;
	e.alive = false;
	e.flags = e.engine_flags = kEntityFlagHusk | kEntityFlagDead | kEntityFlagReflective;
	e.team = 0;
	e.position = { 40, 50, 60 };
	e.death_motion = DeathMotionMode::Generic;
	e.section_mask = 7;
	e.spawned_piece_mask = 15;
	e.veh.crashed = 1;
	e.veh.wheel_comp[5] = 900;
	ai.brain.f[AiBrain::kCurState] = 23;
	ai.slot.f[37] = 4;
	ai.slot.f[38] = 5;
	for (int i = 0; i < 15; ++i)
		w.vehicles.tick_dead(e, ai);
	CHECK(e.health == 0 && e.veh.stuck_ticks == 15);
	ai.inf.wait_cooldown = 1;
	w.vehicles.tick_dead(e, ai);
	CHECK(e.health == 0 && ai.inf.wait_cooldown == 1);
	// The class init re-run at the head of respawn: the profile speeds over any
	// command-set speed, the ammo refilled from a resolved block (an unresolved
	// block empties), the turret words, step 16 then the dead state's enter
	// (step 62, the group alert), the 0..15 think stagger and the rotor words
	// dropped; the respawn body clears the kill credit.
	// [orig: Entity_RespawnVehicle @0x45FF53..0x45FF73 -> Entity_InitVehicleAIFromDef
	//  @0x46885B..0x468964; +0x178 = 0 @0x460074]
	ai.profile.class_speed_a = 111;
	ai.profile.class_speed_b = 222;
	ai.brain.f[AiBrain::kSpeedA] = 5;
	ai.brain.f[AiBrain::kSpeedB] = 6;
	ai.profile.fire_a.ammo_index = 3;
	ai.profile.fire_a.ammo_cap = 7;
	ai.profile.fire_a.flags = 1;
	ai.profile.fire_a.facing_bam = 4242;
	ai.profile.fire_b.ammo_index = 0;
	ai.profile.fire_b.ammo_cap = 9;
	ai.brain.f[AiBrain::kAmmoA] = 0;
	ai.brain.f[AiBrain::kAmmoB] = 4;
	ai.brain.f[AiBrain::kStep] = 1;
	w.vehicle_ai_spawn_phase = 15;
	e.last_attacker = h;
	e.veh.part_spin.rate = 500;
	e.veh.part_spin.speed = 600;
	ai.inf.wait_cooldown = 2; // a value above one decrements AND proceeds
	w.vehicles.tick_dead(e, ai);
	CHECK(ai.inf.wait_cooldown == 1 && e.health == 500 && ai.health == 500);
	CHECK(ai.brain.f[AiBrain::kSpeedA] == 111 && ai.brain.f[AiBrain::kSpeedB] == 222);
	CHECK(ai.brain.f[AiBrain::kAmmoA] == 7 && ai.brain.f[AiBrain::kAmmoB] == 0);
	CHECK(ai.brain.f[AiBrain::kActiveYaw] == 4242 &&
			ai.brain.f[AiBrain::kStagingBlock + 3] == 4242);
	CHECK(ai.brain.f[AiBrain::kStep] == 62);
	CHECK(ai.brain.f[AiBrain::kAlert] == 2);
	CHECK(e.spawn_phase == 15 && w.vehicle_ai_spawn_phase == 0);
	CHECK(!e.last_attacker.valid());
	CHECK(e.veh.part_spin.rate == 0 && e.veh.part_spin.speed == 0);
	CHECK(e.position.x == 10 && e.position.y == 20 && e.position.z == 30);
	CHECK(e.alive && e.team == 2 && ai.team == 2);
	CHECK(e.engine_flags == 0x22400 && e.flags == e.engine_flags);
	CHECK(e.death_motion == DeathMotionMode::None && e.section_mask == 0);
	CHECK(e.spawned_piece_mask == 0 && e.veh.crashed == 0 && e.veh.fresh_2f1 == 1);
	CHECK(e.veh.wheel_comp[5] == 0 && e.veh.slide_z == -501);
	CHECK(ai.brain.f[AiBrain::kPendState] == 22);
	CHECK(ai.brain.f[AiBrain::kWpType] == 1 && ai.brain.f[AiBrain::kWpChannel] == 4);
	CHECK(ai.brain.f[AiBrain::kWpNode] == 5 && e.saved_live_valid);

	Entity support_seed;
	support_seed.position = { 8, 20, 30 };
	support_seed.yaw = 90;
	const auto support_h = w.registry.spawn(2, support_seed);
	Entity &support = *w.registry.get(support_h);
	e.ground_target = support_h;
	w.vehicles.capture_spawn_pose(e); // two units along the carrier's local X
	support.position = { 18, 20, 30 };
	support.yaw = 0;
	int32_t pose[6];
	CHECK(w.vehicles.resolve_spawn_pose(e, pose));
	CHECK(std::abs(pose[0] - 18 * 65536) < 16 && std::abs(pose[1] - 22 * 65536) < 16);
	e.position = { float(pose[0]) / 65536, float(pose[1]) / 65536, float(pose[2]) / 65536 };
	CHECK(vehicle_at_spawn_anchor(w, e));
	support.engine_flags = kEntityFlagDead;
	CHECK(!w.vehicles.resolve_spawn_pose(e, pose));
	CHECK(!vehicle_at_spawn_anchor(w, e));
	e.health = 0;
	ai.inf.wait_cooldown = 0;
	w.vehicles.tick_dead(e, ai);
	CHECK(e.health == 0); // a dead spawn support defers respawn
	w.ai.is_authority = false;
	w.rules.vehicle_respawns = false;
	w.vehicles.tick_dead(e, ai);
	CHECK(w.registry.get(h) != nullptr && w.out.entity_events.empty());
	w.ai.is_authority = true;
	w.vehicles.tick_dead(e, ai);
	CHECK(w.registry.get(h) == nullptr && w.out.entity_events.size() == 1);
	CHECK(std::get<EntityRemoveEvent>(w.out.entity_events[0]).handle == h.packed);
}

// A `Parent` vehicle's death kills the gunner children the class init listed
// in its brain's +576/+580 block — the other pool-1 entities sharing its
// nonzero refNum — through the child's class death callback; a peer with
// another refNum and an item with no refNum are untouched.
// [orig: Entity_SetupGunnerAttachments @0x468130..0x468173 (the list);
//  AI_TransitionToDeath_GroundVehicle @0x467B6E..0x467BBB (the kill walk)]
static void test_vehicle_death_kills_authored_children() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 8);
	Entity seed;
	seed.kind = EntityKind::Item;
	seed.item_id = 700;
	seed.ref_num = 9;
	seed.health = 0;
	seed.is_ai_capable = true;
	const auto parent_h = w.registry.spawn(1, seed);
	w.ai.attach(parent_h);
	VehicleTraits traits;
	traits.family = VehicleFamily::Ground;
	traits.attrib_parent = true;
	w.vehicles.traits.set(700, traits);
	seed.item_id = 0;
	seed.health = 100;
	seed.is_ai_capable = false;
	const auto child_h = w.registry.spawn(1, seed); // refNum 9: listed
	seed.ref_num = 10;
	const auto other_ref_h = w.registry.spawn(1, seed); // another refNum: not listed
	seed.ref_num = 0;
	const auto unrelated_h = w.registry.spawn(1, seed);
	w.vehicles.setup_gunner_attachments(*w.registry.get(parent_h));
	auto &brain = *w.ai.for_handle(parent_h);
	CHECK(brain.brain.f[AiBrain::kAttachCount] == 1);
	brain.brain.f[AiBrain::kCurState] = 22;
	w.vehicles.tick_health(*w.registry.get(parent_h), traits);
	CHECK(brain.brain.f[AiBrain::kPendState] == 21);
	w.ai.apply_transition(brain, w);
	CHECK(w.registry.get(child_h)->health == 0);
	CHECK((w.registry.get(child_h)->engine_flags & kEntityFlagHusk) != 0);
	CHECK(w.registry.get(other_ref_h)->health == 100);
	CHECK(w.registry.get(unrelated_h)->health == 100);
}

// Exercise the public motor -> AI event -> aircraft death -> respawn path.
static void test_aircraft_death_lifecycle() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 4);
	w.env.water_z = INT32_MIN;
	w.logic_tick = 17;
	Entity seed;
	seed.kind = EntityKind::Item;
	seed.item_id = 42;
	seed.is_ai_capable = true;
	seed.item_attrib = kItemAttribPlayerControl;
	seed.health = seed.health_max = 500;
	seed.team = 2;
	seed.position = { 10, 20, 30 };
	const auto h = w.registry.spawn(1, seed);
	w.ai.attach(h);
	auto &e = *w.registry.get(h);
	auto &ai = *w.ai.for_handle(h);
	ai.brain.f[AiBrain::kCurState] = ai.brain.f[AiBrain::kPendState] = 14;
	w.vehicles.capture_spawn_pose(e);
	e.health = 0;
	VehicleTraits traits;
	traits.family = VehicleFamily::Helicopter;
	w.vehicles.tick_health(e, traits);
	CHECK(ai.brain.f[AiBrain::kPendState] == 13);
	w.ai.apply_transition(ai, w);
	CHECK(ai.brain.f[AiBrain::kCurState] == 13 && ai.brain.f[AiBrain::kStep] == 16);
	CHECK(ai.brain.f[138] == 1336 && (e.engine_flags & 6) == 6);
	CHECK(e.death_motion == DeathMotionMode::PiecePhysics);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	// The initializer stamps savedLivePose. An unchanged pose queues event four.
	e.veh.slide_z = -500;
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(e.veh.slide_z == 0);
	w.ai.events.process_timed(w.ai, w);
	CHECK(ai.brain.f[AiBrain::kCurState] == 15 && ai.brain.f[AiBrain::kStep] == 62);
	CHECK(e.team == 0 && e.death_tick == 17);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	for (int tick = 0; tick < 30; ++tick)
		w.vehicles.tick_dead(e, ai);
	CHECK(e.health == 0 && e.veh.stuck_ticks == 30);
	w.vehicles.tick_dead(e, ai);
	CHECK(e.health == 500 && e.alive && e.team == 2);
	CHECK(ai.brain.f[AiBrain::kPendState] == 14);
	CHECK(e.death_motion == DeathMotionMode::None);
	CHECK(w.out.destruction.husk_swaps.back().restore_intact);
	// Boat idle follows the water plane and uses event three, not event five.
	ai.brain.f[AiBrain::kCurState] = ai.brain.f[AiBrain::kPendState] = 22;
	ai.profile.subtype = 1;
	ai.brain.f[AiBrain::kPrevAlert] = ai.brain.f[AiBrain::kAlert];
	w.env.water_z = 7 * 65536;
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(ai.brain.f[AiBrain::kWorkPosZ] == 7 * 65536);
	CHECK(ai.brain.f[AiBrain::kWorkPosX] == 10 * 65536);
	e.health = 0;
	e.veh.vel_x = 0;
	w.ai.process_air_state_machine(ai, w, 0);
	w.ai.events.process_timed(w.ai, w);
	CHECK(ai.brain.f[AiBrain::kCurState] == 23);
}

// The aircraft death spin rates in retail's evaluation order
// [orig: Entity_InitDeathState @0x48F98A/@0x48F9A4 -> Death_RandomSpinRateBam
//  @0x57B940]. Hand-derived from the rol-xor stream seeded 0x200: the first draw
// is state rol4(0x200 + rol11(0x200)) ^ 1 = 0x01002001 -> low16 0x2001 = 8193,
// r = 93; floor = ftol(0.45f * 11930464.0f) = ftol(0.449999988 * 11930464) =
// 5368708; max = ftol(0.65f * 11930464.0f) = ftol(0.649999976 * 11930464) =
// 7754801; t = 93 * 0.01f stays extended = 93 * 10737418 * 2^-30; v =
// ftol(t * 7754801) = 7211964 (the float-rounded evaluation the port used to do
// gives 7211966). The second draw is state 0x20028091 -> low16 0x8091 = 32913,
// r = 13; floor = ftol(0.15f * 11930464) = 1789569; max = ftol(0.35f * 11930464)
// = 4175662; v = ftol(13 * 0.01f * 4175662) is about 542.8k < floor, so the
// pitch rate is -1789569. No death-piece roll precedes them for a hull without
// death traits (Entity_SpawnDeathPieces returns at its husk gate).
static void test_aircraft_death_spin_rates_roll_in_retail_order() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 4);
	w.env.water_z = INT32_MIN;
	Entity seed;
	seed.kind = EntityKind::Item;
	seed.item_id = 4242;
	seed.health = 0;
	seed.position = { 10, 20, 30 };
	const auto h = w.registry.spawn(1, seed);
	Entity &e = *w.registry.get(h);
	w.destruction_rng.state = 0x200u;
	entity_init_aircraft_death(w, e, false);
	CHECK(e.veh.air_roll_rate == 7211964);
	CHECK(e.veh.air_pitch_rate == -1789569);
	CHECK(w.destruction_rng.state == 0x20028091u);
}

// The spawn-marker pass is one of the server tick's every-32 legs: it runs
// after the WAC tick under the same script admission, so an empty host with a
// live WAC clock holds it, and only ticks on a 32 boundary run it.
// [orig: Server_TickUpdate — the admission @0x51D89F..0x51D8BD, `test
//  tick,1Fh` @0x51D8C4, the assign_overlay_spawn_points call @0x51D8D2]
static void test_spawn_markers_ride_the_script_admission() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 8);
	w.registry.configure_pool(2, 4);
	w.registry.configure_pool(3, 8);
	w.add_system(&w.server_idle_legs);
	Entity seed;
	seed.has_item_def = true;
	seed.item_type = 5;
	seed.item_attrib = 0x60000;
	seed.zone_control = 65536;
	seed.zone_number = 1;
	seed.team = 1;
	seed.position = { 10, 0, 0 };
	(void)w.registry.spawn(2, seed);
	seed.zone_number = 2;
	seed.team = 2;
	seed.position = { 20, 0, 0 };
	(void)w.registry.spawn(2, seed);
	seed = Entity{};
	seed.has_item_def = true;
	seed.item_type = 4;
	seed.item_attrib2 = 4;
	seed.vehicle_spawn_ids = { 100042 };
	seed.zone_number = 1;
	seed.position = { 10, 0, 3 };
	(void)w.registry.spawn(3, seed);
	seed = Entity{};
	seed.has_item_def = true;
	seed.item_type = 1;
	seed.item_id = 42;
	seed.vehicle_spawn_team = 1;
	seed.team = 1;
	seed.bound_radius = 2;
	seed.position = { 100, 100, 0 };
	const auto vh = w.registry.spawn(1, seed);
	Entity &v = *w.registry.get(vh);
	w.vehicles.capture_spawn_pose(v);
	w.vehicles.build_spawn_markers();
	v.engine_flags = 6;
	v.veh.stuck_ticks = 1;

	// No human while the WAC clock is live: the leg is held.
	w.cached.humans = 0;
	w.cached.wac_ticks = 1;
	w.logic_tick = 32;
	w.run_logic_tick(true, TickPhase::Gameplay);
	CHECK(v.veh.spawn_pose[0] == 100 * 65536);
	// A human admits it on the next 32 boundary, not in between.
	w.cached.humans = 1;
	w.logic_tick = 33;
	w.run_logic_tick(true, TickPhase::Gameplay);
	CHECK(v.veh.spawn_pose[0] == 100 * 65536);
	w.logic_tick = 64;
	w.run_logic_tick(true, TickPhase::Gameplay);
	CHECK(v.veh.spawn_pose[0] == 10 * 65536 && v.veh.spawn_pose[2] == 3 * 65536);
}

// Captured AS zones select a category-compatible marker in team frontier order.
// Occupied best markers defer instead of falling back to a lower priority marker.
static void test_vehicle_spawn_marker_selection() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 8);
	w.registry.configure_pool(2, 4);
	w.registry.configure_pool(3, 8);
	Entity seed;
	seed.has_item_def = true;
	seed.item_type = 5;
	seed.item_attrib = 0x60000;
	seed.zone_control = 65536;
	seed.zone_number = 1;
	seed.team = 1;
	seed.position = { 10, 0, 0 };
	const auto z1 = w.registry.spawn(2, seed);
	seed.zone_number = 2;
	seed.team = 2;
	seed.position = { 20, 0, 0 };
	const auto z2 = w.registry.spawn(2, seed);
	seed = Entity{};
	seed.has_item_def = true;
	seed.item_type = 4;
	seed.item_attrib2 = 4;
	seed.vehicle_spawn_ids = { 100042 };
	seed.zone_number = 1;
	seed.position = { 10, 0, 3 };
	const auto m1 = w.registry.spawn(3, seed);
	seed.zone_number = 2;
	seed.position = { 20, 0, 4 };
	const auto m2 = w.registry.spawn(3, seed);
	seed = Entity{};
	seed.has_item_def = true;
	seed.item_type = 1;
	seed.item_id = 42;
	seed.vehicle_spawn_team = 1;
	seed.team = 1;
	seed.bound_radius = 2;
	seed.position = { 100, 100, 0 };
	const auto vh = w.registry.spawn(1, seed);
	auto &v = *w.registry.get(vh);
	w.vehicles.capture_spawn_pose(v);
	w.vehicles.build_spawn_markers();
	v.engine_flags = 6;
	v.veh.stuck_ticks = 1;
	w.vehicles.tick_spawn_markers();
	CHECK(!v.veh.respawn_waiting_for_overlay);
	CHECK(v.veh.spawn_pose[0] == 10 * 65536 && v.veh.spawn_pose[2] == 3 * 65536);
	w.registry.get(z2)->team = 1; // capture and secure the forward zone
	w.vehicles.tick_spawn_markers();
	CHECK(v.veh.spawn_pose[0] == 20 * 65536);
	w.registry.get(z2)->zone_control = 65535;
	w.vehicles.tick_spawn_markers();
	CHECK(v.veh.spawn_pose[0] == 10 * 65536);
	w.registry.get(z2)->zone_control = 65536;
	seed.position = { 21, 0, 0 };
	seed.vehicle_spawn_team = 0;
	seed.engine_flags = 6;
	const auto blocking = w.registry.spawn(1, seed);
	w.vehicles.tick_spawn_markers();
	CHECK(v.veh.respawn_waiting_for_overlay); // dead hull blocks clearance
	CHECK(v.veh.spawn_pose[0] == 10 * 65536); // no second-best retry
	w.registry.get(blocking)->engine_flags = 0;
	w.vehicles.tick_spawn_markers();
	CHECK(!v.veh.respawn_waiting_for_overlay); // live hull blocks first-pass eligibility
	CHECK(v.veh.spawn_pose[0] == 10 * 65536);
	w.registry.get(m1)->vehicle_spawn_ids = { 100043 }; // wrong category
	w.vehicles.tick_spawn_markers();
	CHECK(v.veh.respawn_waiting_for_overlay);
	w.registry.get(blocking)->position = { 100, 100, 0 };
	v.veh.stuck_ticks = 0;
	w.vehicles.tick_spawn_markers();
	CHECK(v.veh.respawn_waiting_for_overlay); // the initial dead tick has not run
	v.veh.stuck_ticks = 1;
	w.vehicles.tick_spawn_markers();
	CHECK(!v.veh.respawn_waiting_for_overlay && !v.veh.spawn_support.valid());
	CHECK(v.veh.spawn_pose[0] == 20 * 65536);
	(void)z1;
	(void)m2;
}

static void test_aircraft_landing_and_navigation_states() {
	auto storage = std::make_unique<World>();
	World &w = *storage;
	w.registry.configure_pool(1, 2);
	FlatField flat(0);
	w.tables.terrain = &flat.field;
	w.ai.terrain = &flat.field;
	w.env.water_z = INT32_MIN;
	Entity seed;
	seed.position = { 10, 10, 10 };
	seed.is_ai_capable = true;
	const auto h = w.registry.spawn(1, seed);
	w.ai.attach(h);
	auto &e = *w.registry.get(h);
	auto &ai = *w.ai.for_handle(h);
	auto &b = ai.brain;
	ai.profile.type = 1;
	ai.profile.patrol_climb = 6000;
	b.f[AiBrain::kPendState] = 6;
	w.ai.apply_transition(ai, w);
	CHECK(b.f[AiBrain::kCurState] == 6 && b.f[AiBrain::kStep] == 8 && b.f[138] == 3000);
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(b.f[138] == 3000);
	e.position.z = 4;
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(b.f[138] == 2114);
	e.position.z = -1;
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(b.f[AiBrain::kCurState] == 14 && e.position.z == 0);
	b.f[AiBrain::kPendState] = 7;
	w.ai.apply_transition(ai, w);
	ai.pos[0] = 10 * 65536;
	ai.pos[1] = 10 * 65536;
	b.f[AiBrain::kWpType] = 3;
	b.f[AiBrain::kWpCoordX] = 100 * 65536;
	b.f[AiBrain::kWpCoordY] = 10 * 65536;
	b.f[AiBrain::kWpCoordZ] = 30 * 65536;
	b.f[AiBrain::kWpCoordSrc] = 65536;
	b.f[AiBrain::kSpeedB] = 1000;
	b.f[AiBrain::kSpeedA] = 2000;
	ai.profile.patrol_altitude = 12 * 65536;
	ai.profile.field216 = 20 * 65536;
	ai.profile.field220 = 8000;
	ai.profile.min_agl = 5 * 65536;
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(b.f[AiBrain::kWorkPosZ] == 12 * 65536 && b.f[138] == 6000);
	CHECK(b.f[AiBrain::kOutSpeed] == 1000 && b.f[AiBrain::kWorkPosX] == 100 * 65536);
	b.f[AiBrain::kUseWaypointZones] = 1;
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(b.f[AiBrain::kWorkPosZ] == 30 * 65536);
	b.f[AiBrain::kUseWaypointZones] = 0;
	b.f[AiBrain::kPendState] = 11;
	w.ai.apply_transition(ai, w);
	w.ai.process_air_state_machine(ai, w, 0);
	CHECK(b.f[AiBrain::kWorkPosZ] == 20 * 65536 && b.f[138] == 8000);
	CHECK(b.f[AiBrain::kOutSpeed] == 2000);
}

// ---------------------------------------------------------------------------
// The ai_function class dispatch (D-ITEM-7): the items.def fold stamps the
// event-callback row on the death traits and destruction_notify_item_damage
// runs THAT row's body, not the tree body for every item.
// [orig: g_EntityClassEventCallbackTable @0x813000 — gnrc 0x407020, gnrl
//  0x407F80, gnl2 0x4070F0, tree 0x440210, ewep 0x4409A0, null 0x406FF0]
// ---------------------------------------------------------------------------

ItemDeathTraits class_traits(ItemDeathClass cls) {
	ItemDeathTraits t = barrel_traits();
	t.death_class = cls;
	return t;
}

EntityHandle spawn_prop(World &w, int pool, int32_t item_id, int32_t health) {
	Entity seed;
	seed.kind = pool == 2 ? EntityKind::Building : EntityKind::Item;
	seed.item_id = item_id;
	seed.health = health;
	seed.position = Vec3{10.0f, 0.0f, 0.0f};
	seed.bound_radius = 1.0f;
	return w.registry.spawn(pool, seed);
}

int count_sound(const World &w, const char *name) {
	int n = 0;
	for (const DestructionSoundEvent &s : w.out.destruction.sounds)
		if (s.sound == name) ++n;
	return n;
}

int count_effect(const World &w, const char *name, uint8_t family) {
	int n = 0;
	for (const DestructionEffectEvent &fx : w.out.destruction.effects)
		if (fx.effect == name && fx.family == family) ++n;
	return n;
}

int count_active_pieces(const World &w) {
	int n = 0;
	for (const DeathPiece &p : w.death_pieces.pieces)
		if (p.active) ++n;
	return n;
}

// gnrl (the shipped WpnCt crates, Radio04, the hp-10 decorations): scar clear,
// Flags |= 6 at once, the death tick, the def death sound and ONE transient
// particledeath effect. No section debris and no Entity_InitDeathSounds
// bank/KZ chain — the kz_OrganicBlast an ammo crate used to detonate with came
// from the tree body every item ran. [orig: sub_407F80 — the Flags&4 gate
// @0x407f89, the alive re-arm @0x4080ba, the kill leg @0x4080bd..0x4080f4,
// the tail @0x408044/@0x40806b]
void test_gnrl_death_is_husk_sound_and_one_effect() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(1, 8);
	w.logic_tick = 100;
	const EntityHandle h = spawn_prop(w, 1, 500, 104);
	ItemDeathTraits traits = class_traits(ItemDeathClass::kGnrl);
	traits.kz_points = {Vec3{0.0f, 0.0f, 1.0f}}; // a KZ point the tree body would blast
	w.tables.item_death_traits.set(500, traits);
	Entity *b = w.registry.get(h);

	// Alive: a nonlethal notify only re-arms the think.
	b->health = 50;
	destruction_notify_item_damage(w, *b, 1);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) == 0);
	CHECK(b->alive);

	b->health = 0;
	destruction_notify_item_damage(w, *b, 1);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
			(kEntityFlagDead | kEntityFlagHusk));
	CHECK(!b->alive);
	CHECK(b->death_tick == 100);
	CHECK(w.out.destruction.items_destroyed == 1);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 0) == 1); // transient, unattached
	CHECK(count_effect(w, "Effect_LrgOrdExp", 1) == 0); // no Dead-bone bank
	CHECK(count_effect(w, "Effect_Fire", 2) == 0);      // no fire bank
	CHECK(w.out.destruction.debris_triangles == 0);
	CHECK(w.explosions.queue.empty()); // no kz chain blast
	if (!w.out.destruction.effects.empty()) {
		const DestructionEffectEvent &fx = w.out.destruction.effects[0];
		CHECK(fx.attach_net_id == 0 && fx.attach_wire_handle == EntityHandle::kInvalid);
		CHECK(std::abs(fx.pos.x - 10.0f) < 1.0e-6f);
	}

	// Husked: every later notify meets the Flags&4 gate, and the class think
	// expiry has nothing to add for gnrl.
	destruction_notify_item_damage(w, *b, 2);
	CHECK(w.out.destruction.items_destroyed == 1);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	w.logic_tick = 140;
	step_item_pool(w, h.pool());
	CHECK(w.out.destruction.items_destroyed == 1);
	CHECK(w.explosions.queue.empty());
}

// The gnrl client kill leg: a non-authority peer acts on the S2C 0x13 phase
// alone and lands the same husk + sound + one effect locally.
// [orig: @0x407fff..0x408074]
void test_gnrl_client_kill_leg() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(1, 8);
	w.rules.logic_authority = false;
	w.logic_tick = 7;
	const EntityHandle h = spawn_prop(w, 1, 500, 104);
	w.tables.item_death_traits.set(500, class_traits(ItemDeathClass::kGnrl));
	Entity *b = w.registry.get(h);
	b->health = 0;
	// A client damage notify is not a kill.
	destruction_notify_item_damage(w, *b, 1);
	destruction_notify_item_damage(w, *b, 2);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) == 0);
	CHECK(w.out.destruction.husk_swaps.empty());
	destruction_notify_item_damage(w, *b, 4);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
			(kEntityFlagDead | kEntityFlagHusk));
	CHECK(!b->alive);
	CHECK(b->death_tick == 7);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 0) == 1);
	CHECK(w.explosions.queue.empty());
}

// gnrc (the shipped oil tanks, dishes, generators, the wooden bridge): the
// first authority call marks the entity dead WITHOUT the husk and arms a
// four-tick think; the expiry runs Entity_UpdateDeathTransforms — the
// unitType piece dispatch, the husk, Entity_InitDeathSounds' sound, banks
// and kz blast — then the 0x26 resend and Flags |= 4.
// [orig: sub_407020 — @0x4070b2..0x4070e2 (Flags |= 2, +0x2AC = 4), the
//  Flags&2 leg @0x407072..0x4070a6; Entity_UpdatePool1Slot @0x4b8e1b gate]
void test_gnrc_death_lands_husk_on_pool2_cohort() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(2, 8);
	w.logic_tick = 200;
	const EntityHandle h = spawn_prop(w, 2, 500, 200);
	w.tables.item_death_traits.set(500, class_traits(ItemDeathClass::kGnrc));
	Entity *b = w.registry.get(h);
	b->health = 0;
	destruction_notify_item_damage(w, *b, 2);
	CHECK((b->engine_flags & kEntityFlagDead) != 0);
	CHECK((b->engine_flags & kEntityFlagHusk) == 0);
	CHECK(!b->alive);
	CHECK(b->death_tick == 200);
	CHECK(w.out.destruction.husk_swaps.empty());
	CHECK(w.out.destruction.items_destroyed == 0);
	CHECK(w.out.destruction.sounds.empty());
	CHECK(w.out.destruction.effects.empty());
	CHECK(w.explosions.queue.empty());
	CHECK(count_active_pieces(w) == 0);

	// Pool 2 subtracts 8 on its first visit, then runs the expired clock on the next.
	for (uint32_t t = 200; t < 208; ++t) {
		w.logic_tick = t;
		step_item_pool(w, h.pool());
		CHECK((b->engine_flags & kEntityFlagHusk) == 0);
	}
	w.logic_tick = 208;
	step_item_pool(w, h.pool());
	CHECK((b->engine_flags & kEntityFlagHusk) != 0);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	CHECK(w.out.destruction.items_destroyed == 1);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 1) == 1); // the Dead-bone bank
	CHECK(w.explosions.queue.size() == 1);              // the kz chain blast
	if (w.explosions.queue.size() == 1)
		CHECK(w.explosions.queue[0].radius_override == 4.0f);
	CHECK(count_active_pieces(w) == 3); // husk sections 1..3
	CHECK(b->spawned_piece_mask == 0xEu);
	CHECK(b->death_motion == DeathMotionMode::Generic);

	// Idempotent afterwards: the later think re-arms and the husked notify
	// meets its gate.
	w.logic_tick = 240;
	step_item_pool(w, h.pool());
	destruction_notify_item_damage(w, *b, 1);
	CHECK(w.out.destruction.items_destroyed == 1);
	CHECK(w.explosions.queue.size() == 1);
	CHECK(count_active_pieces(w) == 3);
}

// The gnrc client kill leg runs the whole Entity_UpdateDeathTransforms chain
// at once on phase 4 [orig: @0x40702a..0x407062]; a client damage notify is
// inert.
void test_gnrc_client_kill_runs_death_transforms_at_once() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(2, 8);
	w.rules.logic_authority = false;
	w.logic_tick = 9;
	const EntityHandle h = spawn_prop(w, 2, 500, 200);
	w.tables.item_death_traits.set(500, class_traits(ItemDeathClass::kGnrc));
	Entity *b = w.registry.get(h);
	b->health = 0;
	destruction_notify_item_damage(w, *b, 1);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) == 0);
	destruction_notify_item_damage(w, *b, 4);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
			(kEntityFlagDead | kEntityFlagHusk));
	CHECK(!b->alive);
	CHECK(b->death_tick == 9);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	CHECK(w.explosions.queue.size() == 1);
	CHECK(count_active_pieces(w) == 3);
	// Client callbacks run too; the completed husk remains inert.
	w.logic_tick = 60;
	step_item_pool(w, h.pool());
	CHECK(w.out.destruction.items_destroyed == 1);
}

// ewep (a standalone B50cal / minigun, a tank's turret child): the authority
// kill leg dismounts the seated gunner BEFORE the husk flags land — only the
// occupant whose mount target IS this emplacement — then scar clear, Flags |=
// 6, the death tick, the death sound and one transient effect; no kz chain.
// [orig: Entity_UpdateChildAttachment @0x4409A0 — @0x440c0d..0x440c1a the
//  parentEntity check + Entity_DetachFromVehicleIfServer, the kill leg
//  @0x440c23..0x440c87]
void test_ewep_death_dismounts_gunner_before_husk() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(1, 8);
	w.logic_tick = 50;
	const EntityHandle gun_h = spawn_prop(w, 1, 700, 10);
	const EntityHandle other_h = spawn_prop(w, 1, 700, 10);
	w.tables.item_death_traits.set(700, class_traits(ItemDeathClass::kEwep));
	Entity gunner_seed;
	gunner_seed.kind = EntityKind::Organic;
	gunner_seed.health = 100;
	gunner_seed.position = Vec3{10.0f, 0.0f, 1.0f};
	const EntityHandle gunner_h = w.registry.spawn(0, gunner_seed);

	Entity *gun = w.registry.get(gun_h);
	Entity *gunner = w.registry.get(gunner_h);
	Seat use_gun;
	use_gun.type = SeatType::Gunner;
	use_gun.occupant = gunner_h;
	gun->seats.push_back(use_gun);
	gunner->mounted = true;
	gunner->mount_target = gun_h;
	gunner->mount_seat = 0;
	gunner->mount_type = SeatType::Gunner;
	gun->primary_occupant = gunner_h;

	// A second emplacement whose occupant slot names a rider seated ELSEWHERE
	// leaves that rider alone [orig: the parentEntity == entity check @0x440c11].
	Entity *other = w.registry.get(other_h);
	other->primary_occupant = gunner_h;
	other->health = 0;
	destruction_notify_item_damage(w, *other, 1);
	CHECK(gunner->mounted && gunner->mount_target == gun_h);
	CHECK((other->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
			(kEntityFlagDead | kEntityFlagHusk));

	gun->health = 0;
	destruction_notify_item_damage(w, *gun, 1);
	CHECK(!gunner->mounted);
	CHECK(!gunner->mount_target.valid());
	CHECK(gunner->mount_seat == -1);
	CHECK(!gun->seats[0].occupant.valid());
	CHECK(!gun->primary_occupant.valid());
	CHECK((gun->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
			(kEntityFlagDead | kEntityFlagHusk));
	CHECK(!gun->alive);
	CHECK(gun->death_tick == 50);
	CHECK(w.out.destruction.items_destroyed == 2);
	CHECK(w.out.destruction.husk_swaps.size() == 2);
	CHECK(count_sound(w, "EXPLO_BARREL") == 2);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 0) == 2);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 1) == 0);
	CHECK(w.explosions.queue.empty());
	CHECK(w.out.destruction.debris_triangles == 0);
	// Husked: the entry gate only re-arms.
	destruction_notify_item_damage(w, *gun, 2);
	CHECK(w.out.destruction.items_destroyed == 2);
}

// The ewep client kill leg performs NO detach: a joiner's gunner is
// dismounted by the server's own detach packet, its emplacement husks on
// phase 4 alone. [orig: @0x440b6c..0x440ba7]
void test_ewep_client_kill_keeps_gunner_mounted() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(1, 8);
	w.rules.logic_authority = false;
	w.logic_tick = 11;
	const EntityHandle gun_h = spawn_prop(w, 1, 700, 10);
	w.tables.item_death_traits.set(700, class_traits(ItemDeathClass::kEwep));
	Entity gunner_seed;
	gunner_seed.kind = EntityKind::Organic;
	gunner_seed.health = 100;
	const EntityHandle gunner_h = w.registry.spawn(0, gunner_seed);
	Entity *gun = w.registry.get(gun_h);
	Entity *gunner = w.registry.get(gunner_h);
	Seat use_gun;
	use_gun.type = SeatType::Gunner;
	use_gun.occupant = gunner_h;
	gun->seats.push_back(use_gun);
	gunner->mounted = true;
	gunner->mount_target = gun_h;
	gunner->mount_seat = 0;
	gunner->mount_type = SeatType::Gunner;
	gun->primary_occupant = gunner_h;

	gun->health = 0;
	destruction_notify_item_damage(w, *gun, 1);
	CHECK((gun->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) == 0);
	destruction_notify_item_damage(w, *gun, 4);
	CHECK((gun->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
			(kEntityFlagDead | kEntityFlagHusk));
	CHECK(gun->death_tick == 11);
	CHECK(gunner->mounted && gunner->mount_target == gun_h);
	CHECK(gun->primary_occupant == gunner_h);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 0) == 1);
	CHECK(w.explosions.queue.empty());
}

// The null row (an absent, unknown or callback-less ai_function): the item
// takes damage but never dies. [orig: 0x406FF0 — +0x2AC = 0x1000000, ret]
void test_null_class_never_dies() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(2, 8);
	const EntityHandle h = spawn_prop(w, 2, 500, 10);
	w.tables.item_death_traits.set(500, class_traits(ItemDeathClass::kNull));
	Entity *b = w.registry.get(h);
	b->health = 0;
	destruction_notify_item_damage(w, *b, 1);
	destruction_notify_item_damage(w, *b, 2);
	destruction_notify_item_damage(w, *b, 4);
	w.rules.logic_authority = false;
	destruction_notify_item_damage(w, *b, 4);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) == 0);
	CHECK(b->alive);
	CHECK(b->death_tick == 0);
	CHECK(w.out.destruction.husk_swaps.empty());
	CHECK(w.out.destruction.sounds.empty());
	CHECK(w.out.destruction.effects.empty());
	CHECK(w.explosions.queue.empty());
}

// gnl2: the authority kill leg marks the entity dead, plays the death sound
// and one effect, and arms a 32-tick think; the expiry broadcasts the
// detonation — Effect_AirExp plus one kz_M406HE blast one unit above the
// entity, credited to its last attacker with the ammo's own radius — then
// lands the husk. [orig: Entity_HandleDeathEvent @0x4070F0 — the kill leg
//  @0x407279..0x4072dd (+0x2AC = 0x20), the Flags&2 leg @0x4071ef..0x40725f
//  -> Server_BroadcastExplosionEffect @0x508450 -> Entity_SpawnExplosionEffects
//  @0x4399c0 (@0x4399e3 effect, @0x439a06 queue push)]
void test_gnl2_death_detonates_after_pool_countdown(int pool, uint32_t expiry) {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.tables.ammo.entries.resize(5);
	AmmoTableEntry &he = w.tables.ammo.entries[4];
	he.name = "kz_M406HE";
	he.valid = true;
	he.kztype = ammo_kz::kStandard;
	he.kz_damage = 100;
	he.kz_minradius = 1.0f;
	he.kz_maxradius = 6.0f;
	w.registry.configure_pool(0, 4);
	w.registry.configure_pool(pool, 8);
	w.logic_tick = 300;
	Entity attacker_seed;
	attacker_seed.kind = EntityKind::Organic;
	attacker_seed.health = 100;
	const EntityHandle attacker = w.registry.spawn(0, attacker_seed);
	const EntityHandle h = spawn_prop(w, pool, 500, 50);
	w.tables.item_death_traits.set(500, class_traits(ItemDeathClass::kGnl2));
	Entity *b = w.registry.get(h);
	b->last_attacker = attacker;
	b->health = 0;
	destruction_notify_item_damage(w, *b, 1);
	CHECK((b->engine_flags & kEntityFlagDead) != 0);
	CHECK((b->engine_flags & kEntityFlagHusk) == 0);
	CHECK(!b->alive);
	CHECK(b->death_tick == 300);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 0) == 1);
	CHECK(w.out.destruction.husk_swaps.empty());
	CHECK(w.explosions.queue.empty());

	for (uint32_t t = 300; t < expiry; ++t) {
		w.logic_tick = t;
		step_item_pool(w, h.pool());
		CHECK((b->engine_flags & kEntityFlagHusk) == 0);
	}
	w.logic_tick = expiry;
	step_item_pool(w, h.pool());
	CHECK((b->engine_flags & kEntityFlagHusk) != 0);
	CHECK(w.out.destruction.husk_swaps.size() == 1);
	CHECK(w.out.destruction.items_destroyed == 1);
	CHECK(count_effect(w, "Effect_AirExp", 0) == 1);
	CHECK(w.explosions.queue.size() == 1);
	if (w.explosions.queue.size() == 1) {
		const ExplosionEntry &blast = w.explosions.queue[0];
		CHECK(blast.ammo_index == 4);
		CHECK(blast.owner == attacker);
		CHECK(blast.hit_word == 1);
		CHECK(blast.radius_override == 0.0f); // the ammo's kz_maxradius
		CHECK(std::abs(blast.pos.x - 10.0f) < 1.0e-6f);
		CHECK(std::abs(blast.pos.z - 1.0f) < 1.0e-6f);
	}
	// No death sound replay and no second detonation.
	w.logic_tick = 400;
	step_item_pool(w, h.pool());
	destruction_notify_item_damage(w, *b, 2);
	CHECK(count_sound(w, "EXPLO_BARREL") == 1);
	CHECK(count_effect(w, "Effect_AirExp", 0) == 1);
	CHECK(w.explosions.queue.size() == 1);
}

// A row without a class (hand-built, or a def whose callback is unported)
// keeps the tree body: the section-debris death with the kz chain.
void test_unwitnessed_class_keeps_tree_body() {
	auto w_heap = std::make_unique<World>();
	World &w = *w_heap;
	seed_ammo(w);
	w.registry.configure_pool(1, 8);
	const EntityHandle h = spawn_prop(w, 1, 500, 30);
	ItemDeathTraits traits = barrel_traits();
	CHECK(traits.death_class == ItemDeathClass::kUnwitnessed);
	w.tables.item_death_traits.set(500, traits);
	Entity *b = w.registry.get(h);
	b->health = 0;
	destruction_notify_item_damage(w, *b, 1);
	CHECK((b->engine_flags & (kEntityFlagDead | kEntityFlagHusk)) ==
			(kEntityFlagDead | kEntityFlagHusk));
	CHECK(w.explosions.queue.size() == 1);
	CHECK(count_effect(w, "Effect_LrgOrdExp", 1) == 1);
}

// The retail parentSlot codes describe the attachment TYPE, not the dense
// seat-vector index. Exercise the production attach path with reordered seats.
// [orig: Projectile_ProcessExplosionQueue @0x4eb0e2..0x4eb136]
int mounted_blast_health(SeatType seat_type, int seat_index, bool gun_on_vehicle) {
    auto world = std::make_unique<World>();
    World &w = *world;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    w.tables.ammo.entries.resize(2);
    auto &ammo = w.tables.ammo.entries[1];
    ammo.valid = true;
    ammo.kztype = ammo_kz::kStandard;
    ammo.kz_damage = 50;
    ammo.kz_minradius = 1;
    ammo.kz_maxradius = 8;
    ammo.flags = kAmmoFlagNoMItems | kAmmoFlagNoDItems;
    Entity vehicle;
    vehicle.kind = EntityKind::Item;
    vehicle.has_item_def = true;
    vehicle.item_type = 1;
    vehicle.health = 200;
    vehicle.position = {0,0,0};
    if (gun_on_vehicle) vehicle.ground_target = w.registry.spawn(1, vehicle);
    vehicle.seats.resize(6);
    for (int i = 0; i < 6; ++i) {
        vehicle.seats[i].type = seat_type;
        vehicle.seats[i].bone_index = static_cast<uint8_t>(i + 1);
    }
    EntityHandle vh = w.registry.spawn(1, vehicle);
    Entity passenger;
    passenger.kind = EntityKind::Organic;
    passenger.health = 100;
    passenger.health_max = 100;
    passenger.position = {0,0,0};
    EntityHandle ph = w.registry.spawn(0, passenger);
    CHECK(w.vehicles.process_attach(ph, vh, static_cast<uint8_t>(seat_index + 1)));
    CHECK(w.registry.get(ph)->mount_type == seat_type);
    CHECK(w.registry.get(ph)->mount_seat == seat_index);
    ExplosionEntry blast;
    blast.type = ammo_kz::kStandard;
    blast.ammo_index = 1;
    blast.pos = {0,0,0};
    w.explosions.queue_explosion(w, blast);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
    return w.registry.get(ph)->health;
}

void test_mounted_blast_protection_follows_seat_type() {
    for (SeatType type : {SeatType::Passenger, SeatType::Controller, SeatType::Driver})
        for (int index : {0, 1, 3, 5}) CHECK(mounted_blast_health(type, index, false) == 100);
    for (int index : {0, 1, 3, 5}) {
        CHECK(mounted_blast_health(SeatType::Gunner, index, true) == 100);
        CHECK(mounted_blast_health(SeatType::Gunner, index, false) == 50);
    }
}

void test_blast_breaks_flagged_sections_at_transformed_box_centers() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(2, 4);
    Entity seed;
    seed.kind = EntityKind::Building;
    seed.item_type = 5;
    seed.has_item_def = true;
    seed.health = seed.health_max = 1000;
    seed.bound_radius = 20;
    seed.position = {20, 30, 10};
    const auto h = w.registry.spawn(2, seed);
    CollisionModel model;
    model.sections.resize(4);
    for (auto &sec : model.sections) {
        sec.flags = 2;
        sec.authored_bounds = true;
        sec.radius = 65536;
    }
    model.sections[0].flags = 0;
    // Center is exactly on all three blast AABB edges; retail does not apply
    // a sphere-distance rejection after this box test.
    model.sections[1].min_x = model.sections[1].max_x = 8 * 65536;
    model.sections[1].min_y = model.sections[1].max_y = 8 * 65536;
    model.sections[1].min_z = model.sections[1].max_z = 8 * 65536;
    model.sections[2].min_x = model.sections[2].max_x = 9 * 65536;
    model.sections[3].flags = 0;
    CollisionWorld collision;
    collision.assign_entity(h, collision.add_model(std::move(model)));
    collision.build_initial_tables(w);
    w.collision = &collision;
    ExplosionEntry blast;
    blast.type = ammo_kz::kRadiusBlast;
    blast.ammo_index = 1;
    blast.pos = seed.position;
    w.explosions.queue_explosion(w, blast);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(h)->section_mask == 2u);
    // Repeating the blast preserves the section mask and suppresses a second
    // section-break sound, while ordinary health damage can still occur.
    w.explosions.queue_explosion(w, blast);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.out.destruction);
    CHECK(w.registry.get(h)->section_mask == 2u);
}

// Queue one entry and drain it.
void run_blast(World &w, const ExplosionEntry &e) {
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.out.destruction);
}

// A crewed vehicle's hull keeps the blast less its occupant share: 100 at the
// center with two riders at 0.25 each (capped at 0.5) leaves 50.
// [orig: Entity_ApplyWeaponDamage — ItemDef type 1 @0x4E69C7 ->
//  Entity_ApplyOccupantDamageScale @0x4E69D3; Entity_CountMountedEntities
//  @0x435970]
void test_blast_on_a_crewed_vehicle_scales_by_occupants() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    Entity hull;
    hull.kind = EntityKind::Item;
    hull.item_id = 700;
    hull.has_item_def = true;
    hull.item_type = 1;
    hull.health = hull.health_max = 500;
    hull.bound_radius = 1.0f;
    hull.damage_reduc_pp = 0.25f;
    hull.damage_reduc_max = 0.5f;
    const EntityHandle vehicle = w.registry.spawn(1, hull);
    for (int i = 0; i < 2; ++i) {
        Entity rider;
        rider.kind = EntityKind::Organic;
        rider.has_item_def = true;
        rider.item_type = 3;
        rider.health = rider.health_max = 100;
        rider.position = Vec3{100.0f + 5.0f * static_cast<float>(i), 0.0f, 0.0f}; // out of reach
        rider.ground_target = vehicle;
        w.registry.spawn(0, rider);
    }
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    run_blast(w, e);
    CHECK(w.registry.get(vehicle)->health == 500 - 50);
}

// The victim's damage-disabled word (entity+0x124) zeroes a blast: a death
// piece (-1) and a spawn-protected player (the 620-tick countdown) keep their
// health, and the same bodies take the blast once the word clears.
// [orig: Entity_ApplyWeaponDamage `cmp dword ptr [esi+124h], 0`
//  @0x4E69B8..0x4E69C3; the drain's pool-0 tests @0x4EB17E..0x4EB18B and
//  @0x4EB2FA..0x4EB300]
void test_blast_respects_the_damage_disabled_word() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    Entity piece;
    piece.kind = EntityKind::Item;
    piece.item_id = 500;
    piece.health = piece.health_max = 120;
    piece.bound_radius = 1.0f;
    piece.position = Vec3{2.0f, 0.0f, 0.0f};
    piece.damage_state = -1;
    const EntityHandle item = w.registry.spawn(1, piece);
    w.tables.item_death_traits.set(500, barrel_traits());
    Entity person;
    person.kind = EntityKind::Organic;
    person.has_item_def = true;
    person.item_type = 3;
    person.health = person.health_max = 200;
    person.bound_radius = 0.6f;
    person.position = Vec3{-2.0f, 0.0f, 0.0f};
    person.damage_state = 620;
    const EntityHandle body = w.registry.spawn(0, person);
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    run_blast(w, e);
    CHECK(w.registry.get(item)->health == 120);
    CHECK(w.registry.get(body)->health == 200);
    w.registry.get(item)->damage_state = 0;
    w.registry.get(body)->damage_state = 0;
    run_blast(w, e);
    CHECK(w.registry.get(item)->health < 120);
    CHECK(w.registry.get(body)->health < 200);
}

// An armor-blocked blast (0 damage) still runs the non-person leg: the
// flagged sections break and the tail stores the attacker. No damage test
// sits ahead of the person/item split.
// [orig: Entity_ApplyWeaponDamage — the split @0x4E69FD, the section pass
//  @0x4E6C5E..0x4E6E6B, the tail gates @0x4E6EFD / @0x4E6F0A, the store
//  @0x4E6F18]
void test_zero_damage_blast_still_runs_the_item_leg() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(0, 2);
    w.registry.configure_pool(2, 4);
    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.health = 100;
    shooter.position = Vec3{200.0f, 0.0f, 0.0f};
    const EntityHandle owner = w.registry.spawn(0, shooter);
    Entity seed;
    seed.kind = EntityKind::Building;
    seed.item_id = 510;
    seed.item_type = 5;
    seed.has_item_def = true;
    seed.health = seed.health_max = 1000;
    seed.bound_radius = 20;
    seed.position = {20, 30, 10};
    const EntityHandle h = w.registry.spawn(2, seed);
    ItemDeathTraits armored = barrel_traits();
    armored.armor_blast = 60; // > penetration_kz 50: the damage reads 0
    w.tables.item_death_traits.set(510, armored);
    CollisionModel model;
    model.sections.resize(2);
    for (auto &sec : model.sections) {
        sec.flags = 2;
        sec.authored_bounds = true;
        sec.radius = 65536;
    }
    model.sections[0].flags = 0;
    CollisionWorld collision;
    collision.assign_entity(h, collision.add_model(std::move(model)));
    collision.build_initial_tables(w);
    w.collision = &collision;
    ExplosionEntry blast;
    blast.type = ammo_kz::kStandard;
    blast.ammo_index = 1;
    blast.pos = seed.position;
    blast.owner = owner;
    w.explosions.queue_explosion(w, blast);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.out.destruction);
    const Entity *building = w.registry.get(h);
    CHECK(building->health == 1000);
    CHECK(building->section_mask == 2u);
    CHECK(building->last_attacker == owner);
}

// The person leg selects the blast death anim for every victim the callback
// reaches, 0 damage included, so a victim 4..8 u out draws the fire roll
// whether or not its armor stops the damage.
// [orig: Entity_ApplyWeaponDamage — the band test and the PRNG draw
//  @0x4E6A61..0x4E6AA0 ahead of `test edi, edi; jle` @0x4E6AD6]
void test_zero_damage_person_blast_still_draws_the_cause_roll() {
    const auto rng_after = [](bool armored) {
        auto storage = std::make_unique<World>();
        World &w = *storage;
        seed_ammo(w);
        w.registry.configure_pool(0, 2);
        Entity person;
        person.kind = EntityKind::Organic;
        person.has_item_def = true;
        person.item_type = 3;
        person.item_id = 600;
        person.health = person.health_max = 1000;
        person.bound_radius = 0.6f;
        person.position = Vec3{5.6f, 0.0f, 0.0f}; // surface distance 5.0
        const EntityHandle body = w.registry.spawn(0, person);
        if (armored) {
            ItemDeathTraits traits;
            traits.armor_blast = 60; // > penetration_kz 50
            w.tables.item_death_traits.set(600, traits);
        }
        ExplosionEntry e;
        e.pos = Vec3{};
        e.type = ammo_kz::kStandard;
        e.ammo_index = 1;
        run_blast(w, e);
        CHECK((w.registry.get(body)->health == 1000) == armored);
        return w.destruction_rng.state;
    };
    DestructionRng one_draw;
    one_draw.next16();
    CHECK(rng_after(false) == one_draw.state);
    CHECK(rng_after(true) == one_draw.state);
}

// The person quadrant reads the bearing from the victim toward the blast
// point: a blast in front of the victim is quadrant 0.
// [orig: Entity_ApplyWeaponDamage — the entry-minus-target fpatan
//  @0x4E6A07..0x4E6A2E, `add ecx, 1FFFFFFFh; shr ecx, 1Eh` @0x4E6A58..0x4E6A5E]
void test_person_blast_quadrant_faces_the_blast() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(0, 2);
    Entity person;
    person.kind = EntityKind::Organic;
    person.has_item_def = true;
    person.item_type = 3;
    person.health = person.health_max = 1000;
    person.bound_radius = 0.6f;
    person.yaw = 90; // engine heading 0: facing +x
    const EntityHandle body = w.registry.spawn(0, person);
    ExplosionEntry e;
    e.pos = Vec3{1.0f, 0.0f, 0.0f}; // in front, surface 0.4 (no fire roll)
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    run_blast(w, e);
    const int front = compute_death_anim_state(1, 0, death_cause::kExplosive);
    CHECK(front != compute_death_anim_state(1, 2, death_cause::kExplosive));
    CHECK(w.registry.get(body)->health == 900);
    CHECK(w.registry.get(body)->death_anim_state == front);
}

// A kill zone that deals no damage still reaches the organic burn: the
// drain's damage read sits after it, per victim, not ahead of the entry.
// [orig: Projectile_ProcessExplosionQueue — the burn
//  Entity_ApplyCollisionForce @0x4EB1D2 ahead of the damage read
//  Entity_GetNetIdIfAuthority @0x4EB2A0]
void test_zero_damage_kill_zone_still_burns() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.tables.ammo.entries[1].kz_damage = 0;
    w.tables.ammo.entries[1].secondary_anim = 1;
    w.registry.configure_pool(0, 2);
    Entity person;
    person.kind = EntityKind::Organic;
    person.has_item_def = true;
    person.item_type = 3;
    person.health = person.health_max = 100;
    person.bound_radius = 0.6f;
    person.position = Vec3{2.0f, 0.0f, 0.0f};
    const EntityHandle body = w.registry.spawn(0, person);
    w.ai.attach(body);
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    run_blast(w, e);
    CHECK(w.ai.for_handle(body) != nullptr && w.ai.for_handle(body)->inf.burn_state == 1);
    CHECK(w.registry.get(body)->health == 100);
}

// Same-team immunity compares the entry's SOURCE entity, not the kill credit
// the dead-source walk resolves: a dead friendly truck's blast spares its
// team's protected items even when an enemy killed the truck, and the drain
// then stores the resolved credit on the untouched victim.
// [orig: Entity_ApplyWeaponDamage `mov eax, [ebx+20h]` and the team bytes
//  @0x4E686E..0x4E6881, the attrib 0x8000 test @0x4E6886; the drain's
//  +0x178 fallback @0x4EB593..0x4EB5A4]
void test_team_immunity_compares_the_blast_source() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    Entity enemy;
    enemy.kind = EntityKind::Organic;
    enemy.health = 100;
    enemy.team = 2;
    enemy.position = Vec3{200.0f, 0.0f, 0.0f};
    const EntityHandle killer = w.registry.spawn(0, enemy);
    Entity truck;
    truck.kind = EntityKind::Item;
    truck.health = 0;
    truck.engine_flags |= kEntityFlagDead;
    truck.team = 1;
    truck.last_attacker = killer;
    truck.position = Vec3{300.0f, 0.0f, 0.0f};
    const EntityHandle source = w.registry.spawn(1, truck);
    Entity crate;
    crate.kind = EntityKind::Item;
    crate.item_id = 520;
    crate.health = crate.health_max = 120;
    crate.team = 1;
    crate.bound_radius = 1.0f;
    crate.position = Vec3{1.0f, 0.0f, 0.0f};
    const EntityHandle protected_item = w.registry.spawn(1, crate);
    ItemDeathTraits traits = barrel_traits();
    traits.team_protect = true;
    w.tables.item_death_traits.set(520, traits);
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    e.owner = source;
    run_blast(w, e);
    CHECK(w.registry.get(protected_item)->health == 120);
    CHECK(w.registry.get(protected_item)->last_attacker == killer);
}

// A hop of the dead-source walk takes the link as stored: a dead source
// nobody damaged credits no one, not itself.
// [orig: Projectile_ProcessExplosionQueue — the stores @0x4EAEB3 /
//  @0x4EAECE run ahead of the null branch; the applicator's walk
//  @0x4E6F13..0x4E6F59]
void test_dead_unattributed_source_credits_no_one() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    Entity dead;
    dead.kind = EntityKind::Item;
    dead.health = 0;
    dead.engine_flags |= kEntityFlagDead;
    dead.position = Vec3{300.0f, 0.0f, 0.0f};
    const EntityHandle source = w.registry.spawn(1, dead);
    Entity person;
    person.kind = EntityKind::Organic;
    person.has_item_def = true;
    person.item_type = 3;
    person.health = person.health_max = 40;
    person.bound_radius = 0.6f;
    const EntityHandle body = w.registry.spawn(0, person);
    Entity barrel;
    barrel.kind = EntityKind::Item;
    barrel.item_id = 500;
    barrel.health = barrel.health_max = 40;
    barrel.bound_radius = 1.0f;
    barrel.position = Vec3{1.0f, 0.0f, 0.0f};
    const EntityHandle item = w.registry.spawn(1, barrel);
    w.tables.item_death_traits.set(500, barrel_traits());
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    e.owner = source;
    run_blast(w, e);
    int body_deaths = 0;
    int item_deaths = 0;
    for (const RoundDeath &death : w.round_sim.deaths) {
        if (death.victim == body) {
            ++body_deaths;
            CHECK(!death.killer.valid());
        }
        if (death.victim == item) {
            ++item_deaths;
            CHECK(!death.killer.valid());
        }
    }
    CHECK(body_deaths == 1 && item_deaths == 1);
    CHECK(!w.registry.get(body)->last_attacker.valid());
    CHECK(!w.registry.get(item)->last_attacker.valid());
}

// A session peer without the authority applies no blast: its damage read
// returns 0, which stops the drain after the burn and skips the item pools,
// and the applicator returns at its session head.
// [orig: Entity_GetNetIdIfAuthority @0x4E4010, read @0x4EB2A0 / @0x4EB349;
//  Entity_ApplyWeaponDamage @0x4E683A..0x4E684A]
void test_session_peer_without_authority_applies_no_blast() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    seed_ammo(w);
    w.registry.configure_pool(0, 2);
    w.registry.configure_pool(1, 2);
    Entity person;
    person.kind = EntityKind::Organic;
    person.has_item_def = true;
    person.item_type = 3;
    person.health = person.health_max = 200;
    person.bound_radius = 0.6f;
    person.position = Vec3{-1.0f, 0.0f, 0.0f};
    const EntityHandle body = w.registry.spawn(0, person);
    Entity barrel;
    barrel.kind = EntityKind::Item;
    barrel.item_id = 500;
    barrel.health = barrel.health_max = 120;
    barrel.bound_radius = 1.0f;
    barrel.position = Vec3{1.0f, 0.0f, 0.0f};
    const EntityHandle item = w.registry.spawn(1, barrel);
    w.tables.item_death_traits.set(500, barrel_traits());
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.rules.mp_session = true;
    w.rules.logic_authority = false;
    run_blast(w, e);
    CHECK(w.registry.get(body)->health == 200);
    CHECK(w.registry.get(item)->health == 120);
    w.rules.logic_authority = true;
    run_blast(w, e);
    CHECK(w.registry.get(body)->health < 200);
    CHECK(w.registry.get(item)->health < 120);
}

// The plyr callback's waypoint tail ends its blast (event 2) and knife
// (event 3) events too: a team 1/2 player whose AI slot carries a route
// channel marks the node it stands in.
// [orig: Entity_HandleDamageAndTriggerZones @0x407B64..0x407C6B;
//  Entity_ApplyWeaponDamage's notify @0x4E6B72;
//  Entity_ApplyVehicleCollisionDamage's notify @0x4E6752]
void test_player_blast_and_knife_run_the_waypoint_tail() {
    for (const bool knife : {false, true}) {
        auto storage = std::make_unique<World>();
        World &w = *storage;
        seed_ammo(w);
        AmmoTableEntry cut;
        cut.name = "KNIFE";
        cut.valid = true;
        cut.kztype = ammo_kz::kKnife;
        cut.kz_damage = 150;
        cut.kz_maxradius = 1.5f;
        w.tables.ammo.entries.push_back(cut); // index 4
        w.registry.configure_pool(0, 4);
        Entity source;
        source.kind = EntityKind::Organic;
        source.has_item_def = true;
        source.item_type = 3;
        source.health = 100;
        source.team = 2;
        const EntityHandle attacker = w.registry.spawn(0, source);
        Entity player;
        player.kind = EntityKind::Organic;
        player.has_item_def = true;
        player.item_type = 3;
        player.item_id = 600;
        player.health = player.health_max = 1000;
        player.bound_radius = 0.6f;
        player.team = 1;
        player.net_id = 7;
        player.flags = player.engine_flags = kEntityFlagPlayer;
        player.position = knife ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{5.6f, 0.0f, 0.0f};
        const EntityHandle body = w.registry.spawn(0, player);
        AiEntity &ai = *w.ai.at(w.ai.attach(body));
        ai.pos[0] = to_fixed(player.position.x);
        ai.slot.f[37] = 3;
        w.ai.nav.channels.resize(4);
        w.ai.nav.channels[3].count = 1;
        w.ai.nav.channels[3].entries[0] = 0;
        w.ai.nav.nodes.resize(1);
        w.ai.nav.nodes[0].f[0] = 0x10000; // radius 1 u, centered on the body
        w.ai.nav.nodes[0].f[1] = ai.pos[0];
        w.ai.nav.nodes[0].f[2] = ai.pos[1];
        ExplosionEntry e;
        e.pos = Vec3{};
        e.type = knife ? ammo_kz::kKnife : ammo_kz::kStandard;
        e.ammo_index = knife ? 4 : 1;
        e.owner = attacker;
        run_blast(w, e);
        CHECK(w.registry.get(body)->health < 1000);
        CHECK(w.registry.get(body)->spawn_phase == 64);
        CHECK(w.script.relations.single_visited(7, 3, 0));
        CHECK(w.script.relations.group_visited(1, 3, 0));
    }
}

// The kind-1 (knife) kill zone: a live person other than the source inside
// kz_maxradius takes the full kz_damage as a signed-word subtraction with the
// knife cause bit, and the kill event credits the source; items are never
// swept, and a KnifeBonus class reaches one unit further.
// [orig: Projectile_ProcessExplosionQueue — jumptable @0x4EADC6 case 1
//  @0x4EAE0C..0x4EAE34, the item-pool skip @0x4EB337..0x4EB343;
//  Entity_ApplyVehicleCollisionDamage @0x4E6620]
void test_knife_kill_zone() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 2);
    w.tables.ammo.entries.resize(2);
    AmmoTableEntry &knife = w.tables.ammo.entries[1];
    knife.name = "KNIFE";
    knife.valid = true;
    knife.kztype = ammo_kz::kKnife;
    knife.kz_damage = 150;
    knife.kz_maxradius = 1.5f;
    Entity attacker_seed;
    attacker_seed.kind = EntityKind::Organic;
    attacker_seed.has_item_def = true;
    attacker_seed.item_type = 3;
    attacker_seed.health = 100;
    attacker_seed.bound_radius = 0.6f;
    attacker_seed.player_class = 1;
    attacker_seed.team = 1;
    const EntityHandle attacker = w.registry.spawn(0, attacker_seed);
    Entity victim_seed = attacker_seed;
    victim_seed.team = 2;
    victim_seed.position = Vec3{1.0f, 0.0f, 0.0f};
    victim_seed.yaw = 270; // engine heading 0x80000000: facing the attacker
    const EntityHandle victim = w.registry.spawn(0, victim_seed);
    Entity far_seed = victim_seed;
    far_seed.position = Vec3{-2.6f, 0.0f, 0.0f}; // surface 2.0: past 1.5, inside 2.5
    const EntityHandle far_victim = w.registry.spawn(0, far_seed);
    Entity crate;
    crate.kind = EntityKind::Item;
    crate.item_id = 500;
    crate.health = crate.health_max = 120;
    crate.bound_radius = 1.0f;
    crate.position = Vec3{0.5f, 0.0f, 0.0f};
    const EntityHandle item = w.registry.spawn(1, crate);
    w.tables.item_death_traits.set(500, barrel_traits());
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kKnife;
    e.ammo_index = 1;
    e.owner = attacker;
    run_blast(w, e);
    const Entity *v = w.registry.get(victim);
    CHECK(v->health == -50);
    CHECK((v->cause_flags & kDamageFlagCollision) != 0);
    CHECK(v->last_attacker == attacker);
    CHECK(v->death_anim_state ==
            compute_death_anim_state(kCollisionDeathBone, 0, kCollisionDeathCause));
    // The knife edge runs the kill accounting [orig:
    // Entity_ApplyVehicleCollisionDamage @0x4E6620 (the Score_ProcessKillEvent
    // call @0x4E6773)].
    bool credited = false;
    for (const RoundDeath &death : w.round_sim.deaths)
        if (death.victim == victim && death.killer == attacker &&
                death.event_flags == kDamageFlagCollision && death.kill_event)
            credited = true;
    CHECK(credited);
    CHECK(w.registry.get(attacker)->health == 100);
    CHECK(w.registry.get(far_victim)->health == 100);
    CHECK(w.registry.get(item)->health == 120);
    w.tables.class_attribute_flags[0] = MissionTables::kCharAttrKnifeBonus;
    run_blast(w, e);
    CHECK(w.registry.get(far_victim)->health == -50);
    CHECK(w.registry.get(item)->health == 120);
}

// The kind-3 (medic kit) kill zone: per same-team person in radius other than
// the medic, a dead one not already being revived queues a revive and a live
// one below its def hp queues a heal, in sweep order; a full-health teammate
// and an enemy queue nothing, and the heal itself is the host's transaction.
// [orig: Projectile_ProcessExplosionQueue case 3 @0x4EADDD..0x4EADFC;
//  GameEvent_HandleMedicInteraction @0x4E6790 — gates @0x4E679C..0x4E67BD,
//  dead arm @0x4E67C2..0x4E67D8, live arm @0x4E67E3..0x4E6805]
void test_medic_kill_zone_queues_both_arms() {
    auto storage = std::make_unique<World>();
    World &w = *storage;
    w.registry.configure_pool(0, 8);
    w.tables.ammo.entries.resize(2);
    AmmoTableEntry &kit = w.tables.ammo.entries[1];
    kit.name = "MEDKIT";
    kit.valid = true;
    kit.kztype = ammo_kz::kMedic;
    kit.kz_maxradius = 3.0f;
    w.tables.class_attribute_flags[(5u - 1u) & 0xFu] = MissionTables::kCharAttrMedic;
    Entity medic_seed;
    medic_seed.kind = EntityKind::Organic;
    medic_seed.has_item_def = true;
    medic_seed.item_type = 3;
    medic_seed.health = 150;
    medic_seed.health_max = 150;
    medic_seed.bound_radius = 0.6f;
    medic_seed.player_class = 5;
    medic_seed.team = 1;
    const EntityHandle medic = w.registry.spawn(0, medic_seed);
    Entity hurt_seed = medic_seed;
    hurt_seed.player_class = 1;
    hurt_seed.health = 40;
    hurt_seed.position = Vec3{1.0f, 0.0f, 0.0f};
    const EntityHandle hurt = w.registry.spawn(0, hurt_seed);
    Entity whole_seed = hurt_seed;
    whole_seed.health = 150;
    w.registry.spawn(0, whole_seed);
    Entity downed_seed = hurt_seed;
    downed_seed.health = 0;
    downed_seed.flags |= kEntityFlagDead;
    const EntityHandle downed = w.registry.spawn(0, downed_seed);
    Entity enemy_seed = hurt_seed;
    enemy_seed.team = 2;
    w.registry.spawn(0, enemy_seed);
    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kMedic;
    e.ammo_index = 1;
    e.owner = medic;
    run_blast(w, e);
    const std::vector<MedicInteraction> &queued = w.round_sim.medic_interactions;
    CHECK(queued.size() == 2);
    CHECK(queued.size() == 2 && queued[0].victim == hurt && !queued[0].revive &&
          queued[0].healer == medic);
    CHECK(queued.size() == 2 && queued[1].victim == downed && queued[1].revive &&
          queued[1].healer == medic);
    CHECK(w.registry.get(hurt)->health == 40);
}

int main() {
    test_blast_on_a_crewed_vehicle_scales_by_occupants();
    test_blast_respects_the_damage_disabled_word();
    test_zero_damage_blast_still_runs_the_item_leg();
    test_zero_damage_person_blast_still_draws_the_cause_roll();
    test_person_blast_quadrant_faces_the_blast();
    test_zero_damage_kill_zone_still_burns();
    test_team_immunity_compares_the_blast_source();
    test_dead_unattributed_source_credits_no_one();
    test_session_peer_without_authority_applies_no_blast();
    test_knife_kill_zone();
    test_player_blast_and_knife_run_the_waypoint_tail();
    test_blast_breaks_flagged_sections_at_transformed_box_centers();
    test_mounted_blast_protection_follows_seat_type();
	test_gnrl_death_is_husk_sound_and_one_effect();
	test_gnrl_client_kill_leg();
	test_gnrc_death_lands_husk_on_pool2_cohort();
	test_gnrc_client_kill_runs_death_transforms_at_once();
	test_ewep_death_dismounts_gunner_before_husk();
	test_ewep_client_kill_keeps_gunner_mounted();
	test_null_class_never_dies();
	test_gnl2_death_detonates_after_pool_countdown(1, 332);
	test_gnl2_death_detonates_after_pool_countdown(2, 336);
	test_unwitnessed_class_keeps_tree_body();
	test_death_effect_banks_and_water_crossings();
	test_aircraft_landing_and_navigation_states();
	test_vehicle_spawn_marker_selection();
	test_spawn_markers_ride_the_script_admission();
	test_aircraft_death_lifecycle();
	test_aircraft_death_spin_rates_roll_in_retail_order();
	test_vehicle_death_kills_authored_children();
	test_vehicle_respawn_lifecycle();
	test_generic_wreck_zero_velocity_and_inverted_rest();
	test_wreck_fire_crackle_rolls_on_the_engine_prng();
	test_death_piece_trail_effect_rows();
    test_explosion_damage_gates();
    test_multiplayer_destroy_buildings_rule();
    test_explosion_los_excludes_victim_hull();
    test_radius_blast_skips_pool1_los();
    test_organic_blast_los_is_unlifted();
    test_explosion_cone_wrap();
    test_explosion_resolves_attacker_chain_for_events();
    test_destructible_death_chain();
    test_section_debris_samples_collision_faces();
    test_glass_userpoints_shatter_once_at_authored_radius();
    test_synthetic_husk_events_preserve_distinct_handles();
    test_kz_point_full_euler();
    test_bridge_dead_points_emit_water_shocks();
    test_death_pieces();
    test_death_piece_ring_generation();
    test_death_piece_launch_and_spin();
    test_death_piece_rng_is_world_local();
    test_death_piece_section_center();
    test_death_piece_water();
    test_death_piece_ground_scorch_routing();
    test_bullet_gates();
    test_dead_item_settle();
    test_generic_staticdeath_freezes();
    test_specialized_piece_physics_callback();
    test_dead_item_landing_split();
    test_static_dead_item_settle();
    test_dead_item_water_splash();
    test_death_dispatch_flag_routing();
    test_death_kick_field();
    test_round_destroys_item();
    test_building_death_requires_loaded_husk_model();
    test_net_kill_runs_client_side_death_chain();
    test_medic_kill_zone_queues_both_arms();
    if (failures == 0) std::printf("destruction_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
