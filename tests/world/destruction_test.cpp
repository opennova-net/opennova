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
    t.husk_sub_part_count = 4;
    t.husk_sub_part_types[1] = 3; // CHUNK_M
    t.husk_sub_part_types[2] = 3;
    t.husk_sub_part_types[3] = 2; // CHUNK_S
    t.sound_death = "EXPLO_BARREL";
    t.particledeath = "Effect_LrgOrdExp";
    t.particlefire = "Effect_Fire";
    return t;
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

// The piece spawns at its section's center, rotated through the entity yaw
// [orig: @0x4938bf-0x493900]. Mission yaw 90 bakes the identity rotation.
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
    seed.yaw = 90;
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
        CHECK(std::abs(p->pos.x - 5.5f) < 1.0e-4f);
        CHECK(std::abs(p->pos.y - 5.5f) < 1.0e-4f);
        CHECK(std::abs(p->pos.z - 10.75f) < 1.0e-4f);
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
    e->veh.slide_z = -65536; // falling 1 u/tick
    // No terrain -> ground at -1e9: it keeps falling, gravity accumulating
    // [orig: slideDecay -= 334 @0x493fe5].
    const int32_t sz0 = e->veh.slide_z;
    destruction_tick_dead_items(w, nullptr, -1.0e9f, w.destruction);
    CHECK(e->veh.slide_z == sz0 - 334);
    CHECK(e->position.z < 5.0f);
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
        e->veh.slide_z = -65536; // 1 u/tick down -> lands this tick
        e->veh.vel_x = 3277;     // sliding sideways
        return e;
    };
    // Generic item (unit_type 0): silent landing, motion zeroed, rest offset.
    ItemDeathTraits t = barrel_traits();
    t.husk_rest_min_z = -0.25f;
    Entity *item = drop(700, t);
    destruction_tick_dead_items(w, &flat.field, -1.0e9f, w.destruction);
    CHECK(std::abs(item->position.z - (ground - 0.25f)) < 1.0e-3f);
    CHECK(item->veh.slide_z == 0);
    CHECK(item->veh.vel_x == 0);
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
    (void)wreck;
    destruction_tick_dead_items(w, &flat.field, -1.0e9f, w.destruction);
    for (const DestructionSoundEvent &s : w.destruction.sounds)
        if (s.sound == "IMP_VCL_DROP") clunked = true;
    CHECK(clunked);
    CHECK(!w.explosions.queue.empty());
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
    e->veh.slide_z = -8 * 65536; // 8 u/tick: the bound top crosses in one tick
    destruction_tick_dead_items(w, &flat.field, water, w.destruction);
    bool splashed = false;
    for (const DestructionSoundEvent &s : w.destruction.sounds)
        if (s.sound == "IMP_DEBLRG_WATER") splashed = true;
    CHECK(splashed);
    CHECK(e->position.z > ground); // still sinking, not landed
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

int main() {
    test_explosion_damage_gates();
    test_destructible_death_chain();
    test_death_pieces();
    test_death_piece_launch_and_spin();
    test_death_piece_section_center();
    test_death_piece_water();
    test_bullet_gates();
    test_dead_item_settle();
    test_dead_item_landing_split();
    test_dead_item_water_splash();
    test_death_kick_field();
    test_round_destroys_item();
    if (failures == 0) std::printf("destruction_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
