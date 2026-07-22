// Item destruction tests (world/destruction.h; docs/world/world-wac-ai-re.md
// §24): the explosion queue + AoE gates, the bullet armor gates, the
// destructible death chain (husk flags + events + the kz chain blast), the
// death-piece pool, and the dead-item settle. Driven by a manual World.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "terrain/height_field.h"
#include "world/angle.h"
#include "world/collision.h"
#include "world/destruction.h"
#include "world/world.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// A minimal ammo table: [0] null, [1] a grenade-style kz round, [2] the
// kz_OrganicBlast death-chain entry the InitDeathSounds leg resolves by name.
void seed_ammo(World &w) {
    w.ammo.entries.resize(3);
    AmmoTableEntry &null_e = w.ammo.entries[0];
    null_e.name = "AT_NULL";
    null_e.valid = true;
    AmmoTableEntry &kz = w.ammo.entries[1];
    kz.name = "40mm_HE";
    kz.valid = true;
    kz.kztype = ammo_kz::kStandard;
    kz.kz_damage = 100;
    kz.kz_minradius = 1.0f;
    kz.kz_maxradius = 8.0f;
    kz.penetration_kz = 50;
    AmmoTableEntry &organic = w.ammo.entries[2];
    organic.name = "kz_OrganicBlast";
    organic.valid = true;
    organic.kztype = ammo_kz::kStandard;
    organic.kz_damage = 40;
    organic.kz_minradius = 0.5f;
    organic.kz_maxradius = 5.0f;
    // The death blast spares M/D items (the chain-explosion control the ammo
    // authors via NoMItems/NoDItems) — organics only.
    organic.flags = kAmmoFlagNoMItems | kAmmoFlagNoDItems;
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
    w.item_death_traits.set(500, barrel_traits());

    // In blast range, armor passed (penetration_kz 50 >= armor_blast 10):
    // full kz_damage at the center band, linear falloff outside kz_minradius.
    ExplosionEntry e;
    e.pos = Vec3{10.0f, 0.0f, 0.0f}; // dead center — surface distance 0
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.destruction);
    Entity *b = w.registry.get(barrel);
    CHECK(b != nullptr);
    CHECK(b->health == 20); // 120 - 100 (no falloff inside kz_minradius)
    CHECK(w.destruction.explosions_processed == 1);

    // Blast armor gate: an ammo whose penetration_kz is below the armor word
    // zeroes its damage [orig: @0x4e69b0].
    b->health = 120;
    ItemDeathTraits armored = barrel_traits();
    armored.armor_blast = 60; // > penetration_kz 50
    w.item_death_traits.set(500, armored);
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.destruction);
    CHECK(w.registry.get(barrel)->health == 120);

    // Invulnerable word (-1 = 0xFFFF) [orig: @0x4e68aa].
    armored.armor_blast = -1;
    w.item_death_traits.set(500, armored);
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.destruction);
    CHECK(w.registry.get(barrel)->health == 120);

    // The NoMItems ammo flag skips the whole pool-1 sweep [orig: & 0x100000
    // @0x4eb378].
    w.item_death_traits.set(500, barrel_traits());
    w.ammo.entries[1].flags = kAmmoFlagNoMItems;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.destruction);
    CHECK(w.registry.get(barrel)->health == 120);
    w.ammo.entries[1].flags = 0;
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
    seed.health = 120;
    seed.health_max = 120;
    seed.position = Vec3{4.0f, 0.0f, 0.0f};
    seed.bound_radius = 1.0f;
    const EntityHandle victim = w.registry.spawn(1, seed);
    w.item_death_traits.set(500, barrel_traits());

    CollisionWorld collision;
    const int32_t model_id = collision.add_model(solid_box_model(1.0, 2.0));
    collision.assign_entity(victim, model_id);

    ExplosionEntry e;
    e.pos = Vec3{0.0f, 0.0f, 0.0f};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.destruction);
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
    victim_seed.health = 120;
    victim_seed.health_max = 120;
    victim_seed.position = Vec3{4.0f, 0.0f, 0.5f};
    victim_seed.bound_radius = 1.0f;
    const EntityHandle victim = w.registry.spawn(1, victim_seed);
    w.item_death_traits.set(500, barrel_traits());

    Entity wall_seed;
    wall_seed.kind = EntityKind::Building;
    wall_seed.position = Vec3{2.0f, 0.0f, 0.0f};
    wall_seed.bound_radius = 1.0f;
    const EntityHandle wall = w.registry.spawn(2, wall_seed);
    CollisionWorld collision;
    collision.assign_entity(wall, collision.add_model(solid_box_model(0.25, 2.0)));

    ExplosionEntry e;
    e.pos = Vec3{0.0f, 0.0f, 0.5f};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.destruction);
    CHECK(w.registry.get(victim)->health == 120);

    e.type = ammo_kz::kRadiusBlast;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.destruction);
    CHECK(w.registry.get(victim)->health < 120);
}

// Only the movable-item LOS walk lifts both endpoints by 0.25 units. Pool 0
// uses the authored positions, so low cover that sits below the lifted ray
// still shields an organic target.
void test_organic_blast_los_is_unlifted() {
    auto w_heap = std::make_unique<World>();
    World &w = *w_heap;
    seed_ammo(w);
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(2, 4);

    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.health = 120;
    victim_seed.health_max = 120;
    victim_seed.position = Vec3{4.0f, 0.0f, 0.05f};
    victim_seed.bound_radius = 0.6f;
    const EntityHandle victim = w.registry.spawn(0, victim_seed);

    Entity wall_seed;
    wall_seed.kind = EntityKind::Building;
    wall_seed.position = Vec3{2.0f, 0.0f, 0.0f};
    wall_seed.bound_radius = 0.3f;
    const EntityHandle wall = w.registry.spawn(2, wall_seed);
    CollisionWorld collision;
    collision.assign_entity(wall, collision.add_model(solid_box_model(0.25, 0.15)));

    ExplosionEntry e;
    e.pos = Vec3{0.0f, 0.0f, 0.05f};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, &collision, nullptr, -1.0e9f, w.destruction);
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
        w.ammo.entries[1].kz_pieslice_bam =
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
        w.item_death_traits.set(500, barrel_traits());

        ExplosionEntry e;
        e.pos = Vec3{};
        e.dir_bam = bam_from_degrees_wrapped(direction_deg);
        e.type = ammo_kz::kStandard;
        e.ammo_index = 1;
        w.explosions.queue_explosion(w, e);
        w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.destruction);
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
    w.item_death_traits.set(500, barrel_traits());

    ExplosionEntry e;
    e.pos = Vec3{};
    e.type = ammo_kz::kStandard;
    e.ammo_index = 1;
    e.owner = dead_owner;
    w.explosions.queue_explosion(w, e);
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.destruction);

    bool organic_hit_resolved = false;
    bool organic_death_resolved = false;
    bool item_death_resolved = false;
    for (const RoundHit &hit : w.round_sim.hits)
        if (hit.victim == organic && hit.shooter == live_attacker)
            organic_hit_resolved = true;
    for (const RoundDeath &death : w.round_sim.deaths) {
        if (death.victim == organic && death.killer == live_attacker)
            organic_death_resolved = true;
        if (death.victim == item && death.killer == live_attacker)
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
    w.item_death_traits.set(500, barrel_traits());

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
    CHECK(w.destruction.items_destroyed == 1);
    CHECK(w.destruction.husk_swaps.size() == 1);
    CHECK(w.destruction.debris_bursts.size() == 1);
    bool found_death_sound = false;
    for (const DestructionSoundEvent &s : w.destruction.sounds)
        if (s.sound == "EXPLO_BARREL") found_death_sound = true;
    CHECK(found_death_sound);
    bool found_death_fx = false;
    bool found_fire_fx = false;
    for (const DestructionEffectEvent &fx : w.destruction.effects) {
        if (fx.effect == "Effect_LrgOrdExp" && fx.family == 1) found_death_fx = true;
        if (fx.effect == "Effect_Fire" && fx.family == 2) found_fire_fx = true;
    }
    CHECK(found_death_fx);
    CHECK(found_fire_fx);
    // A second notify never re-husks [orig: the Flags&4 resend leg @0x440272].
    destruction_notify_item_damage(w, *b, 2);
    CHECK(w.destruction.items_destroyed == 1);

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
    w.explosions.process(w, nullptr, nullptr, -1.0e9f, w.destruction);
    const Entity *by = w.registry.get(bystander);
    CHECK(by->health < 100); // organics in range take the death blast
    CHECK(w.registry.get(item2)->health == 100); // M-items are spared by the flag
    CHECK(!w.round_sim.hits.empty()); // the AI reaction stamp fed
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
    w.item_death_traits.set(500, traits);

    process_destructible_death(w, *w.registry.get(first));
    process_destructible_death(w, *w.registry.get(second));

    CHECK(w.destruction.husk_swaps.size() == 2);
    CHECK(w.destruction.husk_swaps[0].wire_handle == first.packed);
    CHECK(w.destruction.husk_swaps[1].wire_handle == second.packed);
    CHECK(w.destruction.husk_swaps[0].wire_handle !=
          w.destruction.husk_swaps[1].wire_handle);

    // The three attached banks must carry the same incarnation identity as
    // the husk event. Without it, both runtime-only children collapse onto
    // (net=0,bms=0,origin=sentinel) in the Godot presenter.
    CHECK(w.destruction.effects.size() == 6);
    for (size_t i = 0; i < w.destruction.effects.size(); ++i) {
        const DestructionEffectEvent &fx = w.destruction.effects[i];
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
    w.item_death_traits.set(500, traits);
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
    w.item_death_traits.set(700, t);

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
        w.death_pieces.tick(w, nullptr, -1.0e9f, w.destruction);
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
    w.item_death_traits.set(700, barrel_traits());
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
        w.death_pieces.tick(w, nullptr, -1.0e9f, w.destruction);
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
        world->item_death_traits.set(700, barrel_traits());
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
    w.item_death_traits.set(700, t);
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
    w.death_pieces.tick(w, nullptr, 0.0f, w.destruction);
    bool splashed = false;
    for (const DestructionSoundEvent &s : w.destruction.sounds)
        if (s.sound == "IMP_DEBSML_WATER" || s.sound == "IMP_DEBMED_WATER")
            splashed = true;
    CHECK(splashed);
    // Tick 2: below water -> horizontal halves, vertical pins at -4096.
    const float vx = p.vel.x;
    w.death_pieces.tick(w, nullptr, 0.0f, w.destruction);
    CHECK(std::abs(p.vel.x - vx * 0.5f) < 1.0e-5f);
    CHECK(std::abs(p.vel.z + 4096.0f / 65536.0f) < 1.0e-6f);
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
    w.item_death_traits.set(500, t);
    CHECK(item_bullet_damage_gate(w, *e, 40, 10) == 40); // pen 10 >= armor 5
    CHECK(item_bullet_damage_gate(w, *e, 40, 3) == 0);   // pen 3 < armor 5 [orig: @0x4e802a]
    t.armor_impact = -1; // the 0xFFFF invulnerable word [orig: @0x4e8019]
    w.item_death_traits.set(500, t);
    CHECK(item_bullet_damage_gate(w, *e, 40, 100) == 0);
    t.armor_impact = 0;
    t.no_die = true; // clamps to health-1 [orig: @0x4e8074]
    w.item_death_traits.set(500, t);
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
    w.item_death_traits.set(700, t);
    Entity *e = w.registry.get(h);
    e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    e->alive = false;
    e->death_motion = DeathMotionMode::Falling;
    e->veh.slide_z = -65536; // falling 1 u/tick
    // No terrain -> ground at -1e9: it keeps falling, gravity accumulating
    // [orig: slideDecay -= 334 @0x493fe5].
    const int32_t sz0 = e->veh.slide_z;
    destruction_tick_dead_items(w, nullptr, -1.0e9f, w.destruction);
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
    w.item_death_traits.set(705, traits);
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
    destruction_tick_dead_items(w, nullptr, -1.0e9f, w.destruction);
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
        w.item_death_traits.set(id, traits);
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
    Entity *item = drop(700, t);
    const Vec3 item_before = item->position;
    destruction_tick_dead_items(w, &flat.field, -1.0e9f, w.destruction);
    CHECK(std::abs(item->position.x - item_before.x) < 1.0e-6f);
    CHECK(std::abs(item->position.y - item_before.y) < 1.0e-6f);
    CHECK(std::abs(item->position.z - item_before.z) < 1.0e-6f);
    CHECK(item->veh.slide_z == 0);
    CHECK(item->veh.vel_x == 3277);
    bool clunked = false;
    for (const DestructionSoundEvent &s : w.destruction.sounds)
        if (s.sound == "IMP_VCL_DROP") clunked = true;
    CHECK(!clunked);
    CHECK(w.explosions.queue.empty()); // no landing kz for the generic leg
    item->engine_flags &= ~kEntityFlagHusk; // retire from the pass
    // A unitType-routed row (1): the clunk + the landing kz.
    ItemDeathTraits tv = barrel_traits();
    tv.unit_type = 1;
    Entity *wreck = drop(701, tv);
    destruction_tick_dead_items(w, &flat.field, -1.0e9f, w.destruction);
    const float routed_new_z = ground + 0.5f +
            static_cast<float>(-65536 - 334) / 65536.0f;
    CHECK(std::abs(wreck->position.z - routed_new_z) < 1.0e-3f);
    CHECK(wreck->veh.slide_z == -65536 - 334);
    CHECK(wreck->veh.vel_x == 3277);
    CHECK(wreck->death_motion == DeathMotionMode::Generic);
    for (const DestructionSoundEvent &s : w.destruction.sounds) {
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
        w.item_death_traits.set(item_id, traits);
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
        destruction_tick_dead_items(w, nullptr, -1.0e9f, w.destruction);
        CHECK(std::abs(sliding->position.x - (x0 + 1.0f)) < 1.0e-6f);
        CHECK(sliding->veh.vel_x == 63569);
        CHECK(sliding->veh.slide_z == 0);
        sliding->death_motion = DeathMotionMode::None;

        Entity *stopping = spawn_static(
                unit_type, Vec3{20.0f + static_cast<float>(unit_type), 8.0f, 10.0f});
        stopping->veh.vel_x = 8;
        stopping->veh.vel_y = -8;
        stopping->veh.slide_z = 0;
        destruction_tick_dead_items(w, nullptr, -1.0e9f, w.destruction);
        CHECK(stopping->veh.vel_x == 0);
        CHECK(stopping->veh.vel_y == 0);
        CHECK(stopping->veh.slide_z == -167);
        stopping->death_motion = DeathMotionMode::None;

        Entity *landing = spawn_static(
                unit_type, Vec3{8.0f, 8.0f, ground + 0.25f});
        landing->veh.vel_x = 0;
        landing->veh.vel_y = 0;
        landing->veh.slide_z = -65536;
        destruction_tick_dead_items(w, &flat.field, -1.0e9f, w.destruction);
        CHECK(std::abs(landing->position.z - ground) < 1.0e-3f);
        CHECK(landing->veh.vel_x == 0);
        CHECK(landing->veh.vel_y == 0);
        CHECK(landing->veh.slide_z == -65536 - 167);
        CHECK(landing->death_motion == DeathMotionMode::Generic);
        destruction_tick_dead_items(w, &flat.field, -1.0e9f, w.destruction);
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
    destruction_tick_dead_items(w, nullptr, -1.0e9f, w.destruction);
    CHECK(ai_static->veh.slide_z == -167);

    Entity *below_sliding = spawn_static(5, Vec3{50.0f, 8.0f, ground - 1.0f});
    below_sliding->veh.vel_x = 65536;
    below_sliding->veh.slide_z = 0;
    destruction_tick_dead_items(w, &flat.field, -1.0e9f, w.destruction);
    CHECK(below_sliding->death_motion == DeathMotionMode::Static);
    CHECK(std::abs(below_sliding->position.z - (ground - 1.0f)) < 1.0e-6f);

    // StaticDeath's water/ground split is asymmetric. Water below terrain
    // transitions immediately, exactly 10 units still uses the ground line,
    // and only values above 10 move the landing line down by 25 units.
    Entity *water_below = spawn_static(5, Vec3{60.0f, 8.0f, ground + 5.0f});
    water_below->veh.vel_x = 65536;
    destruction_tick_dead_items(
            w, &flat.field, ground - 1.0f, w.destruction);
    CHECK(water_below->death_motion == DeathMotionMode::Generic);
    CHECK(std::abs(water_below->position.z - ground) < 1.0e-3f);

    Entity *at_ten = spawn_static(5, Vec3{64.0f, 8.0f, ground - 20.0f});
    destruction_tick_dead_items(
            w, &flat.field, ground + 10.0f, w.destruction);
    CHECK(at_ten->death_motion == DeathMotionMode::Generic);
    CHECK(std::abs(at_ten->position.z - ground) < 1.0e-3f);

    Entity *deep_water_hold = spawn_static(
            5, Vec3{68.0f, 8.0f, ground - 20.0f});
    destruction_tick_dead_items(
            w, &flat.field, ground + 11.0f, w.destruction);
    CHECK(deep_water_hold->death_motion == DeathMotionMode::Static);
    CHECK(std::abs(deep_water_hold->position.z - (ground - 20.0f)) < 1.0e-3f);

    Entity *deep_water_land = spawn_static(
            5, Vec3{72.0f, 8.0f, ground - 26.0f});
    destruction_tick_dead_items(
            w, &flat.field, ground + 11.0f, w.destruction);
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
    w.item_death_traits.set(700, t);
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
    destruction_tick_dead_items(w, &flat.field, water, w.destruction);
    bool splashed = false;
    for (const DestructionSoundEvent &s : w.destruction.sounds)
        if (s.sound == "IMP_DEBLRG_WATER") splashed = true;
    CHECK(!splashed);
    CHECK(e->position.z > ground); // still sinking, not landed
    e->engine_flags &= ~kEntityFlagHusk;

    ItemDeathTraits routed = barrel_traits();
    routed.unit_type = 1;
    w.item_death_traits.set(701, routed);
    seed.item_id = 701;
    const EntityHandle routed_h = w.registry.spawn(1, seed);
    Entity *routed_e = w.registry.get(routed_h);
    routed_e->engine_flags |= (kEntityFlagDead | kEntityFlagHusk);
    routed_e->alive = false;
    routed_e->death_motion = DeathMotionMode::Falling;
    routed_e->veh.slide_z = -8 * 65536;
    routed_e->veh.vel_x = 65536;
    routed_e->veh.vel_y = -32768;
    destruction_tick_dead_items(w, &flat.field, water, w.destruction);
    splashed = false;
    for (const DestructionSoundEvent &s : w.destruction.sounds) {
        if (s.sound != "IMP_DEBLRG_WATER") continue;
        splashed = true;
        CHECK(std::abs(s.pos.x - 9.0f) < 1.0e-6f);
        CHECK(std::abs(s.pos.y - 7.5f) < 1.0e-6f);
        CHECK(std::abs(s.pos.z - water) < 1.0e-6f);
    }
    CHECK(splashed);
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
        w.item_death_traits.set(item_id, traits);
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
    destruction_tick_dead_items(w, nullptr, -1.0e9f, w.destruction);
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
        w.item_death_traits.set(id, traits);
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
    w.ammo.entries.push_back(rifle);
    const int rifle_idx = static_cast<int>(w.ammo.entries.size()) - 1;
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
    barrel_seed.bound_radius = 1.0f;
    const EntityHandle barrel = w.registry.spawn(1, barrel_seed);
    w.item_death_traits.set(500, barrel_traits());

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
        w.run_logic_tick(true, false);
    Entity *b = w.registry.get(barrel);
    CHECK(b->health <= 0);
    CHECK((b->engine_flags & kEntityFlagHusk) != 0);
    CHECK(!w.round_sim.deaths.empty());
    bool husk_evented = false;
    for (const HuskSwapEvent &h : w.destruction.husk_swaps)
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
    w.item_death_traits.set(990, traits);

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
    for (const DestructionSoundEvent &sound : w.destruction.sounds)
        if (sound.sound == "EXPLO_SHIP_TINY") collapse_sound = true;
    CHECK(!collapse_sound);
}

int main() {
    test_explosion_damage_gates();
    test_explosion_los_excludes_victim_hull();
    test_radius_blast_skips_pool1_los();
    test_organic_blast_los_is_unlifted();
    test_explosion_cone_wrap();
    test_explosion_resolves_attacker_chain_for_events();
    test_destructible_death_chain();
    test_synthetic_husk_events_preserve_distinct_handles();
    test_kz_point_full_euler();
    test_death_pieces();
    test_death_piece_ring_generation();
    test_death_piece_launch_and_spin();
    test_death_piece_rng_is_world_local();
    test_death_piece_section_center();
    test_death_piece_water();
    test_bullet_gates();
    test_dead_item_settle();
    test_generic_staticdeath_freezes();
    test_dead_item_landing_split();
    test_static_dead_item_settle();
    test_dead_item_water_splash();
    test_death_dispatch_flag_routing();
    test_death_kick_field();
    test_round_destroys_item();
    test_building_death_requires_loaded_husk_model();
    if (failures == 0) std::printf("destruction_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
