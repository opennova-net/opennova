// Observable projectile consequence tests through RoundSim's public seam:
// arming/dud substitution, NoDie, and geometric dead/indestructible blockers.
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

#include <base/crt/crt_rng.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/infantry.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/system.h>
#include <runtime/world/world.h>

#include "death_clip_source.h"

using namespace opennova::world;
using namespace opennova::crt;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) {                                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

// World is large and has stable identity. Keeping it at its final heap address
// also prevents a function with several lifetime-disjoint rigs from exhausting
// MSVC's Debug stack frame.
struct HeapWorldFixture {
    std::unique_ptr<World> world_owner = std::make_unique<World>();
    World &world = *world_owner;
};

struct Rig : HeapWorldFixture {
    EntityHandle shooter;
    EntityHandle target;

    Rig() {
        world.registry.configure_pool(0, 8);
        Entity s;
        s.kind = EntityKind::Organic;
        s.item_type = 3;
        s.position = {0.0f, 0.0f, 0.0f};
        s.health = 100;
        shooter = world.registry.spawn(0, s);

        Entity t;
        t.kind = EntityKind::Organic;
        t.has_item_def = true;
        t.item_type = 3;
        t.position = {5.0f, 0.0f, 0.0f};
        t.health = 100;
        target = world.registry.spawn(0, t);

        world.tables.ammo.entries.resize(2);
        AmmoTableEntry &armed = world.tables.ammo.entries[0];
        armed.name = "ARMED";
        armed.valid = true;
        armed.velocity = 620; // 10 units/tick
        armed.max_age_ticks = 20;
        armed.arm_age_ticks = 2;
        armed.weight_in_grains = 875;
        armed.max_damage = 25;
        armed.notarmmed_ammo = "DUD";

        AmmoTableEntry &dud = world.tables.ammo.entries[1];
        dud.name = "DUD";
        dud.valid = true;
        dud.velocity = 1;
        dud.max_age_ticks = 7;
    }

    int fire(RoundConsequenceMode mode = RoundConsequenceMode::Authoritative) {
        RoundSpawnParams p;
        p.owner = shooter;
        p.shooter_handle = shooter.packed;
        p.origin = {0.0f, 0.0f, 0.9f};
        p.dir_yaw_bam = 0;
        p.dir_pitch_bam = 0;
        p.ammo_index = 0;
        return world.round_sim.spawn(world, p, mode);
    }

    void clear_events() {
        world.round_sim.impacts.clear();
        world.round_sim.hits.clear();
        world.round_sim.deaths.clear();
    }
};

// Deterministic posed-person fixture. Only `active_section` is placed on the
// projectile ray; every other section is translated far off-axis. This lets the
// public RoundSim seam exercise the exact retail section/zone code without
// reaching into the private damage helper.
struct PosedDamageRig : HeapWorldFixture {
    CollisionWorld collision;
    EntityHandle shooter;
    EntityHandle target;

    // `active_section` is the FIRST sphere the descending walk crosses (the
    // highest on-ray ordinal -> ray[31]); an optional lower `second_section`
    // also sits on the ray so the final overlap rewrites ray[32] below it.
    explicit PosedDamageRig(int active_section, int second_section = -1) {
        world.registry.configure_pool(0, 16);

        Entity s;
        s.kind = EntityKind::Organic;
        s.item_type = 3;
        s.health = 100;
        shooter = world.registry.spawn(0, s);

        Entity t;
        t.kind = EntityKind::Organic;
        t.has_item_def = true;
        t.item_type = 3;
        t.position = {5.0f, 0.0f, 0.0f};
        t.health = 5000;
        target = world.registry.spawn(0, t);

        AmmoTableEntry ammo;
        ammo.name = "ZONE";
        ammo.valid = true;
        ammo.velocity = 620;
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        world.tables.ammo.entries.push_back(ammo);

        const int section_count = active_section + 1;
        CollisionModel model;
        model.sections.resize(static_cast<size_t>(section_count));
        for (CollisionSection &section : model.sections) {
            section.authored_bounds = true;
            section.min_x = section.min_y = section.min_z = -0x10000;
            section.max_x = section.max_y = section.max_z = 0x10000;
            section.radius = 0x10000;
        }

        const int32_t model_id = collision.add_model(std::move(model));
        collision.assign_entity(target, model_id);
        std::vector<CollisionMatrix> matrices;
        matrices.reserve(static_cast<size_t>(section_count));
        for (int section = 0; section < section_count; ++section) {
            const bool on_ray =
                section == active_section || section == second_section;
            const int32_t center[3] = {
                5 * 65536,
                on_ray ? 0 : (20 + section) * 65536,
                58982, // retail unposed torso height, 0.9u in Q16
            };
            matrices.push_back(collision_matrix_from_heading(0, center));
        }
        CHECK(collision.publish_entity_section_matrices(target, std::move(matrices)));
        collision.build_tick_tables(world);
        world.collision = &collision;
    }

    Entity *shooter_entity() { return world.registry.get(shooter); }
    Entity *target_entity() { return world.registry.get(target); }
    AmmoTableEntry &ammo() { return world.tables.ammo.entries[0]; }

    void fire() {
        RoundSpawnParams params;
        params.owner = shooter;
        params.shooter_handle = shooter.packed;
        params.origin = {0.0f, 0.0f, 0.9f};
        params.ammo_index = 0;
        CHECK(world.round_sim.spawn(world, params) >= 0);
    }

    void fire_and_tick() {
        fire();
        world.round_sim.tick(world, nullptr);
    }
};

struct FlightResult {
    bool active = false;
    int32_t age_ticks = 0;
    FixedVec3 pos;
    FixedVec3 vel;
};

FlightResult fly_one_tick(FixedVec3 velocity, uint32_t flags = 0,
                          int32_t drag_fp16 = 0, int32_t min_stable_velocity = 0,
                          int32_t origin_z = 10 * 65536, int32_t water_z = 0) {
    World world;
    world.env.water_z = water_z;
    AmmoTableEntry ammo;
    ammo.name = "FLIGHT";
    ammo.valid = true;
    ammo.velocity = 62;
    ammo.max_age_ticks = 100;
    ammo.flags = flags;
    ammo.drag_fp16 = drag_fp16;
    ammo.drag = static_cast<float>(from_fixed(drag_fp16));
    ammo.min_stable_velocity = min_stable_velocity;
    world.tables.ammo.entries.push_back(ammo);

    RoundSpawnParams params;
    params.origin = {0.0f, 0.0f, static_cast<float>(from_fixed(origin_z))};
    params.ammo_index = 0;
    const int slot = world.round_sim.spawn(world, params);
    CHECK(slot >= 0);
    if (slot < 0) return {};
    LiveRound &round = world.round_sim.rounds[static_cast<size_t>(slot)];
    round.vel = {static_cast<float>(from_fixed(velocity.x)),
                 static_cast<float>(from_fixed(velocity.y)),
                 static_cast<float>(from_fixed(velocity.z))};
    world.round_sim.tick(world, nullptr);
    return FlightResult{
        round.active,
        round.age_ticks,
        FixedVec3{to_fixed(round.pos.x), to_fixed(round.pos.y), to_fixed(round.pos.z)},
        FixedVec3{to_fixed(round.vel.x), to_fixed(round.vel.y), to_fixed(round.vel.z)},
    };
}

void test_arming_dud_and_armed_damage() {
    Rig r;
    r.world.tables.ammo.entries[0].tracer_rate = 1;
    r.world.tables.ammo.entries[0].tracer_type_friendly = 7;
    r.world.tables.ammo.entries[0].tracer_type_enemy = 7;
    const int source_slot = r.fire();
    CHECK(source_slot >= 0);
    CHECK(r.world.round_sim.rounds[static_cast<size_t>(source_slot)].trail_slot >= 0);
    const LiveRound source =
        r.world.round_sim.rounds[static_cast<size_t>(source_slot)];
    r.world.round_sim.tick(r.world, nullptr); // age 1 < arm age 2
    CHECK(r.world.registry.get(r.target)->health == 100);
    CHECK(r.world.round_sim.hits.empty());
    CHECK(r.world.round_sim.deaths.empty());
    CHECK(r.world.round_sim.impacts.size() == 1);
    CHECK(r.world.round_sim.impacts[0].ammo_index == 1);

    // The dud is a live replacement projectile, not merely a different impact row.
    // The modeled fields inside retail's 692-byte copy survive; the trail slot at
    // byte +692 does not.  The dud definition supplies its own maximum lifetime.
    CHECK(r.world.round_sim.active_count == 1);
    const LiveRound &replacement =
        r.world.round_sim.rounds[static_cast<size_t>(source_slot)];
    CHECK(replacement.active);
    CHECK(replacement.ammo_index == 1);
    CHECK(replacement.owner == source.owner);
    CHECK(replacement.shooter_handle == source.shooter_handle);
    CHECK(replacement.age_ticks == 1);
    CHECK(replacement.max_age_ticks == 7);
    CHECK(to_fixed(replacement.vel.x) == to_fixed(source.vel.x));
    CHECK(to_fixed(replacement.vel.y) == to_fixed(source.vel.y));
    CHECK(to_fixed(replacement.vel.z) == to_fixed(source.vel.z));
    CHECK(to_fixed(replacement.pos.x) ==
          to_fixed(r.world.round_sim.impacts[0].position.x));
    CHECK(to_fixed(replacement.pos.y) ==
          to_fixed(r.world.round_sim.impacts[0].position.y));
    CHECK(to_fixed(replacement.pos.z) ==
          to_fixed(r.world.round_sim.impacts[0].position.z));
    CHECK(replacement.trail_slot == -1);

    // It remains in the ordinary flight loop under the DUD row on later ticks.
    r.world.registry.get(r.target)->position.y = 100.0f;
    const int32_t contact_x = to_fixed(replacement.pos.x);
    r.clear_events();
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(r.world.round_sim.active_count == 1);
    CHECK(replacement.active);
    CHECK(replacement.ammo_index == 1);
    CHECK(replacement.age_ticks == 2);
    CHECK(to_fixed(replacement.pos.x) > contact_x);
    CHECK(r.world.round_sim.impacts.empty());

    r.world.round_sim.reset();
    r.world.registry.get(r.target)->position.y = 0.0f;
    r.clear_events();
    r.world.tables.ammo.entries[0].arm_age_ticks = 0;
    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(r.world.registry.get(r.target)->health == 75);
    CHECK(r.world.round_sim.hits.size() == 1);
    CHECK(r.world.round_sim.hits[0].damage == 25);
    CHECK(r.world.round_sim.impacts.size() == 1);
    CHECK(r.world.round_sim.impacts[0].ammo_index == 0);
}

void test_missing_item_def_consumes_round_without_damage() {
    Rig r;
    r.world.tables.ammo.entries[0].arm_age_ticks = 0;
    Entity *target = r.world.registry.get(r.target);
    target->has_item_def = false;

    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(target->health == 100);
    CHECK(r.world.round_sim.hits.empty());
    CHECK(r.world.round_sim.deaths.empty());
    CHECK(r.world.round_sim.impacts.size() == 1);
    CHECK(r.world.round_sim.active_count == 0);
}

void test_damage_uses_retail_signed_wrap_and_ftol_cap() {
    Rig r;
    AmmoTableEntry &ammo = r.world.tables.ammo.entries[0];
    ammo.arm_age_ticks = 0;
    ammo.flags = 0x100u;
    ammo.weight_in_grains = 875;
    ammo.min_damage = -1000;
    ammo.max_damage = 0;
    Entity *target = r.world.registry.get(r.target);
    target->item_type = 5; // isolate kinetic arithmetic from person-zone scaling
    target->health = 1000;

    const int slot = r.fire();
    CHECK(slot >= 0);
    if (slot < 0) return;
    // The vector exceeds retail's 0x4EFFFE00 ftol cap. That exact cap is
    // 0x7FFF0000 as an integer; wrapped *62, then sar 16, produces -62.
    r.world.round_sim.rounds[static_cast<size_t>(slot)].vel = {32767.0f, 255.0f, 0.0f};
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(r.world.round_sim.hits.size() == 1);
    CHECK(r.world.round_sim.hits[0].damage == -62);
    CHECK(target->health == 1062);
}

void test_nodie_and_nontransparent_damage_gates() {
    Rig r;
    r.world.tables.ammo.entries[0].arm_age_ticks = 0;
    Entity *target = r.world.registry.get(r.target);
    target->health = 10;
    target->item_attrib = 0x40000000u; // ItemDefAttrib NoDie
    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(target->health == 1);
    CHECK(r.world.round_sim.hits.size() == 1);
    CHECK(r.world.round_sim.hits[0].damage == 9); // NoDie adjusts the applied/logged amount
    CHECK(r.world.round_sim.deaths.empty());

    // Retail has no lower clamp around this leaf: malformed state (zero health
    // while damage_state is still zero) turns health-1 into -1 applied damage.
    r.clear_events();
    target->health = 0;
    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(target->health == 1);
    CHECK(r.world.round_sim.hits.size() == 1);
    CHECK(r.world.round_sim.hits[0].damage == -1);

    r.clear_events();
    target->item_attrib = 0;
    target->health = 10;
    target->engine_flags |= 0x4000000u; // indestructible damage gate
    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(target->health == 10);
    CHECK(r.world.round_sim.hits.empty());
    CHECK(r.world.round_sim.impacts.size() == 1); // still a physical impact
    CHECK(r.world.round_sim.active_count == 0);

    r.clear_events();
    target->engine_flags = 0;
    target->health = 0;
    target->alive = false;
    Entity behind;
    behind.kind = EntityKind::Organic;
    behind.item_type = 3;
    behind.position = {8.0f, 0.0f, 0.0f};
    behind.health = 100;
    const EntityHandle farther = r.world.registry.spawn(0, behind);
    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(r.world.round_sim.impacts.size() == 1); // corpse consumed the round
    CHECK(r.world.registry.get(farther)->health == 100);
    CHECK(r.world.round_sim.hits.empty());
}

void test_posed_head_zone_multiplier() {
    World world;
    world.registry.configure_pool(0, 8);
    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    const EntityHandle sh = world.registry.spawn(0, shooter);
    Entity target;
    target.kind = EntityKind::Organic;
    target.has_item_def = true;
    target.item_type = 3;
    target.position = {5.0f, 0.0f, 0.0f};
    target.health = 1000;
    const EntityHandle th = world.registry.spawn(0, target);

    AmmoTableEntry ammo;
    ammo.name = "ZONE";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    ammo.weight_in_grains = 875;
    world.tables.ammo.entries.push_back(ammo);

    CollisionModel model;
    CollisionSection bone;
    bone.authored_bounds = true;
    bone.min_x = bone.min_y = bone.min_z = -0x10000;
    bone.max_x = bone.max_y = bone.max_z = 0x10000;
    bone.radius = 0x10000;
    model.sections.push_back(bone); // bone 0 is in the 1.25 head-zone group

    CollisionWorld collision;
    const int32_t model_id = collision.add_model(std::move(model));
    collision.assign_entity(th, model_id);
    const int32_t center[3] = {5 * 65536, 0, static_cast<int32_t>(0.9 * 65536.0)};
    CHECK(collision.publish_entity_section_matrices(
        th, {collision_matrix_from_heading(0, center)}));
    collision.build_tick_tables(world);
    world.collision = &collision;

    RoundSpawnParams params;
    params.owner = sh;
    params.shooter_handle = sh.packed;
    params.origin = {0.0f, 0.0f, 0.9f};
    params.ammo_index = 0;
    CHECK(world.round_sim.spawn(world, params) >= 0);
    world.round_sim.tick(world, nullptr);
    CHECK(world.round_sim.hits.size() == 1);
    CHECK(world.round_sim.hits[0].damage == 775); // 620 * 1.25
    CHECK(world.registry.get(th)->health == 225);
}

void test_item_type_zone_domain_and_attrib_0200_sections() {
    {
        auto r = std::make_unique<PosedDamageRig>(0);
        r->fire_and_tick();
        CHECK(r->world.round_sim.hits.size() == 1);
        CHECK(r->world.round_sim.hits[0].damage == 775); // person zone 0: 620 * 1.25
        CHECK(r->target_entity()->health == 4225);
        CHECK((r->target_entity()->flags & 0x800u) == 0);
    }
    {
        auto r = std::make_unique<PosedDamageRig>(0);
        r->target_entity()->item_type = 5;
        r->fire_and_tick();
        CHECK(r->world.round_sim.hits.size() == 1);
        CHECK(r->world.round_sim.hits[0].damage == 620); // zones are type-3-only
        CHECK(r->target_entity()->health == 4380);
    }
    {
        auto r = std::make_unique<PosedDamageRig>(0);
        r->target_entity()->item_attrib = 0x200u;
        r->fire_and_tick();
        CHECK(r->world.round_sim.hits.size() == 1);
        // The 0x200 branch uses its own section-code domain; section 0 is not
        // interpreted as an ordinary person head zone.
        CHECK(r->world.round_sim.hits[0].damage == 620);
        CHECK((r->target_entity()->flags & 0x800u) == 0);
    }

    {
        auto r = std::make_unique<PosedDamageRig>(13);
        r->fire_and_tick();
        CHECK(r->world.round_sim.hits.size() == 1);
        CHECK(r->world.round_sim.hits[0].damage == 1860); // ordinary critical zone: x3
        CHECK((r->target_entity()->flags & 0x800u) == 0); // cause is not an Entity Flags bit
        CHECK(r->world.round_sim.deaths.empty());
    }
    {
        auto r = std::make_unique<PosedDamageRig>(13);
        r->target_entity()->health = 1000;
        r->fire_and_tick();
        CHECK(r->world.round_sim.deaths.size() == 1);
        CHECK((r->world.round_sim.deaths[0].event_flags & 0x800u) != 0);
        CHECK((r->target_entity()->flags & 0x800u) == 0);
    }

    // ItemDefAttrib 0x200 selects the retail section-code switch. These four
    // codes receive 6.0x; the literal cases prevent a range approximation.
    const int special_sections[] = {2, 3, 6, 7};
    for (int section : special_sections) {
        auto r = std::make_unique<PosedDamageRig>(section);
        r->target_entity()->item_attrib = 0x200u;
        r->fire_and_tick();
        CHECK(r->world.round_sim.hits.size() == 1);
        CHECK(r->world.round_sim.hits[0].damage == 3720); // 620 * 6.0
        CHECK(r->target_entity()->health == 1280);
        CHECK((r->target_entity()->flags & 0x800u) == 0);
    }

    // A walk crossing TWO spheres splits the channels: ray[31] keeps the first/
    // highest overlap (7) while ray[32] ends at the final/lowest (4). The
    // attrib-0x200 seat switch reads ray[31] (hitZoneData+124 @0x4ec977).
    {
        auto r = std::make_unique<PosedDamageRig>(7, 4);
        r->target_entity()->item_attrib = 0x200u;
        r->fire_and_tick();
        CHECK(r->world.round_sim.hits.size() == 1);
        CHECK(r->world.round_sim.hits[0].damage == 3720); // seat code 7 via ray[31]
        CHECK((r->target_entity()->flags & 0x800u) == 0);
    }
    // The normal-infantry table keeps reading ray[32] (@0x4ec9bf): final zone 4
    // takes 1.25x even though the primary bone 7 sits in the 1.0x band.
    {
        auto r = std::make_unique<PosedDamageRig>(7, 4);
        r->fire_and_tick();
        CHECK(r->world.round_sim.hits.size() == 1);
        CHECK(r->world.round_sim.hits[0].damage == 775); // 620 * 1.25 via ray[32]
        CHECK((r->target_entity()->flags & 0x800u) == 0);
    }
}

void test_shooter_damage_class_runs_after_zone_truncation() {
    {
        PosedDamageRig r(0);
        r.ammo().weight_in_grains = 13;
        r.shooter_entity()->ammo_damage_class = {1}; // x0.9
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        // base=floor(620*13/875)=9; zone trunc=floor(9*1.25)=11;
        // class trunc=floor(11*0.9f)=9. A combined multiply would produce 10.
        CHECK(r.world.round_sim.hits[0].damage == 9);
    }
    {
        PosedDamageRig r(0);
        r.ammo().weight_in_grains = 10;
        r.shooter_entity()->ammo_damage_class = {2}; // x1.1
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        // base=7; zone trunc=8; class trunc=floor(8*1.1f)=8.
        // A combined multiply would produce 9.
        CHECK(r.world.round_sim.hits[0].damage == 8);
    }
}


void test_body_armor_energy_and_impact_row() {
    {
        PosedDamageRig r(4);
        r.target_entity()->carry_flags = 8;
        r.ammo().armor_density[0] = 100;
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        if (!r.world.round_sim.hits.empty())
            CHECK(r.world.round_sim.hits[0].damage == 491);
        // First loss: sqrt(620^2 - 200000000/875) -> 394 m/s.
        // Quantizing back to Q16/tick gives 393 for damage, then x1.25 -> 491.
        // The second loss consumes the remaining energy.
        CHECK(r.world.round_sim.rounds[0].vel.x == 0.0f);
        CHECK(r.world.round_sim.impacts.size() == 2);
        if (r.world.round_sim.impacts.size() == 2) {
            CHECK(r.world.round_sim.impacts[0].effect_tag == 24);
            CHECK(r.world.round_sim.impacts[0].present_effect);
            CHECK(r.world.round_sim.impacts[0].source_order <
                  r.world.round_sim.impacts[1].source_order);
        }
    }
    for (uint8_t damage_class = 0; damage_class < 3; ++damage_class) {
        PosedDamageRig r(0);
        r.target_entity()->carry_flags = 8;
        r.shooter_entity()->ammo_damage_class = {damage_class};
        r.ammo().armor_density[0] = r.ammo().armor_density[1] =
                r.ammo().armor_density[2] = 1000;
        r.ammo().armor_density[damage_class] = 0;
        r.fire_and_tick();
        const int damage[] = {775, 697, 852};
        CHECK(r.world.round_sim.hits.size() == 1);
        if (!r.world.round_sim.hits.empty())
            CHECK(r.world.round_sim.hits[0].damage == damage[damage_class]);
    }
    for (int section : {4, 5, 13}) {
        PosedDamageRig r(section);
        r.target_entity()->carry_flags = 8;
        r.ammo().armor_density[0] = 1000; // stops a torso round entirely
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.empty() == (section == 4));
        CHECK(r.world.round_sim.impacts.size() == (section == 4 ? 2u : 1u));
    }
    {
        PosedDamageRig r(4);
        r.ammo().armor_density[0] = 1000; // no equipped armor
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits[0].damage == 775);
        CHECK(r.world.round_sim.impacts.size() == 1);
    }
    {
        PosedDamageRig r(2);
        r.target_entity()->carry_flags = 8;
        r.target_entity()->item_attrib = 0x200;
        r.ammo().armor_density[0] = 1000;
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits[0].damage == 3720); // Landable section domain
    }
    {
        PosedDamageRig r(0);
        r.target_entity()->carry_flags = 8;
        r.ammo().armor_density[0] = 1000;
        r.world.rules.mp_session = true;
        r.world.rules.projectile_authority = true;
        r.world.rules.one_shot_kill = true;
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits[0].damage == 2000); // early rule bypass
    }
}

// Every type-3 hit runs the (1, 35) drag leg after the armor arms, armored
// or not; a non-person target returns before it and keeps its velocity.
// [orig: Weapon_CalcImpactDamage @0x4ecb96 (type-3 gate), @0x4ecc38..0x4ecc42]
void test_person_hit_applies_the_surface_drag_leg() {
    for (const int item_type : {3, 5}) {
        PosedDamageRig r(4);
        r.target_entity()->item_type = item_type;
        r.ammo().drag_fp16 = 0x10000; // a live coefficient; no body armor
        r.fire();
        const Vec3 spawn_vel = r.world.round_sim.rounds[0].vel;
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.hits.size() == 1);
        FixedVec3 expected{to_fixed(spawn_vel.x), to_fixed(spawn_vel.y), to_fixed(spawn_vel.z)};
        if (item_type == 3) projectile_apply_person_hit_drag(expected, r.ammo());
        const Vec3 stored = r.world.round_sim.rounds[0].vel;
        CHECK(stored.x == static_cast<float>(from_fixed(expected.x)));
        CHECK(stored.z == static_cast<float>(from_fixed(expected.z)));
        if (item_type == 3) {
            CHECK(expected.x > 0 && expected.x < to_fixed(spawn_vel.x));
        } else {
            CHECK(stored.x == spawn_vel.x);
        }
    }
}

void test_network_oneshot_authority_and_session_gate() {
    {
        Rig r;
        r.world.tables.ammo.entries[0].arm_age_ticks = 0;
        r.world.rules.mp_session = true;
        r.world.rules.projectile_authority = true;
        r.world.rules.one_shot_kill = true;
        r.world.registry.get(r.target)->health = 3000;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 2000);
        CHECK(r.world.registry.get(r.target)->health == 1000);
        // The early MP option return bypasses the authored max_damage=25.
        CHECK(r.world.tables.ammo.entries[0].max_damage == 25);
    }
    {
        Rig r;
        r.world.tables.ammo.entries[0].arm_age_ticks = 0;
        r.world.rules.mp_session = false;
        r.world.rules.one_shot_kill = true;
        r.world.registry.get(r.target)->health = 3000;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 25);
        CHECK(r.world.registry.get(r.target)->health == 2975);
    }
    {
        Rig r;
        r.world.tables.ammo.entries[0].arm_age_ticks = 0;
        r.world.rules.mp_session = true;
        r.world.rules.projectile_authority = false;
        r.world.rules.one_shot_kill = true;
        // A visual client's only native live pool-0 person is its own player L;
        // any synthetic non-local native rows are excluded from projectile walks
        // because remote pool-0 poses arrive as wire proxies.
        r.world.cached.local_player = r.target;
        Entity *target = r.world.registry.get(r.target);
        CHECK(target != nullptr);
        if (target == nullptr) return;
        // Pin the independent retail world-role gate. Even if a client caller
        // forgets to mark its round VisualOnly, a non-authority network world
        // cannot normalize entity words or queue the impact kill zone.
        target->health = 0x1FFFF;
        target->health_max = 0x1FFFE;
        AmmoTableEntry &ammo = r.world.tables.ammo.entries[0];
        ammo.kztype = ammo_kz::kStandard;
        ammo.kz_maxradius = 5.0f;
        ammo.kz_damage = 50;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(target->health == 0x1FFFF);
        CHECK(target->health_max == 0x1FFFE);
        CHECK(r.world.round_sim.hits.empty());
        CHECK(r.world.explosions.queue.empty());
        CHECK(r.world.round_sim.impacts.size() == 1); // client prediction remains visual
        CHECK(r.world.round_sim.active_count == 0);

        // The same world-role gate covers spawn-time instant kill zones.
        AmmoTableEntry instant;
        instant.name = "CLIENT_INSTANT";
        instant.valid = true;
        instant.flags = 0x400u;
        instant.kztype = ammo_kz::kStandard;
        instant.kz_maxradius = 4.0f;
        instant.kz_damage = 40;
        const int instant_index = static_cast<int>(r.world.tables.ammo.entries.size());
        r.world.tables.ammo.entries.push_back(instant);
        RoundSpawnParams instant_params;
        instant_params.owner = r.shooter;
        instant_params.shooter_handle = r.shooter.packed;
        instant_params.origin = {1.0f, 0.0f, 1.0f};
        instant_params.ammo_index = instant_index;
        CHECK(r.world.round_sim.spawn(r.world, instant_params) < 0);
        CHECK(r.world.explosions.queue.empty());
    }
}

void test_visual_only_rounds_have_no_gameplay_consequences() {
    Rig r;
    r.world.rules.mp_session = true;
    r.world.rules.projectile_authority = false;
    r.world.rules.one_shot_kill = true;
    // The consequence gates are proven against the one local person a visual
    // client still collides with: its own player L (ghost slots are excluded).
    r.world.cached.local_player = r.target;
    Entity *shooter = r.world.registry.get(r.shooter);
    Entity *target = r.world.registry.get(r.target);
    CHECK(shooter != nullptr && target != nullptr);
    if (shooter == nullptr || target == nullptr) return;

    shooter->group_id = 3;
    shooter->net_id = 7;
    target->group_id = 4;
    target->net_id = 9;
    // Values outside signed-word range catch the old consequence-boundary
    // normalization even when calculated damage happened to be zero.
    target->health = 0x1FFFF;
    target->health_max = 0x1FFFE;
    target->armor_impact = 0x1FFFD;
    target->armor_kz = 0x1FFFC;
    target->flags = 0x10000u;
    target->last_attacker = r.shooter;
    target->death_anim_state = 77;
    const Entity before = *target;

    AmmoTableEntry &bullet = r.world.tables.ammo.entries[0];
    bullet.arm_age_ticks = 0;
    bullet.kztype = ammo_kz::kStandard;
    bullet.kz_maxradius = 5.0f;
    bullet.kz_damage = 50;
    CHECK(r.fire(RoundConsequenceMode::VisualOnly) >= 0);
    r.world.round_sim.tick(r.world, nullptr);

    CHECK(r.world.round_sim.impacts.size() == 1);
    CHECK(r.world.round_sim.hits.empty());
    CHECK(r.world.round_sim.deaths.empty());
    CHECK(r.world.explosions.queue.empty());
    CHECK(target->health == before.health);
    CHECK(target->health_max == before.health_max);
    CHECK(target->armor_impact == before.armor_impact);
    CHECK(target->armor_kz == before.armor_kz);
    CHECK(target->flags == before.flags);
    CHECK(target->last_attacker == before.last_attacker);
    // The person class callback still fires on the non-authority peer's own
    // body (the call follows the authority-gated subtraction, with the forced
    // zero damage) and its event-1 leg stages the bone/quadrant bullet death
    // in +0x2C0: the organic stand-in is synthetic torso 1 and the +X round on
    // a heading-0 body is quadrant 3. Presentation, not a gameplay consequence.
    // [orig: Projectile_ProcessDamageOnTarget @0x4e820e;
    //  Entity_HandleDamageAndTriggerZones @0x4077eb select]
    CHECK(target->death_anim_state ==
          compute_death_anim_state(1, 3, death_cause::kBullet));
    CHECK(!r.world.script.relations.group_group(
        TriggerRelations::kShot, shooter->group_id, target->group_id));
    CHECK(!r.world.script.relations.single_group(
        TriggerRelations::kShot, shooter->net_id, target->group_id));
    CHECK(!r.world.script.relations.group_single(
        TriggerRelations::kShot, shooter->group_id, target->net_id));
    CHECK(!r.world.script.relations.single_single(
        TriggerRelations::kShot, shooter->net_id, target->net_id));
    CHECK(r.world.script.relations.group(target->group_id).alert ==
          TriggerRelations::kAlertGreen);

    auto entity_count = [&]() {
        int count = 0;
        r.world.registry.for_each([&](const Entity &) { ++count; });
        return count;
    };
    const int entities_before = entity_count();
    const size_t devices_before = r.world.throwables.devices.size();

    // Spawn-time instant killzones still emit their visual impact descriptor,
    // but may not queue the authoritative explosion.
    AmmoTableEntry instant;
    instant.name = "VISUAL_INSTANT";
    instant.valid = true;
    instant.flags = 0x400u;
    instant.kztype = ammo_kz::kStandard;
    instant.kz_maxradius = 4.0f;
    instant.kz_damage = 40;
    const int instant_index = static_cast<int>(r.world.tables.ammo.entries.size());
    r.world.tables.ammo.entries.push_back(instant);
    RoundSpawnParams instant_params;
    instant_params.owner = r.shooter;
    instant_params.shooter_handle = r.shooter.packed;
    instant_params.origin = {1.0f, 0.0f, 1.0f};
    instant_params.ammo_index = instant_index;
    const size_t impacts_before = r.world.round_sim.impacts.size();
    CHECK(r.world.round_sim.spawn(r.world, instant_params,
                                  RoundConsequenceMode::VisualOnly) < 0);
    CHECK(r.world.round_sim.impacts.size() == impacts_before + 1);
    CHECK(r.world.explosions.queue.empty());

    // A visual use-own-move charge reaches the exact retail rest conversion,
    // then releases without cloning a placed-device entity.
    AmmoTableEntry charge;
    charge.name = "VISUAL_CHARGE";
    charge.valid = true;
    charge.flags = 0x2000u;
    charge.velocity = 62;
    charge.max_age_ticks = 20;
    charge.drag_fp16 = 65536;
    const int charge_index = static_cast<int>(r.world.tables.ammo.entries.size());
    r.world.tables.ammo.entries.push_back(charge);
    RoundSpawnParams charge_params;
    charge_params.owner = r.shooter;
    charge_params.shooter_handle = r.shooter.packed;
    charge_params.origin = {0.0f, 0.0f, 0.0f};
    charge_params.ammo_index = charge_index;
    const int charge_slot = r.world.round_sim.spawn(
        r.world, charge_params, RoundConsequenceMode::VisualOnly);
    CHECK(charge_slot >= 0);
    if (charge_slot >= 0) {
        LiveRound &round =
            r.world.round_sim.rounds[static_cast<size_t>(charge_slot)];
        round.motor = ThrowClass::kSatchel;
        round.vel = Vec3{};
    }
    std::vector<uint16_t> heights(512u * 512u, 0);
    std::vector<int> sectors(256u, 1);
    opennova::terrain::TerrainHeightField flat;
    flat.heightmap = heights.data();
    flat.dim = 512;
    flat.layout.sector_grid = sectors.data();
    r.world.round_sim.tick(r.world, &flat, nullptr);
    CHECK(r.world.throwables.devices.size() == devices_before);
    CHECK(entity_count() == entities_before);
    CHECK(r.world.explosions.queue.empty());
}

void test_visual_person_proxy_keeps_wire_identity_out_of_authority() {
    World world;
    world.registry.configure_pool(0, 8);

    // Occupy local slot zero so valid wire H=0 and local L=1 are distinct.
    // The hidden dummy remains in the registry person table but cannot collide.
    Entity dummy;
    dummy.kind = EntityKind::Organic;
    dummy.hidden = true;
    dummy.position = {100.0f, 100.0f, 0.0f};
    CHECK(world.registry.spawn(0, dummy).packed == 0);

    Entity local;
    local.kind = EntityKind::Organic;
    local.has_item_def = true;
    local.item_type = 3;
    local.position = {8.0f, 0.0f, 0.0f};
    local.health = 100;
    const EntityHandle local_l = world.registry.spawn(0, local);
    CHECK(local_l.packed == 1);
    world.cached.local_player = local_l;

    // H=0 is the local player's valid server identity. The remote H deliberately
    // aliases local L's packed value: excluding owners by casting H -> L would
    // drop the actual remote target and make this trace miss.
    constexpr uint16_t self_h = 0;
    std::vector<WirePersonCollisionProxy> proxies{
        WirePersonCollisionProxy{self_h, FixedVec3{2 * 65536, 0, 0}},
        WirePersonCollisionProxy{local_l.packed, FixedVec3{5 * 65536, 0, 0}},
    };
    CollisionWorld collision;
    collision.replace_wire_collision_proxies(proxies, {}, self_h);
    collision.build_tick_tables(world);
    world.collision = &collision;

    ProjectileTrace trace;
    trace.start = FixedVec3{0, 0, 58982};
    trace.end = FixedVec3{10 * 65536, 0, 58982};
    trace.owner = local_l;
    trace.shooter_wire_handle = self_h;
    CHECK(!collision.trace_projectile(world, trace).hit());

    trace.include_wire_proxies = true;
    const ProjectileHit hit = collision.trace_projectile(world, trace);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(!hit.geometry_entity.valid());
    CHECK(hit.bone_index == -1 && hit.hit_zone == -1);
    CHECK(hit.position_q16.x > 4 * 65536);
    CHECK(hit.position_q16.x < 5 * 65536);

    AmmoTableEntry ammo;
    ammo.name = "VISUAL_PROXY";
    ammo.valid = true;
    ammo.velocity = 620; // 10 units/tick
    ammo.max_age_ticks = 20;
    ammo.weight_in_grains = 875;
    ammo.max_damage = 25;
    world.tables.ammo.entries.push_back(ammo);
    world.rules.mp_session = true;
    world.rules.projectile_authority = false;

    RoundSpawnParams params;
    params.owner = local_l;
    params.shooter_handle = self_h;
    params.origin = {0.0f, 0.0f, kOrganicStandInCenterZ};
    params.ammo_index = 0;
    CHECK(world.round_sim.spawn(
              world, params, RoundConsequenceMode::VisualOnly) >= 0);
    world.round_sim.tick(world, nullptr, &collision);

    CHECK(world.round_sim.impacts.size() == 1);
    // A decoded remote-player proxy is a NON-LOCAL person, so it takes the flesh
    // row, not the local player's. It carries no registry entity, so retail's
    // same-group/half-health suppression cannot be evaluated for it and the effect
    // is emitted unconditionally.
    // [orig: Projectile_HandleTerrainImpact_0 @ 0x4e98f0 — the non-local leg
    //  @0x4e9aa5, push 17h @0x4e9ad7]
    CHECK(world.round_sim.impacts[0].effect_tag == 23);
    CHECK(world.round_sim.impacts[0].present_effect);
    CHECK(to_fixed(world.round_sim.impacts[0].position.x) == hit.position_q16.x);
    // A person stop's descriptor carries the ray record, whose +24..+32 is the
    // round's normalized flight direction (+x here) [orig:
    // Projectile_HandleTerrainImpact_0 @0x4e9a57; the record layout in
    // Projectile_UpdatePhysics @0x4ea241..0x4ea25f].
    CHECK(world.round_sim.impacts[0].direction.x > 0.99f);
    CHECK(world.round_sim.impacts[0].direction.y < 0.01f);
    CHECK(world.round_sim.impacts[0].direction.y > -0.01f);
    CHECK(world.round_sim.hits.empty());
    CHECK(world.round_sim.deaths.empty());
    CHECK(world.explosions.queue.empty());
    CHECK(world.registry.get(local_l)->health == 100);
    CHECK(world.round_sim.debug_trail_count == 1);
    CHECK(world.round_sim.debug_trail[0].entity == EntityHandle::kInvalid);
}

// A one-section q8 CFAC quad (two triangles) at section-local z = 1, normal +z,
// spanning (+-1, +-1) — the same fixture shape the collision suite's face-walk
// tests use, here exercised through the wire dynamic-proxy pass.
CollisionModel proxy_face_quad_model(uint8_t material) {
    CollisionModel m;
    m.face_vertices = {{-256, -256, 256}, {256, -256, 256}, {256, 256, 256},
                       {-256, 256, 256}}; // Q8: (+-1, +-1, 1)
    auto face = [&](int a, int b, int c) {
        CollisionFace f;
        f.v[0] = static_cast<int16_t>(a);
        f.v[1] = static_cast<int16_t>(b);
        f.v[2] = static_cast<int16_t>(c);
        f.normal[0] = 0;
        f.normal[1] = 0;
        f.normal[2] = 16384;
        f.axis = 1;
        f.plane_dist = -0x10000; // on-plane at z=1
        f.min[0] = -0x10000; f.max[0] = 0x10000;
        f.min[1] = -0x10000; f.max[1] = 0x10000;
        f.min[2] = 0x10000;  f.max[2] = 0x10000;
        f.material = material;
        m.faces.push_back(f);
    };
    face(0, 1, 2);
    face(0, 2, 3);
    m.sections.assign(1, {});
    m.sections[0].face_start = 0;
    m.sections[0].face_count = 2;
    m.sections[0].face_vertex_start = 0;
    m.sections[0].face_vertex_count = 4;
    return m;
}

// The Knife leaf uses a person's authored CFAC mesh rather than the ordinary
// projectile bone spheres. This vertical quad is centered at the entity pose;
// a +X ray meets it exactly at the target's X coordinate.
CollisionModel knife_person_face_model(uint8_t material) {
    CollisionModel m;
    m.face_vertices = {{0, -256, 0}, {0, 256, 0}, {0, 256, 512},
                       {0, -256, 512}}; // Q8: x=0, y=+-1, z=0..2
    auto face = [&](int a, int b, int c) {
        CollisionFace f;
        f.v[0] = static_cast<int16_t>(a);
        f.v[1] = static_cast<int16_t>(b);
        f.v[2] = static_cast<int16_t>(c);
        f.normal[0] = -16384;
        f.normal[1] = 0;
        f.normal[2] = 0;
        f.axis = 4; // YZ projection
        f.plane_dist = 0;
        f.min[0] = 0;          f.max[0] = 0;
        f.min[1] = -0x10000;   f.max[1] = 0x10000;
        f.min[2] = 0;          f.max[2] = 0x20000;
        f.material = material;
        m.faces.push_back(f);
    };
    face(0, 1, 2);
    face(0, 2, 3);
    m.sections.assign(1, {});
    m.sections[0].face_start = 0;
    m.sections[0].face_count = 2;
    m.sections[0].face_vertex_start = 0;
    m.sections[0].face_vertex_count = 4;
    return m;
}

void test_knife_instant_kill_zone_raycast() {
    World world;
    world.registry.configure_pool(0, 8);
    world.registry.configure_pool(1, 8);

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    const EntityHandle sh = world.registry.spawn(0, shooter);

    Entity target;
    target.kind = EntityKind::Organic;
    target.has_item_def = true;
    target.item_type = 3;
    target.position = {2.0f, 0.0f, 0.0f};
    target.health = 100;
    const EntityHandle th = world.registry.spawn(0, target);

    CollisionWorld collision;
    const int32_t model_id = collision.add_model(
        knife_person_face_model(/*material=*/7));
    collision.assign_entity(th, model_id);
    const int32_t target_pose[3] = {2 * 65536, 0, 0};
    CHECK(collision.publish_entity_section_matrices(
        th, {collision_matrix_from_heading(0, target_pose)}));
    collision.build_tick_tables(world);
    world.collision = &collision;

    AmmoTableEntry knife;
    knife.name = "KNIFE";
    knife.valid = true;
    knife.flags = kAmmoFlagInstantKillZone;
    knife.kztype = ammo_kz::kKnife;
    knife.kz_maxradius = 3.0f;
    world.tables.ammo.entries.push_back(knife);

    RoundSpawnParams params;
    params.owner = sh;
    params.shooter_handle = sh.packed;
    params.origin = {0.0f, 0.0f, 1.0f};
    params.dir_yaw_bam = 0;
    params.dir_pitch_bam = 0;
    params.ammo_index = 0;

    CHECK(world.round_sim.spawn(world, params) < 0);
    CHECK(world.round_sim.active_count == 0);
    CHECK(world.explosions.queue.size() == 1);
    CHECK(world.round_sim.hits.empty());
    CHECK(world.registry.get(th)->health == 100);
    CHECK(world.round_sim.impacts.size() == 1);
    if (!world.round_sim.impacts.empty()) {
        const RoundImpact &impact = world.round_sim.impacts[0];
        const int32_t impact_x = to_fixed(impact.position.x);
        CHECK(impact_x >= 2 * 65536 - 2 && impact_x <= 2 * 65536);
        CHECK(to_fixed(impact.position.y) == 0);
        CHECK(to_fixed(impact.position.z) == 65536);
        CHECK(to_fixed(impact.direction.x) == 65536);
        CHECK(to_fixed(impact.direction.y) == 0);
        CHECK(to_fixed(impact.direction.z) == 0);
        CHECK(impact.effect_tag == 11); // CFAC material 7 + 4
    }

    // Moving the same face beyond the authored extent leaves the presenter
    // silent while the instant-kill-zone explosion remains queued at the hand.
    world.registry.get(th)->position.x = 4.0f;
    const int32_t far_pose[3] = {4 * 65536, 0, 0};
    CHECK(collision.publish_entity_section_matrices(
        th, {collision_matrix_from_heading(0, far_pose)}));
    collision.build_tick_tables(world);
    world.round_sim.impacts.clear();
    world.explosions.queue.clear();
    CHECK(world.round_sim.spawn(world, params) < 0);
    CHECK(world.round_sim.active_count == 0);
    CHECK(world.explosions.queue.size() == 1);
    CHECK(world.round_sim.impacts.empty());

    // The PERSON leg remaps CFAC material 1 to the flesh row (23): hit type 3
    // is Projectile_RaycastProximitySlots' default (person) slot walk
    // [orig: Weapon_RaycastAndSpawnImpact @0x4e8880..0x4e8888].
    Entity flesh_target;
    flesh_target.kind = EntityKind::Organic;
    flesh_target.has_item_def = true;
    flesh_target.item_type = 3;
    flesh_target.position = {1.0f, 0.0f, 0.0f};
    flesh_target.health = 100;
    const EntityHandle fh = world.registry.spawn(0, flesh_target);
    const int32_t flesh_model_id = collision.add_model(
        knife_person_face_model(/*material=*/1));
    collision.assign_entity(fh, flesh_model_id);
    const int32_t flesh_pose[3] = {1 * 65536, 0, 0};
    CHECK(collision.publish_entity_section_matrices(
        fh, {collision_matrix_from_heading(0, flesh_pose)}));
    collision.build_tick_tables(world);
    world.round_sim.impacts.clear();
    world.explosions.queue.clear();
    CHECK(world.round_sim.spawn(world, params) < 0);
    CHECK(world.explosions.queue.size() == 1);
    CHECK(world.registry.get(fh)->health == 100);
    CHECK(world.round_sim.impacts.size() == 1);
    if (!world.round_sim.impacts.empty()) {
        const RoundImpact &impact = world.round_sim.impacts[0];
        const int32_t impact_x = to_fixed(impact.position.x);
        CHECK(impact_x >= 1 * 65536 - 2 && impact_x <= 1 * 65536);
        CHECK(impact.effect_tag == 23);
    }

    // The ITEM leg (hit type 2) is plain material + 4 — a pool-1 item's
    // material 1 is NOT flesh — and, being closer, it wins the strict
    // arbitration over the person behind it
    // [orig: @0x4e87cb item leg (<); @0x4e8867 material + 4].
    Entity item;
    item.kind = EntityKind::Item;
    item.has_item_def = true;
    item.item_type = 3;
    item.position = {0.5f, 0.0f, 0.0f};
    item.bound_radius = 1.0f;
    const EntityHandle ih = world.registry.spawn(1, item);
    const int32_t item_model_id = collision.add_model(
        knife_person_face_model(/*material=*/1));
    collision.assign_entity(ih, item_model_id);
    const int32_t item_pose[3] = {32768, 0, 0};
    CHECK(collision.publish_entity_section_matrices(
        ih, {collision_matrix_from_heading(0, item_pose)}));
    collision.build_tick_tables(world);
    world.round_sim.impacts.clear();
    world.explosions.queue.clear();
    CHECK(world.round_sim.spawn(world, params) < 0);
    CHECK(world.round_sim.impacts.size() == 1);
    if (!world.round_sim.impacts.empty()) {
        const RoundImpact &impact = world.round_sim.impacts[0];
        const int32_t impact_x = to_fixed(impact.position.x);
        CHECK(impact_x >= 32768 - 2 && impact_x <= 32768);
        CHECK(impact.effect_tag == 5);
    }
}

// Moving decoded pool-1 movers collide through their wire-keyed authored
// geometry at the DECODED pose, while a synthetic duplicate native row does
// not serve projectile collision on a visual client.
void test_visual_dynamic_proxy_projects_decoded_pose_geometry() {
    World world;
    world.registry.configure_pool(0, 8);
    world.registry.configure_pool(1, 8);
    world.rules.mp_session = true;
    world.rules.projectile_authority = false;

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    shooter.position = {0.0f, 0.0f, 0.0f};
    const EntityHandle sh = world.registry.spawn(0, shooter);
    world.cached.local_player = sh;

    // A synthetic duplicate native row: the same vehicle the wire also carries,
    // at authored spawn X=5 rather than the host's current pose.
    Entity ghost;
    ghost.kind = EntityKind::Item;
    ghost.has_item_def = true;
    ghost.position = {5.0f, 0.0f, 0.0f};
    const EntityHandle gh = world.registry.spawn(1, ghost);

    CollisionWorld collision;
    const int32_t model_id =
        collision.add_model(proxy_face_quad_model(/*material=*/7));
    collision.assign_entity(gh, model_id);
    collision.build_tick_tables(world);
    world.collision = &collision;

    // The decoded wire pose: the same authored geometry at X=12. The quad
    // plane sits at proxy-local z=1, so a ray descending through world
    // z=1 over (12, 0) crosses it.
    WireDynamicCollisionProxy proxy;
    proxy.wire_handle = 0x1002; // host pool-1 slot 2
    proxy.model_id = model_id;
    proxy.position_q16 = FixedVec3{12 * 65536, 0, 0};
    proxy.bound_radius_q16 = 3 * 65536;
    collision.replace_wire_collision_proxies({}, {proxy});

    auto trace_down_at = [&](int32_t x_q16, bool include) {
        ProjectileTrace trace;
        trace.start = FixedVec3{x_q16, 0, 3 * 65536};
        trace.end = FixedVec3{x_q16, 0, -3 * 65536};
        trace.owner = sh;
        trace.include_wire_proxies = include;
        return collision.trace_projectile(world, trace);
    };

    // The stale local pose no longer stops rounds on a visual client...
    CHECK(!trace_down_at(5 * 65536, true).hit());
    // ...while the decoded pose does, through the authored CFAC mesh.
    const ProjectileHit hit = trace_down_at(12 * 65536, true);
    CHECK(hit.hit_class == ProjectileHitClass::DynamicEntity);
    CHECK(!hit.geometry_entity.valid());
    CHECK(hit.surface_type == 7);
    // The proxy-local z=1 plane, unrotated pose (the face walk's distance
    // split quantizes within a few Q16 ULPs).
    CHECK(hit.position_q16.z > 0xF000);
    CHECK(hit.position_q16.z < 0x11000);
    // A round that never opted into wire proxies (an authoritative round
    // mistakenly stepped on a client world) sees neither representation.
    CHECK(!trace_down_at(12 * 65536, false).hit());

    // The same world WITH authority keeps the ordinary local-table behavior.
    world.rules.projectile_authority = true;
    CHECK(trace_down_at(5 * 65536, false).hit_class ==
          ProjectileHitClass::DynamicEntity);
    world.rules.projectile_authority = false;

    // The wire projection consumes the SAME effective items.def scale as its
    // visual and local collision twin. A ray 1.5u off-center misses the
    // unscaled +-1 quad, then hits after the exact 2x basis is installed; the
    // local z=1 face moves to world z=2 as well.
    CHECK(!trace_down_at(12 * 65536 + 0x18000, true).hit());
    WireDynamicCollisionProxy scaled = proxy;
    scaled.uniform_scale_q16 = 0x20000;
    scaled.bound_radius_q16 = 6 * 65536;
    collision.replace_wire_collision_proxies({}, {scaled});
    const ProjectileHit scaled_hit =
        trace_down_at(12 * 65536 + 0x18000, true);
    CHECK(scaled_hit.hit_class == ProjectileHitClass::DynamicEntity);
    CHECK(scaled_hit.position_q16.z > 0x1F000);
    CHECK(scaled_hit.position_q16.z < 0x21000);

    // The decoded heading rotates the authored geometry with the visual: at
    // heading 90 deg the (+-1, +-1) quad still spans the section origin, but a
    // proxy REPOSED under pitch 90 deg turns the z=1 plane vertical and the
    // descending ray at its center now passes through where the flat plane
    // would have stopped it.
    WireDynamicCollisionProxy pitched = proxy;
    pitched.pitch_bam = 0x40000000;
    collision.replace_wire_collision_proxies({}, {pitched});
    CHECK(!trace_down_at(12 * 65536, true).hit());

    // A horizontal ray across the now-vertical plane hits it instead.
    ProjectileTrace across;
    across.start = FixedVec3{9 * 65536, 0, 0};
    across.end = FixedVec3{15 * 65536, 0, 0};
    across.owner = sh;
    across.include_wire_proxies = true;
    CHECK(collision.trace_projectile(world, across).hit_class ==
          ProjectileHitClass::DynamicEntity);
}

// The decoded shooter's own carrier is excluded exactly like the retail
// ray[18] mount exclusion; an unrelated proxy behind it still stops the round.
// A pool-1 proxy without its initialized model is an impossible client-entity
// state and raises instead of acquiring replacement collision geometry.
// [orig: Entity_InitFromModel @0x40DC30;
// Projectile_RaycastProximitySlots @0x4E5340 ->
// Physics_RaycastAgainstBoneCollision @0x4E4CB0]
void test_visual_dynamic_proxy_carrier_gate_and_unresolved_model_raises() {
    World world;
    world.registry.configure_pool(0, 4);
    world.rules.mp_session = true;
    world.rules.projectile_authority = false;

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    const EntityHandle sh = world.registry.spawn(0, shooter);
    world.cached.local_player = sh;

    CollisionWorld collision;
    const int32_t model_id =
        collision.add_model(proxy_face_quad_model(/*material=*/9));
    collision.build_tick_tables(world);
    world.collision = &collision;

    WireDynamicCollisionProxy carrier;
    carrier.wire_handle = 0x1004;
    carrier.model_id = model_id;
    carrier.position_q16 = FixedVec3{6 * 65536, 0, 0};
    carrier.pitch_bam = 0x40000000; // vertical plane across the lane
    carrier.bound_radius_q16 = 3 * 65536;
    WireDynamicCollisionProxy behind = carrier;
    behind.wire_handle = 0x1007;
    behind.position_q16 = FixedVec3{10 * 65536, 0, 0};
    collision.replace_wire_collision_proxies({}, {carrier, behind});

    ProjectileTrace trace;
    trace.start = FixedVec3{0, 0, 0};
    trace.end = FixedVec3{14 * 65536, 0, 0};
    trace.owner = sh;
    trace.include_wire_proxies = true;
    trace.shooter_wire_handle = 0x0003;
    trace.shooter_carrier_wire_handle = 0x1004;
    const ProjectileHit hit = collision.trace_projectile(world, trace);
    CHECK(hit.hit_class == ProjectileHitClass::DynamicEntity);
    // The carrier at X=6 was ridden through; the unrelated proxy at X=10
    // stops the round on its vertical plane (X=10 +- 1 depending on the
    // rotation sign). Anything past X=8 proves the carrier plane (<= 7) was
    // skipped rather than struck.
    CHECK(hit.position_q16.x > 8 * 65536);
    CHECK(hit.position_q16.x < 12 * 65536);

    // Retail cannot enter the ordinary pool-1 face walk with this state.
    WireDynamicCollisionProxy unresolved;
    unresolved.wire_handle = 0x1009;
    unresolved.model_id = -1;
    unresolved.position_q16 = FixedVec3{5 * 65536, 0, 0};
    unresolved.bound_radius_q16 = 0x18000; // 1.5 u
    collision.replace_wire_collision_proxies({}, {unresolved});
    bool raised = false;
    try {
        (void)collision.trace_projectile(world, trace);
    } catch (const std::logic_error &) {
        raised = true;
    }
    CHECK(raised);
}

// A decoded vehicle/item shooter never clips its own wire slot: the dyn-proxy
// walk applies the same self-site immunity as the local tables (the retail
// four-slot ray[17..20] exclusion), and an unrelated proxy behind it still
// stops the round.
void test_visual_dynamic_proxy_excludes_shooter_self_slot() {
    World world;
    world.registry.configure_pool(0, 4);
    world.rules.mp_session = true;
    world.rules.projectile_authority = false;

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    const EntityHandle sh = world.registry.spawn(0, shooter);
    world.cached.local_player = sh;

    CollisionWorld collision;
    const int32_t model_id =
        collision.add_model(proxy_face_quad_model(/*material=*/9));
    collision.build_tick_tables(world);
    world.collision = &collision;

    WireDynamicCollisionProxy self;
    self.wire_handle = 0x1004; // the decoded shooter's own pool-1 slot
    self.model_id = model_id;
    self.position_q16 = FixedVec3{6 * 65536, 0, 0};
    self.pitch_bam = 0x40000000; // vertical plane across the lane
    self.bound_radius_q16 = 3 * 65536;
    WireDynamicCollisionProxy behind = self;
    behind.wire_handle = 0x1007;
    behind.position_q16 = FixedVec3{10 * 65536, 0, 0};
    collision.replace_wire_collision_proxies({}, {self, behind});

    ProjectileTrace trace;
    trace.start = FixedVec3{0, 0, 0};
    trace.end = FixedVec3{14 * 65536, 0, 0};
    trace.owner = sh;
    trace.include_wire_proxies = true;
    trace.shooter_wire_handle = 0x1004; // firing AS the decoded vehicle
    const ProjectileHit hit = collision.trace_projectile(world, trace);
    CHECK(hit.hit_class == ProjectileHitClass::DynamicEntity);
    // The shooter's own plane (<= 7) was ridden through; the unrelated proxy
    // stops the round on its vertical plane at X~10.
    CHECK(hit.position_q16.x > 8 * 65536);
    CHECK(hit.position_q16.x < 12 * 65536);
}

// A visual client's decoded remote throwable sweeps the wire dyn proxies like
// the bullet walk: the motor rides through the shooter's own carrier, stops on
// (and reflects off) an unrelated decoded mover instead of flying through it.
void test_visual_throwable_motor_sweeps_wire_proxies() {
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(1, 4);
    world.rules.mp_session = true;
    world.rules.projectile_authority = false;

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    const EntityHandle sh = world.registry.spawn(0, shooter);
    world.cached.local_player = sh;

    CollisionWorld collision;
    const int32_t model_id =
        collision.add_model(proxy_face_quad_model(/*material=*/7));
    collision.build_tick_tables(world);
    world.collision = &collision;

    WireDynamicCollisionProxy carrier;
    carrier.wire_handle = 0x1004; // the decoded shooter's own carrier
    carrier.model_id = model_id;
    carrier.position_q16 = FixedVec3{6 * 65536, 0, 0};
    carrier.pitch_bam = 0x40000000; // vertical plane across the lane
    carrier.bound_radius_q16 = 3 * 65536;
    WireDynamicCollisionProxy target = carrier;
    target.wire_handle = 0x1007;
    target.position_q16 = FixedVec3{10 * 65536, 0, 0};
    collision.replace_wire_collision_proxies({}, {carrier, target});

    AmmoTableEntry nade;
    nade.name = "VISUAL_NADE";
    nade.valid = true;
    nade.flags = 0x2000u | 0x80u; // useownmove + nocollide-terrain
    nade.velocity = 124;          // 2 units/tick
    nade.max_age_ticks = 60;
    nade.drag_fp16 = 65536;       // dragless: keep the 2 u/tick lane speed
    const int nade_index = static_cast<int>(world.tables.ammo.entries.size());
    world.tables.ammo.entries.push_back(nade);

    RoundSpawnParams params;
    params.owner = sh;
    params.shooter_handle = 0x0002;
    params.origin = {0.0f, 0.0f, 0.5f};
    params.ammo_index = nade_index;
    const int slot = world.round_sim.spawn(
        world, params, RoundConsequenceMode::VisualOnly);
    CHECK(slot >= 0);
    if (slot < 0) return;
    LiveRound &round = world.round_sim.rounds[static_cast<size_t>(slot)];
    round.motor = ThrowClass::kNade;
    round.shooter_carrier_handle = 0x1004;
    round.vel = Vec3{2.0f, 0.0f, 0.0f};

    // The pitched quads sit one unit shy of their proxy origins: the carrier
    // plane crosses the lane at X=5, the target plane at X=9.
    float first_bounce_x = -1.0f;
    float speed_before = 2.0f;
    float speed_after = -1.0f;
    for (int tick = 0; tick < 10; ++tick) {
        const float vx_entering =
            world.round_sim.rounds[static_cast<size_t>(slot)].vel.x;
        world.round_sim.tick(world, nullptr, &collision);
        const LiveRound &r = world.round_sim.rounds[static_cast<size_t>(slot)];
        if (first_bounce_x < 0.0f && r.bounce_count > 0) {
            first_bounce_x = r.pos.x;
            speed_before = vx_entering;
            speed_after = r.vel.x;
        }
        if (!r.active) break;
    }
    // The sweep found the wire geometry (a fly-through regression records no
    // bounce at all), and the FIRST contact is the unrelated mover's plane at
    // X=9 — the shooter's own carrier plane at X=5 was ridden through.
    CHECK(first_bounce_x > 8.0f);
    CHECK(first_bounce_x < 9.6f);
    // The contact reflected the motor: retail's 0.35 impact-speed retention
    // [orig: Physics_ComputeReflectionForce @ 0x4e4310].
    CHECK(speed_after >= 0.0f
              ? speed_after < speed_before * 0.4f
              : -speed_after < speed_before * 0.4f);
    CHECK(world.explosions.queue.empty());
}

// A decoded non-player Infantry proxy joins the person walk exactly like the
// remote-player proxy: torso stand-in at the decoded position, consequence-free.
void test_visual_infantry_proxy_joins_person_walk() {
    World world;
    world.registry.configure_pool(0, 8);
    world.rules.mp_session = true;
    world.rules.projectile_authority = false;

    Entity local;
    local.kind = EntityKind::Organic;
    local.item_type = 3;
    const EntityHandle local_l = world.registry.spawn(0, local);
    world.cached.local_player = local_l;

    // A synthetic non-local native AI at its authored spawn on the lane. On a
    // visual client it does not stop rounds; the live decoded pool-0 proxy
    // (the host moved it to X=9) serves instead.
    Entity frozen_ai;
    frozen_ai.kind = EntityKind::Organic;
    frozen_ai.has_item_def = true;
    frozen_ai.item_type = 3;
    frozen_ai.position = {5.0f, 0.0f, -0.9f};
    frozen_ai.health = 100;
    const EntityHandle ai_h = world.registry.spawn(0, frozen_ai);

    CollisionWorld collision;
    collision.build_tick_tables(world);
    world.collision = &collision;

    std::vector<WirePersonCollisionProxy> proxies{
        WirePersonCollisionProxy{ai_h.packed, FixedVec3{9 * 65536, 0, -58982}},
    };
    collision.replace_wire_collision_proxies(proxies, {}, /*self H*/ 0x0002);

    ProjectileTrace trace;
    trace.start = FixedVec3{0, 0, 0};
    trace.end = FixedVec3{14 * 65536, 0, 0};
    trace.owner = local_l;
    trace.shooter_wire_handle = 0x0002;
    trace.include_wire_proxies = true;
    const ProjectileHit hit = collision.trace_projectile(world, trace);
    CHECK(hit.hit_class == ProjectileHitClass::Person);
    CHECK(!hit.geometry_entity.valid());
    // The impact lands at the DECODED torso (X~9), not the frozen spawn (X=5).
    CHECK(hit.position_q16.x > 8 * 65536);
    CHECK(world.registry.get(ai_h)->health == 100);
}

// The merged person walk is ordered by the SUBSTITUTED key, not by the order the
// registry happens to hand out: L sits at a high local slot while carrying a low
// server H, so the local pool order and the wire order disagree. Both cases keep
// L nearer than the proxy, so distance alone cannot explain either victim.
void test_person_walk_orders_local_player_by_its_server_handle() {
    const auto run_case = [](uint16_t local_wire_h) {
        World world;
        world.registry.configure_pool(0, 8);
        world.rules.mp_session = true;
        world.rules.projectile_authority = false;

        // Synthetic non-local native organics at slots 0..3. A visual client
        // never collides them (decoded pool-0 proxies serve instead), but they
        // push L to a local slot whose packed value outruns the proxy handle.
        for (int i = 0; i < 4; ++i) {
            Entity ghost;
            ghost.kind = EntityKind::Organic;
            ghost.has_item_def = true;
            ghost.item_type = 3;
            ghost.position = {100.0f + static_cast<float>(i), 100.0f, -0.9f};
            ghost.health = 100;
            CHECK(world.registry.spawn(0, ghost).packed ==
                  static_cast<uint16_t>(i));
        }

        Entity local;
        local.kind = EntityKind::Organic;
        local.has_item_def = true;
        local.item_type = 3;
        local.position = {4.0f, 0.0f, -0.9f};
        local.health = 100;
        const EntityHandle local_l = world.registry.spawn(0, local);
        CHECK(local_l.packed == 4);
        world.cached.local_player = local_l;

        CollisionWorld collision;
        collision.build_tick_tables(world);
        world.collision = &collision;

        // One decoded remote, FARTHER down the lane than L, holding a handle
        // between L's two test identities.
        std::vector<WirePersonCollisionProxy> proxies{
            WirePersonCollisionProxy{0x0002, FixedVec3{9 * 65536, 0, -58982}},
        };
        collision.replace_wire_collision_proxies(proxies, {}, local_wire_h);

        ProjectileTrace trace;
        trace.start = FixedVec3{0, 0, 0};
        trace.end = FixedVec3{14 * 65536, 0, 0};
        trace.owner = EntityHandle{}; // a decoded remote shooter owns no entity
        trace.shooter_wire_handle = 0x0007;
        trace.include_wire_proxies = true;
        return collision.trace_projectile(world, trace);
    };

    // H=1 sorts L ahead of the proxy: the first qualifying person is L, even
    // though every local slot ahead of L carries a larger packed handle.
    const ProjectileHit low = run_case(0x0001);
    CHECK(low.hit_class == ProjectileHitClass::Person);
    CHECK(low.geometry_entity.valid());
    CHECK(low.geometry_entity.packed == 4);
    CHECK(low.position_q16.x > 3 * 65536);
    CHECK(low.position_q16.x < 4 * 65536);

    // H=5 sorts L behind the proxy: the farther proxy wins, so the walk is
    // ordered by handle rather than by distance.
    const ProjectileHit high = run_case(0x0005);
    CHECK(high.hit_class == ProjectileHitClass::Person);
    CHECK(!high.geometry_entity.valid());
    CHECK(high.position_q16.x > 8 * 65536);
    CHECK(high.position_q16.x < 9 * 65536);
}

void test_signed_armor_equality_and_damage_state_gates() {
    const auto run_case = [](int32_t armor, int32_t damage_state,
                             int32_t expected_damage, int32_t expected_armor) {
        Rig r;
        r.world.tables.ammo.entries[0].arm_age_ticks = 0;
        r.world.tables.ammo.entries[0].penetration_impact = 10;
        Entity *target = r.world.registry.get(r.target);
        target->armor_impact = armor;
        target->damage_state = damage_state;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.impacts.size() == 1);
        CHECK(r.world.round_sim.active_count == 0);
        CHECK(target->armor_impact == expected_armor);
		CHECK(r.world.registry.get(r.shooter)->hud_hit_feedback_serial == 1);
        if (expected_damage == 0) {
            CHECK(target->health == 100);
            CHECK(r.world.round_sim.hits.empty());
        } else {
            CHECK(target->health == 100 - expected_damage);
            CHECK(r.world.round_sim.hits.size() == 1);
            CHECK(r.world.round_sim.hits[0].damage == expected_damage);
        }
    };

    run_case(-1, 0, 0, -1);  // signed sentinel is always immune
    run_case(11, 0, 0, 11);  // penetration 10 is below armor 11
    run_case(10, 0, 25, 10); // equality penetrates (the comparison is strict <)
    run_case(0, 1, 0, 0);    // entity+292 nonzero independently zeros damage
    run_case(65546, 0, 25, 10); // 0x1000A stores as signed-word 10, so equality penetrates
    run_case(65535, 0, 0, -1);  // 0x0FFFF stores as the signed -1 immunity sentinel
}

void test_signed_health_subtraction_wraps_at_entity_word() {
    Rig r;
    Entity *target = r.world.registry.get(r.target);
    target->item_type = 1; // skip person-zone scaling; keep the arithmetic isolated
    target->health = 32760;
    target->health_max = 65535; // normalized at the same target consequence boundary
    target->armor_kz = 65535;

    AmmoTableEntry &ammo = r.world.tables.ammo.entries[0];
    ammo.arm_age_ticks = 0;
    ammo.weight_in_grains = -875;
    ammo.min_damage = -1000;
    ammo.max_damage = 0;

    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(r.world.round_sim.hits.size() == 1);
    CHECK(r.world.round_sim.hits[0].damage == -620);
    // 32760 - (-620) = 33380 -> low word 0x8264 -> signed -32156.
    CHECK(target->health == -32156);
    CHECK(target->health_max == -1 && target->armor_kz == -1);
    CHECK(r.world.round_sim.deaths.size() == 1);
}

// The person impact-effect leg. Bullets reach a person only through the
// bone-section pass, whose handler splits the effect tag on victim identity and
// then suppresses the non-local row for a healthy squad-mate.
// [orig: Projectile_HandleTerrainImpact_0 @ 0x4e98f0 — dead gate
//  @0x4e9920/@0x4e994f, local-player compare @0x4e9a55, push 2 @0x4e9aa1,
//  group WORDs @0x4e9aac/@0x4e9ab3, healthMax `sar dx,1` @0x4e9abf..@0x4e9ac6,
//  signed Health compare + `jge` skip @0x4e9ac9/@0x4e9ad0, push 17h @0x4e9ad7]
void test_person_impact_tag_splits_on_identity_and_squad_health() {
    // 1. The LOCAL player takes tag 2 'player'.
    {
        Rig r;
        r.world.cached.local_player = r.target;
        // World::tick stamps round_sim.local_player from cached each frame
        // (world.cpp); these cases drive RoundSim::tick directly, so stamp it.
        r.world.round_sim.local_player = r.target;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.impacts.size() == 1);
        CHECK(r.world.round_sim.impacts[0].effect_tag == 2);
        CHECK(r.world.round_sim.impacts[0].present_effect);
    }

    // 2. A non-local person in a DIFFERENT group takes tag 23 'flesh', at any
    //    health — the group mismatch alone satisfies the gate.
    {
        Rig r;
        Entity local;
        local.kind = EntityKind::Organic;
        local.item_type = 3;
        local.position = {0.0f, 50.0f, 0.0f};
        local.health = 100;
        local.group_id = 1;
        const EntityHandle local_h = r.world.registry.spawn(0, local);
        r.world.cached.local_player = local_h;
        r.world.round_sim.local_player = local_h;
        r.world.registry.get(r.target)->group_id = 2;
        r.world.registry.get(r.target)->health_max = 100;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.impacts.size() == 1);
        CHECK(r.world.round_sim.impacts[0].effect_tag == 23);
        CHECK(r.world.round_sim.impacts[0].present_effect);
    }

    // 3. SAME group and at or above half hp: the row is selected but NOT
    //    presented. This is retail's squad declutter — a healthy team-mate shows
    //    no impact effect at all.
    {
        Rig r;
        Entity local;
        local.kind = EntityKind::Organic;
        local.item_type = 3;
        local.position = {0.0f, 50.0f, 0.0f};
        local.health = 100;
        local.group_id = 4;
        const EntityHandle local_h = r.world.registry.spawn(0, local);
        r.world.cached.local_player = local_h;
        r.world.round_sim.local_player = local_h;
        Entity *t = r.world.registry.get(r.target);
        t->group_id = 4;
        t->health = 60;
        t->health_max = 100; // 60 >= 100>>1 -> suppressed
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.impacts.size() == 1);
        CHECK(r.world.round_sim.impacts[0].effect_tag == 23);
        CHECK(!r.world.round_sim.impacts[0].present_effect);
        CHECK(!r.world.round_sim.impacts[0].present_sound);
    }

    // 4. Same group but BELOW half hp: the gate's OR arm reopens it.
    {
        Rig r;
        Entity local;
        local.kind = EntityKind::Organic;
        local.item_type = 3;
        local.position = {0.0f, 50.0f, 0.0f};
        local.health = 100;
        local.group_id = 4;
        const EntityHandle local_h = r.world.registry.spawn(0, local);
        r.world.cached.local_player = local_h;
        r.world.round_sim.local_player = local_h;
        Entity *t = r.world.registry.get(r.target);
        t->group_id = 4;
        t->health = 40;
        t->health_max = 100; // 40 < 100>>1 -> presented
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.impacts.size() == 1);
        CHECK(r.world.round_sim.impacts[0].effect_tag == 23);
        CHECK(r.world.round_sim.impacts[0].present_effect);
    }

    // 5. An already-dead victim presents NOTHING — neither leg, on either row.
    {
        Rig r;
        r.world.cached.local_player = r.target;
        // World::tick stamps round_sim.local_player from cached each frame
        // (world.cpp); these cases drive RoundSim::tick directly, so stamp it.
        r.world.round_sim.local_player = r.target;
        r.world.registry.get(r.target)->flags |= kEntityFlagDead;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.impacts.size() == 1);
        CHECK(!r.world.round_sim.impacts[0].present_effect);
        CHECK(!r.world.round_sim.impacts[0].present_sound);
    }
}

void test_exact_one_hop_vehicle_parent_damage_routing() {
    const auto run_direct_case = [](uint8_t child_type, uint32_t child_attrib,
                                    uint8_t parent_type, bool expect_parent) {
        Rig r;
        r.world.tables.ammo.entries[0].arm_age_ticks = 0;
        Entity *child = r.world.registry.get(r.target);
        child->item_type = child_type;
        child->item_attrib = child_attrib;

        Entity parent;
        parent.kind = EntityKind::Item;
        parent.has_item_def = true;
        parent.item_type = parent_type;
        parent.position = {100.0f, 100.0f, 100.0f};
        parent.health = 100;
        const EntityHandle parent_h = r.world.registry.spawn(0, parent);
        child = r.world.registry.get(r.target);
        child->ground_target = parent_h;

        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.impacts.size() == 1);
        if (expect_parent) {
            CHECK(r.world.registry.get(r.target)->health == 100);
            CHECK(r.world.registry.get(parent_h)->health == 75);
            CHECK(r.world.round_sim.hits[0].victim == parent_h);
            // Presentation still classifies the child geometry actually struck,
            // not the vehicle that receives the routed damage: the gunner is a
            // non-local person, so the flesh row, not the vehicle's material.
            CHECK(r.world.round_sim.impacts[0].effect_tag == 23);
        } else {
            CHECK(r.world.registry.get(r.target)->health == 75);
            CHECK(r.world.registry.get(parent_h)->health == 100);
            CHECK(r.world.round_sim.hits[0].victim == r.target);
        }
    };

    run_direct_case(3, 0x20u, 1, true);
    run_direct_case(3, 0u, 1, false);     // missing child-link attribute
    run_direct_case(1, 0x20u, 1, false);  // a vehicle child never rolls upward
    run_direct_case(3, 0x20u, 5, false);  // immediate parent must be a vehicle

    // The resolver does not search ancestors: child -> nonvehicle -> vehicle
    // still damages the child.
    Rig r;
    r.world.tables.ammo.entries[0].arm_age_ticks = 0;
    Entity vehicle;
    vehicle.kind = EntityKind::Item;
    vehicle.has_item_def = true;
    vehicle.item_type = 1;
    vehicle.position = {100.0f, 100.0f, 100.0f};
    vehicle.health = 100;
    const EntityHandle vehicle_h = r.world.registry.spawn(0, vehicle);
    Entity intermediate;
    intermediate.kind = EntityKind::Item;
    intermediate.item_type = 5;
    intermediate.position = {100.0f, 100.0f, 100.0f};
    intermediate.health = 100;
    intermediate.ground_target = vehicle_h;
    const EntityHandle intermediate_h = r.world.registry.spawn(0, intermediate);
    Entity *child = r.world.registry.get(r.target);
    child->has_item_def = true;
    child->item_type = 3;
    child->item_attrib = 0x20u;
    child->ground_target = intermediate_h;
    CHECK(r.fire() >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(r.world.round_sim.hits.size() == 1);
    CHECK(r.world.round_sim.hits[0].victim == r.target);
    CHECK(r.world.registry.get(r.target)->health == 75);
    CHECK(r.world.registry.get(vehicle_h)->health == 100);
}

void test_vehicle_occupant_reduction_count_cap_and_depth() {
    Rig r;
    r.world.registry.configure_pool(1, 2);
    r.world.tables.ammo.entries[0].arm_age_ticks = 0;
    r.world.tables.ammo.entries[0].max_damage = 0;
    r.world.tables.ammo.entries[0].weight_in_grains = 143; // floor(620*143/875) = 101

    Entity vehicle;
    vehicle.kind = EntityKind::Item;
    vehicle.item_type = 1;
    vehicle.position = {100.0f, 100.0f, 100.0f};
    vehicle.health = 5000;
    vehicle.has_item_def = true;
    vehicle.damage_reduc_pp = 0.10f;
    vehicle.damage_reduc_max = 0.25f;
    const EntityHandle vehicle_h = r.world.registry.spawn(0, vehicle);

    Entity *child = r.world.registry.get(r.target);
    child->item_type = 3;
    child->item_attrib = 0x20u;
    child->ground_target = vehicle_h; // the one-hop damage rollup channel
    child->mounted = true;
    child->mount_target = vehicle_h;
    child->has_item_def = true;

    const auto fire_expect = [&](int32_t expected_damage) {
        r.clear_events();
        r.world.registry.get(vehicle_h)->health = 5000;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].victim == vehicle_h);
        CHECK(r.world.round_sim.hits[0].damage == expected_damage);
        CHECK(r.world.registry.get(vehicle_h)->health == 5000 - expected_damage);
        CHECK(r.world.registry.get(r.target)->health == 100);
        CHECK(r.world.round_sim.impacts.size() == 1);
        // The struck geometry is the non-local occupant, so the flesh row.
        CHECK(r.world.round_sim.impacts[0].effect_tag == 23);
    };
    // [orig: Entity_CountMountedEntities @ 0x435970] Occupants are counted by
    // the groundEntity link alone (+0x28, both hops @0x4359B4..0x4359C2): the
    // candidate's own link is the vehicle, or its link's link is. A seated
    // rider carries its parent there because the organic movers refresh it
    // every update [orig: Entity_UpdateInfantryPlayerBody @0x4B41A2..0x4B41B4],
    // which these riders mirror.
    const auto add_rider = [&](EntityHandle mount, uint32_t flags = 0,
                               int pool = 0) {
        Entity rider;
        rider.kind = EntityKind::Organic;
        rider.has_item_def = true;
        rider.item_type = 3;
        rider.position = {100.0f, 100.0f, 100.0f};
        rider.health = 100;
        rider.flags = flags;
        rider.mounted = true;
        rider.mount_target = mount;
        rider.ground_target = mount;
        return r.world.registry.spawn(pool, rider);
    };

    fire_expect(101); // the struck child is the sole direct occupant: no scale

    add_rider(vehicle_h);
    fire_expect(81); // count 2: trunc(101 * 0.20) = 20 reduction

    const EntityHandle nested = add_rider(r.target);
    fire_expect(76); // count 3: 0.30 capped to 0.25; trunc(25.25) = 25

    const EntityHandle flagged = add_rider(vehicle_h, 0x2u);
    CHECK(flagged.valid());
    fire_expect(76); // Flags&2 occupants are excluded

    CHECK(add_rider(vehicle_h, 0, 1).valid());
    fire_expect(76); // the retail occupant scan is pool-0-only

    Entity no_item_def;
    no_item_def.kind = EntityKind::Organic;
    no_item_def.item_type = 3;
    no_item_def.position = {100.0f, 100.0f, 100.0f};
    no_item_def.health = 100;
    no_item_def.mounted = true;
    no_item_def.mount_target = vehicle_h;
    CHECK(r.world.registry.spawn(0, no_item_def).valid());
    fire_expect(76); // a pool-0 slot without an ItemDef pointer is also excluded

    // Lift the cap so the count itself is observable: 3 x 0.10 -> 30.
    r.world.registry.get(vehicle_h)->damage_reduc_max = 0.5f;
    fire_expect(71);

    Entity deck_stander;
    deck_stander.kind = EntityKind::Organic;
    deck_stander.has_item_def = true;
    deck_stander.item_type = 3;
    deck_stander.position = {100.0f, 100.0f, 100.0f};
    deck_stander.health = 100;
    deck_stander.ground_target = vehicle_h; // standing on the deck, not seated
    const EntityHandle deck = r.world.registry.spawn(0, deck_stander);
    CHECK(deck.valid());
    fire_expect(61); // a deck stander counts: 4 x 0.10 -> trunc(40.4) = 40

    // A seat parent alone, without the groundEntity link, is not what the
    // scan reads (the pool is full: reuse the deck stander's row).
    Entity *seated = r.world.registry.get(deck);
    seated->ground_target = EntityHandle{};
    seated->mounted = true;
    seated->mount_target = vehicle_h;
    fire_expect(71);

    // Two intermediates deep is outside the retail count.
    Entity *deep = r.world.registry.get(flagged);
    deep->flags = 0;
    deep->mount_target = nested;
    deep->ground_target = nested;
    fire_expect(71);
}

void test_retail_force_order_and_stock_gates() {
    // The first sweep uses the old velocity. Gravity becomes visible only in
    // the velocity for the following tick.
    FlightResult gravity = fly_one_tick(FixedVec3{65536, 0, 0});
    CHECK(gravity.active);
    CHECK(gravity.pos.x == 65536 && gravity.pos.z == 10 * 65536);
    CHECK(gravity.vel.x == 65536 && gravity.vel.z == -167);

    FlightResult no_gravity = fly_one_tick(FixedVec3{65536, 0, 0}, 0x100u);
    CHECK(no_gravity.pos.x == 65536 && no_gravity.pos.z == 10 * 65536);
    CHECK(no_gravity.vel.z == 0);

    // Exact zero takes the dedicated pre-ray leaf: no position change and one
    // gravity step even when the authored ammo says NoGravity.
    FlightResult stopped = fly_one_tick(FixedVec3{}, 0x100u);
    CHECK(stopped.active && stopped.pos.x == 0 && stopped.pos.z == 10 * 65536);
    CHECK(stopped.vel.x == 0 && stopped.vel.y == 0 && stopped.vel.z == -167);

    // Both flags bypass the entire stock ballistic callback, but still age.
    FlightResult ignored = fly_one_tick(FixedVec3{65536, 0, 0}, 0x2u);
    CHECK(ignored.active && ignored.age_ticks == 1 && ignored.pos.x == 0);
    CHECK(ignored.vel.x == 65536 && ignored.vel.z == 0);
    FlightResult own_move = fly_one_tick(FixedVec3{65536, 0, 0}, 0x2000u);
    CHECK(own_move.active && own_move.age_ticks == 1 && own_move.pos.x == 0);
    CHECK(own_move.vel.x == 65536 && own_move.vel.z == 0);
}

void test_retail_aerodynamic_drag_vectors() {
    // Recovered dry-air reference: speed bin 100 contains 1596; the two IDIV
    // truncations turn that into a 25-Q16 step for drag=1.0.
    FlightResult dry = fly_one_tick(FixedVec3{105704, 0, 0}, 0x100u, 65536);
    CHECK(dry.pos.x == 105704);
    CHECK(dry.vel.x == 105679 && dry.vel.y == 0 && dry.vel.z == 0);

    // At/below water the step is multiplied by 25 before the component Q16
    // multiply. The old-position stall gate does not fire above 0.25u/tick.
    FlightResult wet = fly_one_tick(FixedVec3{105704, 0, 0}, 0x100u, 65536, 0,
                                    4 * 65536, 5 * 65536);
    CHECK(wet.active && wet.vel.x == 105079);
    FlightResult slow_wet = fly_one_tick(FixedVec3{0x3fff, 0, 0}, 0x100u, 0, 0,
                                         4 * 65536, 5 * 65536);
    CHECK(!slow_wet.active);
    // The stall zeroes the lifetime BEFORE the sweep and the round still flies
    // its final segment [orig: no early return @0x4ea142-0x4ea148].
    CHECK(slow_wet.pos.x == 0x3fff);
    FlightResult edge_wet = fly_one_tick(FixedVec3{0x4000, 0, 0}, 0x100u, 0, 0,
                                         4 * 65536, 5 * 65536);
    CHECK(edge_wet.active && edge_wet.pos.x == 0x4000);

    // Drag zero returns before the stability branch. With drag live, the exact
    // below-stable damping follows drag, including retail's NoGravity Z step.
    FlightResult no_drag_stability =
        fly_one_tick(FixedVec3{105704, 0, 0}, 0x100u, 0, 200);
    CHECK(no_drag_stability.vel.x == 105704 && no_drag_stability.vel.z == 0);
    FlightResult stable =
        fly_one_tick(FixedVec3{105704, 0, 0}, 0x100u, 65536, 101);
    CHECK(stable.vel.x == 102377 && stable.vel.z == -167);

    // Source speed 3999 only writes bin 1218. Bin 1219 remains BSS zero in
    // retail, so a capped/high-speed round receives no aerodynamic decrement.
    const int32_t speed_1218 = static_cast<int32_t>(((int64_t{1218} << 16) + 61) / 62);
    const int32_t speed_1219 = static_cast<int32_t>(((int64_t{1219} << 16) + 61) / 62);
    FlightResult bin_1218 =
        fly_one_tick(FixedVec3{speed_1218, 0, 0}, 0x100u, 65536);
    FlightResult bin_1219 =
        fly_one_tick(FixedVec3{speed_1219, 0, 0}, 0x100u, 65536);
    CHECK(bin_1218.vel.x == speed_1218 - 8567);
    CHECK(bin_1219.vel.x == speed_1219);
}

void test_consumed_hit_skips_post_sweep_forces() {
    Rig r;
    r.world.tables.ammo.entries[0].arm_age_ticks = 0;
    const int slot = r.fire();
    CHECK(slot >= 0);
    if (slot < 0) return;
    r.world.round_sim.tick(r.world, nullptr);
    const LiveRound &round = r.world.round_sim.rounds[static_cast<size_t>(slot)];
    CHECK(!round.active);
    CHECK(to_fixed(round.vel.z) == 0); // no post-hit gravity or drag
}

} // namespace

// Terrain impact rows sample the charmap surface at the impact point and shift
// it into the effects table: no charmap -> sampler default 1 -> tag 5 dirt, a
// mapped cell reports its authored type + 4, an unmapped sector -> ocean 7 ->
// tag 11 water. [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510 result + 4
// in the terrain leg of the @ 0x4ea6a7 hit switch]
void test_terrain_impact_samples_charmap_surface() {
    static const uint8_t raster[4] = {6, 6, 6, 6}; // charmap type 6 = grass
    static const int mapped_grid[256] = {1};       // cell (0,0) -> quadrant 0
    static const int unmapped_grid[256] = {0};     // every cell unmapped

    struct Variant {
        const uint8_t *data;
        const int *grid;
        int expected_tag;
    };
    const Variant variants[] = {
        {nullptr, nullptr, 5},           // no charmap: default 1 -> dirt
        {raster, mapped_grid, 10},       // authored type 6 -> grass
        {raster, unmapped_grid, 11},     // unmapped sector: 7 -> water
    };
    for (const Variant &v : variants) {
        World world;
        world.registry.configure_pool(0, 4);
        Entity s;
        s.kind = EntityKind::Organic;
        s.item_type = 3;
        s.position = {0.0f, 0.0f, 10.0f};
        const EntityHandle shooter = world.registry.spawn(0, s);

        AmmoTableEntry ammo;
        ammo.name = "TRN";
        ammo.valid = true;
        ammo.velocity = 620;
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        world.tables.ammo.entries.push_back(ammo);

        std::vector<uint16_t> heights(512u * 512u, 0);
        std::vector<int> sectors(256u, 1);
        opennova::terrain::TerrainHeightField flat;
        flat.heightmap = heights.data();
        flat.dim = 512;
        flat.layout.sector_grid = sectors.data();
        CollisionWorld cw;
        cw.terrain = &flat;
        cw.build_tick_tables(world);
        world.collision = &cw;

        world.tables.surface_map.data = v.data;
        world.tables.surface_map.width = v.data != nullptr ? 2 : 0;
        world.tables.surface_map.height = v.data != nullptr ? 2 : 0;
        world.tables.surface_map.sector_grid = v.grid;

        RoundSpawnParams p;
        p.owner = shooter;
        p.shooter_handle = shooter.packed;
        p.origin = {10.0f, -10.0f, 2.0f};
        p.ammo_index = 0;
        const int slot =
            world.round_sim.spawn(world, p, RoundConsequenceMode::Authoritative);
        CHECK(slot >= 0);
        if (slot < 0) continue;
        world.round_sim.rounds[static_cast<size_t>(slot)].vel =
            Vec3{0.0f, 0.0f, -10.0f};
        world.round_sim.tick(world, &flat, &cw);
        CHECK(world.round_sim.impacts.size() == 1);
        if (!world.round_sim.impacts.empty()) {
            CHECK(world.round_sim.impacts[0].effect_tag == v.expected_tag);
            // A terrain stop's descriptor carries NO orientation: the handler
            // hands the spawner a NULL record, so EMITVECTOR members emit around
            // world +Y, not along the round [orig: Projectile_HandleTerrainImpact
            // @0x4e92c8 -> CEffectWorld_SpawnEmitterAtPosition @0x5f6e52].
            CHECK(world.round_sim.impacts[0].direction.x == 0.0f);
            CHECK(world.round_sim.impacts[0].direction.y == 0.0f);
            CHECK(world.round_sim.impacts[0].direction.z == 0.0f);
        }
    }
}

// A physical terrain stop resolves ammo word +0x74 into the permanent terrain
// cache registry before presenting the ordinary impact. The record remains in
// mission x/y here; the Simulation drain owns the one Godot x/-z fold.
// [orig: Projectile_HandleTerrainImpact @0x4E9314 -> Terrain_AddScorchForEffectKind @0x6060d0]
void test_terrain_impact_emits_permanent_scorch() {
    crt_srand(1);
    World world;
    world.registry.configure_pool(0, 4);
    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    shooter.position = {0.0f, 0.0f, 10.0f};
    const EntityHandle owner = world.registry.spawn(0, shooter);

    AmmoTableEntry ammo;
    ammo.name = "SCORCH";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    ammo.scorch_id = 2;
    world.tables.ammo.entries.push_back(ammo);

    std::vector<uint16_t> heights(512u * 512u, 0);
    std::vector<int> sectors(256u, 1);
    opennova::terrain::TerrainHeightField flat;
    flat.heightmap = heights.data();
    flat.dim = 512;
    flat.layout.sector_grid = sectors.data();
    CollisionWorld collision;
    collision.terrain = &flat;
    collision.build_tick_tables(world);
    world.collision = &collision;

    RoundSpawnParams params;
    params.owner = owner;
    params.shooter_handle = owner.packed;
    params.origin = {10.0f, -20.0f, 2.0f};
    params.ammo_index = 0;
    const int slot = world.round_sim.spawn(world, params);
    CHECK(slot >= 0);
    if (slot < 0) return;
    world.round_sim.rounds[static_cast<size_t>(slot)].vel =
        Vec3{0.0f, 0.0f, -10.0f};
    world.round_sim.tick(world, &flat, &collision);

    CHECK(world.out.terrain_scorches.pending().size() == 1);
    if (world.out.terrain_scorches.pending().empty()) return;
    const TerrainScorchEvent &event = world.out.terrain_scorches.pending()[0];
    CHECK(event.mission_bounds.texture_index == 2); // first CRT rand = 41
    CHECK(event.mission_bounds.minimum_x_q16 == 6 * 65536);
    CHECK(event.mission_bounds.maximum_x_q16 == 14 * 65536);
    CHECK(event.mission_bounds.minimum_z_q16 == -24 * 65536);
    CHECK(event.mission_bounds.maximum_z_q16 == -16 * 65536);
    CHECK(event.tick == world.logic_tick);

    world.out.terrain_scorches.reset();
    ammo.scorch_id = 0;
    // No second active round remains, so an inert route can be pinned directly
    // without manufacturing a renderer record.
    CHECK(!world.out.terrain_scorches.emit_standard(0, 0, ammo.scorch_id, 1));
    CHECK(world.out.terrain_scorches.pending().empty());
}

// A terrain stop records its round in the global hit record with no damage,
// section or target. [orig: Projectile_HandleTerrainImpact
// Projectile_CopyEntityToHitRecord @0x4E9319..0x4E931F, +0x30 / +0x48 zeroed
// @0x4E9326 / @0x4E932B]
void test_terrain_stop_records_the_round() {
    World world;
    world.registry.configure_pool(0, 4);
    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    shooter.position = {0.0f, 0.0f, 10.0f};
    const EntityHandle owner = world.registry.spawn(0, shooter);
    AmmoTableEntry ammo;
    ammo.name = "RECORD";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    world.tables.ammo.entries.push_back(ammo);
    std::vector<uint16_t> heights(512u * 512u, 0);
    std::vector<int> sectors(256u, 1);
    opennova::terrain::TerrainHeightField flat;
    flat.heightmap = heights.data();
    flat.dim = 512;
    flat.layout.sector_grid = sectors.data();
    CollisionWorld collision;
    collision.terrain = &flat;
    collision.build_tick_tables(world);
    world.collision = &collision;
    world.round_sim.hit_record.damage = 99;
    world.round_sim.hit_record.section = 5;
    RoundSpawnParams params;
    params.owner = owner;
    params.shooter_handle = owner.packed;
    params.origin = {10.0f, -20.0f, 2.0f};
    params.ammo_index = 0;
    const int slot = world.round_sim.spawn(world, params);
    CHECK(slot >= 0);
    if (slot < 0) return;
    world.round_sim.rounds[static_cast<size_t>(slot)].vel = Vec3{0.0f, 0.0f, -10.0f};
    world.round_sim.tick(world, &flat, &collision);
    const HitRecord &record = world.round_sim.hit_record;
    CHECK(record.has_round);
    CHECK(record.round_owner == owner && record.owner == owner);
    CHECK(record.round_ammo_index == 0);
    CHECK(record.round_vel_q16.z < 0);
    CHECK(record.damage == 0 && record.section == 0 && !record.target.valid());
}

// The ordinary BULLET path never remaps a building's CFAC material 1 to the
// flesh bank: retail's ballistic entity impact passes material + 4
// unconditionally; the 1 -> 23 remap belongs to the knife's PERSON leg alone.
// [orig: Projectile_HandleEntityImpact passes ray[22] + 4 @0x4e982b;
//  AmmoDef_ProcessImpactEffect @0x40a170 clamps only >= 28 to 4 @0x40a1bf;
//  the remap lives in Weapon_RaycastAndSpawnImpact case 3 @0x4e8880..0x4e8888]
void test_bullet_building_material_is_plain_plus_four() {
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(2, 4);

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.has_item_def = true;
    shooter.item_type = 3;
    shooter.position = {0.0f, 0.0f, 0.0f};
    const EntityHandle owner = world.registry.spawn(0, shooter);

    Entity building;
    building.kind = EntityKind::Building;
    building.has_item_def = true;
    building.item_type = 3;
    building.position = {4.0f, 0.0f, 0.0f};
    building.health = 1000;
    const EntityHandle bh = world.registry.spawn(2, building);

    CollisionWorld collision;
    const int32_t model_id = collision.add_model(
        knife_person_face_model(/*material=*/1));
    collision.assign_entity(bh, model_id);
    const int32_t pose[3] = {4 * 65536, 0, 0};
    CHECK(collision.publish_entity_section_matrices(
        bh, {collision_matrix_from_heading(0, pose)}));
    collision.build_tick_tables(world);
    world.collision = &collision;

    AmmoTableEntry ammo;
    ammo.name = "BULLET";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    ammo.weight_in_grains = 875;
    ammo.max_damage = 25;
    world.tables.ammo.entries.push_back(ammo);

    RoundSpawnParams params;
    params.owner = owner;
    params.shooter_handle = owner.packed;
    params.origin = {0.0f, 0.0f, 1.0f};
    params.ammo_index = 0;
    CHECK(world.round_sim.spawn(
              world, params, RoundConsequenceMode::Authoritative) >= 0);
    world.round_sim.tick(world, nullptr, &collision);

    CHECK(world.round_sim.impacts.size() == 1);
    if (!world.round_sim.impacts.empty())
        CHECK(world.round_sim.impacts[0].effect_tag == 5); // material 1 + 4
}

// A two-section CFAC pane sharing one face/vertex run. Section 1 is the one the
// caller places on the ray, so the section bit the break sets is 1 << 1 rather
// than the section-0 default every single-section fixture would produce.
CollisionModel building_pane_face_model(uint8_t material) {
    CollisionModel m = knife_person_face_model(material);
    m.sections.push_back(m.sections[0]);
    return m;
}

// Gunfire through a BUILDING's glass breaks the struck section: face material
// 15 on a live item-type-5 victim sets 1 << ray[31] in the victim's section
// mask and plays GLASS_SMASH at the hit point. Any other face material, a
// non-building item type, or an already-husked victim leaves the mask clear.
// The pane still stops the round either way: only the lawr/fgrenade
// pass-through report would let it continue, and that report is unported.
// [orig: Projectile_HandleEntityImpact @ 0x4E9390 — `cmp ecx, 0Fh` @0x4e964f,
//  `test byte ptr [esi+24h], 4` @0x4e9654, `cmp dword ptr [ecx+5Ch], 5`
//  @0x4e965d, `or [esi+134h], edx` @0x4e9684; the sound through
//  Entity_PlaySectionBreakSound @ 0x439C00 @0x439d25, whose bank
//  dword_24E0920 is the `GLASS_SMASH` resolver row @0x82F9A4]
void test_material_15_breaks_the_building_glass_section() {
    struct Case {
        uint8_t material;
        int32_t item_type;
        uint32_t engine_flags;
        uint32_t expect_mask;
        bool expect_sound;
    };
    const Case cases[] = {
        {15, 5, 0u, 1u << 1, true},           // glass on a live building
        {14, 5, 0u, 0u, false},               // a metal pane never breaks
        {15, 2, 0u, 0u, false},               // an ordinary item is not a building
        {15, 5, kEntityFlagHusk, 0u, false},  // the husk stage is already broken
    };
    for (const Case &c : cases) {
        HeapWorldFixture fixture;
        World &world = fixture.world;
        world.registry.configure_pool(0, 4);
        world.registry.configure_pool(2, 4);

        Entity shooter;
        shooter.kind = EntityKind::Organic;
        shooter.has_item_def = true;
        shooter.item_type = 3;
        shooter.position = {0.0f, 0.0f, 0.0f};
        const EntityHandle owner = world.registry.spawn(0, shooter);

        Entity building;
        building.kind = EntityKind::Building;
        building.has_item_def = true;
        building.item_type = c.item_type;
        building.engine_flags = c.engine_flags;
        building.position = {4.0f, 0.0f, 0.0f};
        building.health = 1000;
        const EntityHandle bh = world.registry.spawn(2, building);

        CollisionWorld collision;
        const int32_t model_id =
            collision.add_model(building_pane_face_model(c.material));
        collision.assign_entity(bh, model_id);
        const int32_t off_ray[3] = {4 * 65536, 100 * 65536, 0};
        const int32_t on_ray[3] = {4 * 65536, 0, 0};
        CHECK(collision.publish_entity_section_matrices(
            bh, {collision_matrix_from_heading(0, off_ray),
                 collision_matrix_from_heading(0, on_ray)}));
        collision.build_tick_tables(world);
        world.collision = &collision;
        world.out.fire_sounds.set_listener({0.0f, 0.0f, 1.0f});

        AmmoTableEntry ammo;
        ammo.name = "BULLET";
        ammo.valid = true;
        ammo.velocity = 620;
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        ammo.max_damage = 25;
        world.tables.ammo.entries.push_back(ammo);

        RoundSpawnParams params;
        params.owner = owner;
        params.shooter_handle = owner.packed;
        params.origin = {0.0f, 0.0f, 1.0f};
        params.ammo_index = 0;
        CHECK(world.round_sim.spawn(
                  world, params, RoundConsequenceMode::Authoritative) >= 0);
        world.round_sim.tick(world, nullptr, &collision);

        CHECK(world.round_sim.impacts.size() == 1);
        const Entity *struck = world.registry.get(bh);
        CHECK(struck != nullptr);
        if (struck != nullptr) CHECK(struck->section_mask == c.expect_mask);
        bool smashed = false;
        for (const ReadyFireSound &sound : world.out.fire_sounds.drain())
            if (sound.set_name == "GLASS_SMASH") smashed = true;
        CHECK(smashed == c.expect_sound);
    }

    // The break is idempotent: a second round through the same broken pane
    // re-ORs the bit it already owns and plays nothing, because the helper's
    // gate reads the mask BEFORE the OR.
    // [orig: `test [ebx+134h], eax` @0x439c1e]
    HeapWorldFixture fixture;
    World &world = fixture.world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(2, 4);

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.has_item_def = true;
    shooter.item_type = 3;
    const EntityHandle owner = world.registry.spawn(0, shooter);

    Entity building;
    building.kind = EntityKind::Building;
    building.has_item_def = true;
    building.item_type = 5;
    building.position = {4.0f, 0.0f, 0.0f};
    building.health = 1000;
    building.section_mask = 1u << 1; // already smashed
    const EntityHandle bh = world.registry.spawn(2, building);

    CollisionWorld collision;
    const int32_t model_id = collision.add_model(building_pane_face_model(15));
    collision.assign_entity(bh, model_id);
    const int32_t off_ray[3] = {4 * 65536, 100 * 65536, 0};
    const int32_t on_ray[3] = {4 * 65536, 0, 0};
    CHECK(collision.publish_entity_section_matrices(
        bh, {collision_matrix_from_heading(0, off_ray),
             collision_matrix_from_heading(0, on_ray)}));
    collision.build_tick_tables(world);
    world.collision = &collision;
    world.out.fire_sounds.set_listener({0.0f, 0.0f, 1.0f});

    AmmoTableEntry ammo;
    ammo.name = "BULLET";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    ammo.weight_in_grains = 875;
    ammo.max_damage = 25;
    world.tables.ammo.entries.push_back(ammo);

    RoundSpawnParams params;
    params.owner = owner;
    params.shooter_handle = owner.packed;
    params.origin = {0.0f, 0.0f, 1.0f};
    params.ammo_index = 0;
    CHECK(world.round_sim.spawn(
              world, params, RoundConsequenceMode::Authoritative) >= 0);
    world.round_sim.tick(world, nullptr, &collision);

    const Entity *struck = world.registry.get(bh);
    CHECK(struck != nullptr);
    if (struck != nullptr) CHECK(struck->section_mask == (1u << 1));
    bool smashed = false;
    for (const ReadyFireSound &sound : world.out.fire_sounds.drain())
        if (sound.set_name == "GLASS_SMASH") smashed = true;
    CHECK(!smashed);
}

// The in-flight `move` emitter's liveness through the round simulation, the
// `useownmove` leg: the first flight tick arms it, the ClipWaterFx release
// fires on the tick whose PRE-move z sits at/below the plane (the tests
// precede the function's only position store), and a round back above water
// re-arms. A useownmove round with no bound motor drifts unmoved, so the
// round is repositioned between ticks to put its pre-move z on either side
// of the plane.
// [orig: Projectile_UpdatePhysics @0x4E9D70 — the spawn test @0x4e9f58..0x4e9f8e,
//  the handle branch @0x4ea019..0x4ea03e, the z store @0x4eaa45]
static void test_move_effect_water_release_reads_pre_move_z() {
    Rig rig;
    rig.world.env.water_z = 0x8000; // water at 0.5
    AmmoTableEntry &ammo = rig.world.tables.ammo.entries[0];
    ammo.impact_effects[1].effect = "Effect_Smoke";
    ammo.flags |= kAmmoFlagClipWaterFx | kAmmoFlagUseOwnMove;
    ammo.max_age_ticks = 200;
    RoundSpawnParams p;
    p.owner = rig.shooter;
    p.shooter_handle = rig.shooter.packed;
    p.origin = {0.0f, 0.0f, -5.0f}; // starts under the plane
    p.dir_yaw_bam = 0;
    p.dir_pitch_bam = 0;
    p.ammo_index = 0;
    const int idx = rig.world.round_sim.spawn(rig.world, p, RoundConsequenceMode::Authoritative);
    CHECK(idx >= 0);
    if (idx < 0) return;
    LiveRound &r = rig.world.round_sim.rounds[static_cast<size_t>(idx)];
    CHECK(!r.move_effect_live); // the spawn does not arm; the ticks do
    rig.world.round_sim.tick(rig.world, nullptr);
    CHECK(r.active);
    CHECK(!r.move_effect_live); // pre-move z -5 <= 0.5 under ClipWaterFx: no spawn
    // Above the plane the emitter arms.
    r.pos = {20.0f, 0.0f, 5.0f};
    rig.world.round_sim.tick(rig.world, nullptr);
    CHECK(r.active);
    CHECK(r.move_effect_live); // pre-move z 5 > 0.5: spawned
    // Under it again: this tick's PRE-move z releases it (un-latched).
    r.pos = {40.0f, 0.0f, -5.0f};
    rig.world.round_sim.tick(rig.world, nullptr);
    CHECK(r.active);
    CHECK(!r.move_effect_live);
    // Without the flag the plane is ignored entirely: it re-arms under water.
    ammo.flags &= ~kAmmoFlagClipWaterFx;
    r.pos = {60.0f, 0.0f, -5.0f};
    rig.world.round_sim.tick(rig.world, nullptr);
    CHECK(r.active);
    CHECK(r.move_effect_live);
}

// The BALLISTIC leg has no water term at all: a ballistic round's plume arms
// on its first tick even under the plane, ClipWaterFx or not, and the plane
// never releases it.
// [orig: the ballistic spawn test @0x4ea8ae..0x4ea8d3 — effect, no handle,
//  life — and the re-pose-only handle branch @0x4ea963..0x4ea96d]
static void test_move_effect_ballistic_leg_ignores_the_water_plane() {
    Rig rig;
    rig.world.env.water_z = 0x8000; // water at 0.5
    AmmoTableEntry &ammo = rig.world.tables.ammo.entries[0];
    ammo.impact_effects[1].effect = "Effect_Smoke";
    ammo.flags |= kAmmoFlagClipWaterFx | kAmmoFlagNoGravity;
    ammo.max_age_ticks = 200;
    RoundSpawnParams p;
    p.owner = rig.shooter;
    p.shooter_handle = rig.shooter.packed;
    p.origin = {0.0f, 0.0f, -5.0f}; // under the plane
    p.dir_yaw_bam = 0;
    p.dir_pitch_bam = 0; // level, along +x, never crossing the plane
    p.ammo_index = 0;
    const int idx = rig.world.round_sim.spawn(rig.world, p, RoundConsequenceMode::Authoritative);
    CHECK(idx >= 0);
    if (idx < 0) return;
    LiveRound &r = rig.world.round_sim.rounds[static_cast<size_t>(idx)];
    rig.world.round_sim.tick(rig.world, nullptr);
    CHECK(r.active);
    CHECK(r.move_effect_live); // armed under water: no water term on this leg
    r.pos = {40.0f, 0.0f, -5.0f};
    rig.world.round_sim.tick(rig.world, nullptr);
    CHECK(r.active);
    CHECK(r.move_effect_live); // and never released by the plane
}

static void test_projectile_stamps_burn_before_death_dispatch() {
    PosedDamageRig rig(0);
    rig.world.ai.attach(rig.target);
    auto &inf = rig.world.ai.for_handle(rig.target)->inf;
    rig.target_entity()->engine_flags |= kEntityFlagPlayer;
    rig.ammo().secondary_anim = 2;
    rig.ammo().max_damage = 10;
    rig.fire_and_tick();
    CHECK(!rig.world.round_sim.hits.empty());
    CHECK(inf.burn_state == 2 && inf.idle_counter == 1);
}

void test_item_callbacks_receive_geometric_section_on_both_peers() {
    for (bool authority : {true, false}) {
        HeapWorldFixture fixture; auto &world = fixture.world;
        world.registry.configure_pool(0, 4); world.registry.configure_pool(2, 4);
        world.rules.mp_session = true; world.rules.logic_authority = authority;
        world.rules.projectile_authority = authority;
        Entity shooter; shooter.kind = EntityKind::Organic; shooter.item_type = 3;
        const auto owner = world.registry.spawn(0, shooter);
        Entity seed; seed.kind = EntityKind::Building; seed.item_type = 5;
        seed.item_id = 812; seed.has_item_def = true; seed.health = 500;
        seed.position = {4,0,0}; seed.yaw = 90;
        const auto h = world.registry.spawn(2, seed);
        auto &target = *world.registry.get(h);
        if (!authority) target.item_section_damage[2] = 15;
        ItemDeathTraits traits; traits.death_class = authority ? ItemDeathClass::kPalm : ItemDeathClass::kTower;
        traits.husk_sub_part_count = 4;
        world.tables.item_death_traits.set(812, traits);
        auto model = knife_person_face_model(7);
        const auto section = model.sections[0];
        model.sections.assign(3, {}); model.sections[2] = section;
        CollisionWorld collision;
        const int id = collision.add_model(std::move(model));
        collision.assign_entity(h, id); collision.build_tick_tables(world);
        world.collision = &collision;
        AmmoTableEntry ammo; ammo.name = "BULLET"; ammo.valid = true;
        ammo.velocity = 620; ammo.max_age_ticks = 20; ammo.weight_in_grains = 875; ammo.max_damage = 25;
        world.tables.ammo.entries.push_back(ammo);
        RoundSpawnParams shot; shot.owner = owner; shot.shooter_handle = owner.packed;
        shot.origin = {0,0,1}; shot.ammo_index = 0;
        CHECK(world.round_sim.spawn(world, shot, authority ? RoundConsequenceMode::Authoritative :
                RoundConsequenceMode::VisualOnly) >= 0);
        world.round_sim.tick(world, nullptr, &collision);
        CHECK(world.round_sim.impacts.size() == 1);
        if (authority) {
            CHECK(target.palm_damage[1] > 0 && target.palm_damage[0] == 0);
            CHECK(target.health < 500);
        } else {
            // Retail returns zero damage on MP peers but still invokes the
            // callback, whose existing counter can trigger another piece.
            CHECK(target.item_section_damage[2] == 15 && (target.engine_flags & 4) != 0);
            CHECK(target.class_think_ticks == 32);
            CHECK(target.health == 500 && world.round_sim.hits.empty() && world.round_sim.deaths.empty());
        }
    }
}

using test_world::DeathClipSource;

// A remote-peer PLAYER body the authority animates (so the org2 think cadence
// runs over it) behind a posed 14-section collision model: section 13 is the
// x3 critical head zone, section 0 the x1.25 body zone; `pose` puts exactly one
// of them on the +X ray.
struct PlayerVictimRig : HeapWorldFixture {
    static constexpr int kSectionCount = 14;
    CollisionWorld collision;
    DeathClipSource clips;
    EntityHandle shooter;
    EntityHandle victim;
    uint32_t tick = 0;

    PlayerVictimRig() {
        world.registry.configure_pool(0, 8);
        world.tables.player.has_item_def = true;
        world.tables.player.item_type = 3;
        world.tables.player.item_hp = 5000;
        world.ai.is_authority = true;
        world.ai.root_motion = &clips;

        Entity s;
        s.kind = EntityKind::Organic;
        s.item_type = 3;
        s.health = 100;
        shooter = world.registry.spawn(0, s);

        PlayerSpawn spawn;
        spawn.position = {5.0f, 0.0f, 0.0f};
        spawn.net_id = 0xFFF1;
        spawn.yaw = 90; // engine heading 0: the +X round arrives from behind
        victim = spawn_remote_player(world, spawn);
        CHECK(victim.valid());
        AiEntity *body = world.ai.for_handle(victim);
        CHECK(body != nullptr);
        if (body != nullptr) body->net_is_remote_peer = true;

        AmmoTableEntry ammo;
        ammo.name = "ZONE";
        ammo.valid = true;
        ammo.velocity = 620;
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        world.tables.ammo.entries.push_back(ammo);

        CollisionModel model;
        model.sections.resize(kSectionCount);
        for (CollisionSection &section : model.sections) {
            section.authored_bounds = true;
            section.min_x = section.min_y = section.min_z = -0x10000;
            section.max_x = section.max_y = section.max_z = 0x10000;
            section.radius = 0x10000;
        }
        const int32_t model_id = collision.add_model(std::move(model));
        collision.assign_entity(victim, model_id);
        world.collision = &collision;
        pose(13);
    }

    Entity *victim_entity() { return world.registry.get(victim); }
    AiEntity *victim_body() { return world.ai.for_handle(victim); }

    void pose(int on_ray_section) {
        std::vector<CollisionMatrix> matrices;
        matrices.reserve(kSectionCount);
        for (int section = 0; section < kSectionCount; ++section) {
            const int32_t center[3] = {
                5 * 65536,
                section == on_ray_section ? 0 : (20 + section) * 65536,
                58982,
            };
            matrices.push_back(collision_matrix_from_heading(0, center));
        }
        CHECK(collision.publish_entity_section_matrices(victim, std::move(matrices)));
        collision.build_tick_tables(world);
    }

    void set_health(int32_t health) {
        victim_entity()->health = health;
        if (AiEntity *body = victim_body()) body->health = static_cast<int16_t>(health);
    }

    void fire_and_tick() {
        RoundSpawnParams params;
        params.owner = shooter;
        params.shooter_handle = shooter.packed;
        params.origin = {0.0f, 0.0f, 0.9f};
        params.ammo_index = 0;
        CHECK(world.round_sim.spawn(world, params) >= 0);
        collision.build_tick_tables(world);
        world.round_sim.tick(world, nullptr);
    }

    // The org2 body update over the victim only (no round flight).
    void run_body_ticks(int count) {
        TickContext ctx;
        ctx.world = &world;
        ctx.is_authority = true;
        for (int i = 0; i < count; ++i) {
            ctx.logic_tick = ++tick;
            world.update_all_entities(ctx);
        }
    }
};

// The plyr callback's waypoint tail ends a round hit's event too: a team 1/2
// player whose AI slot carries a route channel marks the node it stands in.
// [orig: Entity_HandleDamageAndTriggerZones @0x407B64..0x407C6B, reached from
// Projectile_ProcessDamageOnTarget's event-1 call @0x4E820E]
void test_round_hit_runs_the_player_waypoint_tail() {
    auto r = std::make_unique<PlayerVictimRig>();
    Entity *v = r->victim_entity();
    v->team = 1;
    v->net_id = 7;
    AiEntity *body = r->victim_body();
    body->slot.f[37] = 3;
    NavNodeTable &nav = r->world.ai.nav;
    nav.channels.resize(4);
    nav.channels[3].count = 1;
    nav.channels[3].entries[0] = 0;
    nav.nodes.resize(1);
    nav.nodes[0].f[0] = 0x10000; // radius 1 u, centered on the body
    nav.nodes[0].f[1] = body->pos[0];
    nav.nodes[0].f[2] = body->pos[1];
    r->fire_and_tick();
    CHECK(v->spawn_phase == 64);
    CHECK(r->world.script.relations.single_visited(7, 3, 0));
    CHECK(r->world.script.relations.group_visited(1, 3, 0));
}

// A round hit records its round, the damage, the struck section and the
// target in the global hit record. [orig: Projectile_ProcessDamageOnTarget
// Projectile_CopyEntityToHitRecord @0x4E81BA..0x4E81C0, +0x30 @0x4E81C9,
// +0x38 @0x4E81D2, +0x48 @0x4E81D7]
void test_round_hit_records_the_round() {
    auto r = std::make_unique<PlayerVictimRig>();
    r->fire_and_tick();
    const HitRecord &record = r->world.round_sim.hit_record;
    CHECK(record.has_round);
    CHECK(record.round_owner == r->shooter && record.owner == r->shooter);
    CHECK(record.round_ammo_index == 0);
    CHECK(record.damage == 1860);
    CHECK(record.section == 13);
    CHECK(record.target == r->victim);
}

// The kill-cause bits live on the ENTITY (retail +44 bits 8..11), latched at
// hit time and read at the death edge: a non-lethal head hit marks the next
// kill critical until the plyr callback's 64-tick think clears it, and every
// hit re-arms that window. [orig: Weapon_CalcImpactDamage @0x4ec9c6 latch;
// Entity_UpdateInfantryPlayerBody @0x4b4bc9..0x4b4be9 cadence;
// Entity_HandleDamageAndTriggerZones @0x407b4d..0x407b4f clear, @0x407b5e /
// @0x407c71 re-arm; GameEvent_PlayerDeath @0x5171d8 consumer]
void test_kill_cause_bits_latch_per_hit_and_clear_on_the_think_cadence() {
    {
        // A non-lethal head hit latches 0x800; a limb kill 30 ticks later
        // reports critical.
        auto r = std::make_unique<PlayerVictimRig>();
        r->fire_and_tick();
        Entity *v = r->victim_entity();
        CHECK(v->health == 5000 - 1860);
        CHECK(r->world.round_sim.deaths.empty());
        CHECK((v->cause_flags & 0x800u) != 0);
        CHECK(v->spawn_phase == 64);
        r->run_body_ticks(30);
        CHECK(v->spawn_phase == 34);
        CHECK((v->cause_flags & 0x800u) != 0);
        r->pose(0);
        r->set_health(100);
        r->fire_and_tick();
        CHECK(r->world.round_sim.deaths.size() == 1);
        CHECK(v->health == 0);
        if (r->world.round_sim.deaths.size() == 1)
            CHECK((r->world.round_sim.deaths[0].event_flags & 0x800u) != 0);
    }
    {
        // 65 body ticks with no further hit clear the latch: the same kill is
        // a standard one. The 64th tick brings the counter to 0; the 65th
        // fires the think.
        auto r = std::make_unique<PlayerVictimRig>();
        r->fire_and_tick();
        Entity *v = r->victim_entity();
        r->run_body_ticks(64);
        CHECK(v->spawn_phase == 0);
        CHECK((v->cause_flags & 0x800u) != 0);
        r->run_body_ticks(1);
        CHECK(v->spawn_phase == 63);
        CHECK((v->cause_flags & 0x800u) == 0);
        r->pose(0);
        r->set_health(100);
        r->fire_and_tick();
        CHECK(r->world.round_sim.deaths.size() == 1);
        if (r->world.round_sim.deaths.size() == 1)
            CHECK(r->world.round_sim.deaths[0].event_flags == 0);
    }
    {
        // A non-critical hit at tick 60 re-arms the window, so a kill at tick
        // 100 still reports the head hit from tick 0.
        auto r = std::make_unique<PlayerVictimRig>();
        r->fire_and_tick();
        Entity *v = r->victim_entity();
        r->run_body_ticks(60);
        CHECK(v->spawn_phase == 4);
        r->pose(0);
        r->fire_and_tick();
        CHECK(r->world.round_sim.deaths.empty());
        CHECK(v->spawn_phase == 64);
        CHECK((v->cause_flags & 0x800u) != 0);
        r->run_body_ticks(40);
        CHECK(v->spawn_phase == 24);
        r->set_health(100);
        r->fire_and_tick();
        CHECK(r->world.round_sim.deaths.size() == 1);
        if (r->world.round_sim.deaths.size() == 1)
            CHECK((r->world.round_sim.deaths[0].event_flags & 0x800u) != 0);
    }
    {
        // OneShotKill returns before the zone branch: no latch even on the
        // head. Its flat 2000 [orig: Weapon_CalcImpactDamage @0x4ec942] kills
        // a stock-HP player; the rig's 5000 HP exists only to survive the
        // 1860 head hit of the blocks above, so bring the body back to a
        // stock 100 first.
        auto r = std::make_unique<PlayerVictimRig>();
        r->world.rules.mp_session = true;
        r->world.rules.one_shot_kill = true;
        r->set_health(100);
        r->fire_and_tick();
        CHECK(r->world.round_sim.deaths.size() == 1);
        if (r->world.round_sim.deaths.size() == 1)
            CHECK(r->world.round_sim.deaths[0].event_flags == 0);
        CHECK((r->victim_entity()->cause_flags & 0x800u) == 0);
    }
}

// One round's second lethal player hit latches the same-projectile bit 0x100
// on that victim (the +688 kill byte past 1); the first victim never carries
// it. [orig: Projectile_ProcessDamageOnTarget @0x4e8159..0x4e816b]
void test_same_projectile_second_player_kill_latches_0x100() {
    HeapWorldFixture fixture;
    World &world = fixture.world;
    world.registry.configure_pool(0, 8);
    Entity s;
    s.kind = EntityKind::Organic;
    s.item_type = 3;
    s.health = 100;
    const EntityHandle shooter = world.registry.spawn(0, s);
    auto spawn_player_victim = [&](float x) {
        Entity t;
        t.kind = EntityKind::Organic;
        t.has_item_def = true;
        t.item_type = 3;
        t.position = {x, 0.0f, 0.0f};
        t.health = 10;
        t.flags = kEntityFlagPlayer;
        t.engine_flags = kEntityFlagPlayer;
        return world.registry.spawn(0, t);
    };
    const EntityHandle first = spawn_player_victim(5.0f);
    const EntityHandle second = spawn_player_victim(6.0f);

    AmmoTableEntry ammo;
    ammo.name = "ZONE";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    ammo.weight_in_grains = 875;
    world.tables.ammo.entries.push_back(ammo);

    LiveRound round;
    round.active = true;
    round.owner = shooter;
    round.shooter_handle = shooter.packed;
    round.ammo_index = 0;
    round.vel = {10.0f, 0.0f, 0.0f};
    auto hit_person = [&](EntityHandle h) {
        ProjectileHit hit;
        hit.hit_class = ProjectileHitClass::Person;
        hit.geometry_entity = h;
        hit.section_index = 1;
        hit.bone_index = 1;
        hit.hit_zone = 1;
        FixedVec3 velocity{10 * 65536, 0, 0};
        world.round_sim.process_damage_hit(world, round, hit, velocity);
    };
    hit_person(first);
    hit_person(second);
    CHECK(round.player_kills == 2);
    CHECK(world.round_sim.deaths.size() == 2);
    CHECK(world.registry.get(first)->health == 0);
    CHECK(world.registry.get(second)->health == 0);
    CHECK((world.registry.get(first)->cause_flags & 0x100u) == 0);
    CHECK((world.registry.get(second)->cause_flags & 0x100u) != 0);
    if (world.round_sim.deaths.size() == 2) {
        CHECK((world.round_sim.deaths[0].event_flags & 0x100u) == 0);
        CHECK((world.round_sim.deaths[1].event_flags & 0x100u) != 0);
    }
}

// The kill accounting's lethal edge reads the health word BEFORE the hit: a
// body an unclamped path left below zero is raised back to zero by the clamped
// hit but restages no death, and a lethal hit on a dead-flagged body stages
// its death without the kill event. [orig: Projectile_ProcessDamageOnTarget
// @0x4E80F7..0x4E8101 pre-hit health > 0, @0x4E810A damage >= health,
// @0x4E811F Flags & 2 skipping Score_ProcessKillEvent @0x4E8133]
void test_kill_event_reads_the_pre_hit_health_and_the_dead_flag() {
    HeapWorldFixture fixture;
    World &world = fixture.world;
    world.registry.configure_pool(0, 8);
    Entity s;
    s.kind = EntityKind::Organic;
    s.item_type = 3;
    s.health = 100;
    const EntityHandle shooter = world.registry.spawn(0, s);
    auto spawn_victim = [&](int32_t health, uint32_t flags) {
        Entity t;
        t.kind = EntityKind::Organic;
        t.has_item_def = true;
        t.item_type = 3;
        t.position = {5.0f, 0.0f, 0.0f};
        t.health = health;
        t.flags = flags;
        t.engine_flags = flags;
        return world.registry.spawn(0, t);
    };

    AmmoTableEntry ammo;
    ammo.name = "ZONE";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    ammo.weight_in_grains = 875;
    world.tables.ammo.entries.push_back(ammo);

    LiveRound round;
    round.active = true;
    round.owner = shooter;
    round.shooter_handle = shooter.packed;
    round.ammo_index = 0;
    round.vel = {10.0f, 0.0f, 0.0f};
    auto hit_person = [&](EntityHandle h) {
        ProjectileHit hit;
        hit.hit_class = ProjectileHitClass::Person;
        hit.geometry_entity = h;
        hit.section_index = 1;
        hit.bone_index = 1;
        hit.hit_zone = 1;
        FixedVec3 velocity{10 * 65536, 0, 0};
        world.round_sim.process_damage_hit(world, round, hit, velocity);
    };

    const EntityHandle live = spawn_victim(10, 0);
    hit_person(live);
    CHECK(world.registry.get(live)->health == 0);
    CHECK(world.round_sim.deaths.size() == 1);
    if (world.round_sim.deaths.size() == 1)
        CHECK(world.round_sim.deaths[0].kill_event);

    world.round_sim.deaths.clear();
    const EntityHandle below_zero = spawn_victim(-5, 0);
    hit_person(below_zero);
    CHECK(world.registry.get(below_zero)->health == 0);
    CHECK(world.round_sim.deaths.empty());

    world.round_sim.deaths.clear();
    const EntityHandle flagged = spawn_victim(10, kEntityFlagDead);
    hit_person(flagged);
    CHECK(world.registry.get(flagged)->health == 0);
    CHECK(world.round_sim.deaths.size() == 1);
    if (world.round_sim.deaths.size() == 1)
        CHECK(!world.round_sim.deaths[0].kill_event);
}

// On a non-authority in-session peer the person class callback still fires
// (with Weapon_CalcImpactDamage's forced zero) on the peer's OWN body — the
// only person a joiner resolves to a World entity: the hit stages the death
// anim, tips the body (the FP camera roll's source), and stamps the attacker,
// with no health, hit, death, or cause-bit consequence. [orig:
// Weapon_CalcImpactDamage @0x4ec933; Projectile_ProcessDamageOnTarget
// @0x4e81b1 (authority-gated subtraction) / @0x4e820e (callback)]
void test_visual_person_hit_on_the_joiners_body_stages_and_rolls_without_consequences() {
    HeapWorldFixture fixture;
    World &world = fixture.world;
    world.registry.configure_pool(0, 8);
    world.rules.mp_session = true;
    world.rules.projectile_authority = false;
    world.rules.logic_authority = false;
    world.tables.player.has_item_def = true;
    world.tables.player.item_type = 3;
    world.tables.player.item_hp = 100;

    Entity s;
    s.kind = EntityKind::Organic;
    s.item_type = 3;
    s.health = 100;
    const EntityHandle shooter = world.registry.spawn(0, s);

    PlayerSpawn spawn;
    spawn.position = {5.0f, 0.0f, 0.0f};
    spawn.net_id = 0xFFF0;
    spawn.yaw = 90;
    const EntityHandle victim = spawn_player(world, spawn); // the joiner's own body
    CHECK(victim.valid() && world.cached.local_player == victim);
    AiEntity *body = world.ai.for_handle(victim);
    CHECK(body != nullptr);

    AmmoTableEntry ammo;
    ammo.name = "ZONE";
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    ammo.weight_in_grains = 875;
    world.tables.ammo.entries.push_back(ammo);

    CollisionWorld collision;
    CollisionModel model;
    model.sections.resize(4);
    for (CollisionSection &section : model.sections) section.radius = -1;
    CollisionSection &torso = model.sections[3];
    torso.authored_bounds = true;
    torso.min_x = torso.min_y = torso.min_z = -0x4000;
    torso.max_x = torso.max_y = torso.max_z = 0x4000;
    torso.radius = 0x4000;
    const int32_t model_id = collision.add_model(std::move(model));
    collision.assign_entity(victim, model_id);
    const int32_t bone_at[3] = {5 * 65536, 0, 58982};
    std::vector<CollisionMatrix> matrices(4, collision_matrix_from_heading(0, bone_at));
    CHECK(collision.publish_entity_section_matrices(victim, std::move(matrices)));
    collision.build_tick_tables(world);
    world.collision = &collision;

    RoundSpawnParams params;
    params.owner = shooter;
    params.shooter_handle = shooter.packed;
    params.origin = {0.0f, 0.0f, 0.9f};
    params.ammo_index = 0;
    CHECK(world.round_sim.spawn(world, params, RoundConsequenceMode::VisualOnly) >= 0);
    world.round_sim.tick(world, nullptr);

    Entity *v = world.registry.get(victim);
    CHECK(v->health == 100);
    CHECK(world.round_sim.hits.empty());
    CHECK(world.round_sim.deaths.empty());
    CHECK(v->cause_flags == 0);
    CHECK(v->death_anim_state == compute_death_anim_state(3, 2, death_cause::kBullet));
    CHECK(v->last_attacker == shooter);
    if (body != nullptr) CHECK(body->roll == -0x05B05B00);
    world.collision = nullptr;
}

// A round inside a building's BB volume crosses terrain without stopping,
// then resumes terrain collision after leaving the volume [orig: @0x4EA2F0].
void test_projectile_indoor_terrain_gate() {
    HeapWorldFixture f;
    World &w = f.world;
    w.registry.configure_pool(2, 4);
    Entity building;
    building.kind = EntityKind::Building;
    building.item_type = 5;
    building.position = {10.0f, -20.0f, 0.0f};
    building.bound_radius = 30.0f;
    const auto h = w.registry.spawn(2, building);
    CollisionModel model;
    CollisionVolume bb;
    bb.type = 8;
    bb.flags = 0; // flags ^ 6 includes the indoor bit
    bb.min_x = bb.min_y = bb.min_z = -5 * 65536;
    bb.max_x = bb.max_y = bb.max_z = 5 * 65536;
    model.volumes.push_back(bb);
    CollisionSection sec;
    sec.volume_count = 1;
    model.sections.push_back(sec);
    CollisionWorld cw;
    cw.assign_entity(h, cw.add_model(std::move(model)));
    std::vector<uint16_t> heights(512u * 512u, 0);
    std::vector<int> sectors(256u, 1);
    opennova::terrain::TerrainHeightField flat;
    flat.heightmap = heights.data();
    flat.dim = 512;
    flat.layout.sector_grid = sectors.data();
    cw.terrain = &flat;
    cw.build_initial_tables(w);
    w.collision = &cw;
    AmmoTableEntry ammo;
    ammo.valid = true;
    ammo.velocity = 620;
    ammo.max_age_ticks = 20;
    w.tables.ammo.entries.push_back(ammo);
    RoundSpawnParams params;
    params.origin = {10.0f, -20.0f, 2.0f};
    params.ammo_index = 0;
    const int slot = w.round_sim.spawn(w, params);
    CHECK(slot >= 0);
    if (slot < 0) return;
    LiveRound &r = w.round_sim.rounds[slot];
    r.vel = {0.0f, 0.0f, -10.0f};
    w.round_sim.tick(w, &flat, &cw);
    CHECK(r.active);
    CHECK(r.pos.z < 0.0f);
    // Outside the volume, the same terrain-crossing segment must stop.
    r.pos = {50.0f, -20.0f, 2.0f};
    r.vel = {0.0f, 0.0f, -10.0f};
    w.round_sim.tick(w, &flat, &cw);
    CHECK(!r.active);
}

void test_guided_round_uses_live_target_ammo_and_pool_lifetime() {
    Rig rig;
    auto &world=rig.world;
    world.rules.mp_session=true;
    auto &ammo=world.tables.ammo.entries[0];
    ammo.tracer_item_friendly=10;
    ammo.velocity=248;
    ammo.max_age_ticks=100;
    ammo.turnrate_maxyaw=1000000;
    ammo.turnrate_maxpit=2000000;
    world.throwables.classes.set({10,ThrowClass::kNone,ThrowClass::kStinger});
    world.registry.get(rig.shooter)->last_fire_target=rig.target;
    world.registry.get(rig.target)->position={500,100,20};
    RoundSpawnParams params;
    params.owner=rig.shooter; params.shooter_handle=rig.shooter.packed;
    params.origin={0,0,20}; params.ammo_index=0; params.shot_seq=77;
    const int slot=world.round_sim.spawn(world,params,RoundConsequenceMode::VisualOnly);
    CHECK(slot>=0); if(slot<0) return;
    auto &round=world.round_sim.rounds[slot];
    CHECK(round.guided_family==GuidedFamily::Stinger);
    CHECK(round.vel.x==0.5f); // (248 / 8) / 62, at yaw/pitch zero
    CHECK(world.round_sim.find_guided(77)==&round);
    world.round_sim.tick(world,nullptr);
    world.round_sim.tick(world,nullptr);
    world.registry.get(rig.target)->position={500,-100,30};
    round.age_ticks=39;
    world.round_sim.tick(world,nullptr);
    // The in-flight refresh samples the person target's AIM ORIGIN: its
    // Position plus the phased CameraOffset point and the XY jitter, keyed on
    // tick + 36 * net id. This rig's target carries a zero CameraOffset.
    // [orig: Entity_ComputeWeaponFireOrigin @0x43B4C2..0x43B523; stng @0x4463A3]
    const Entity *tracked=world.registry.get(rig.target);
    const uint32_t phase=world.logic_tick+36u*uint32_t(tracked->net_id);
    const int32_t jitter=int32_t(phase&0x60u);
    CHECK(round.guided.steer[0]==500*65536+(jitter-64)*64);
    CHECK(round.guided.steer[1]==-100*65536+(jitter-32)*64 && round.guided.steer[2]==30*65536);
    CHECK(round.yaw_bam==-1000000); // ammo's yaw limit, not a fixed default
    CHECK(round.vel.x>3.9f && round.vel.x<=4.0f); // this ammo's 248 u/s
    CHECK(round.guided.target==rig.target.packed);
    // The armed head presents its obj row only for a kill-zone class
    // [orig: Projectile_UpdatePhysics @0x4E9DDC].
    ammo.kztype=ammo_kz::kStandard;
    round.guided.flags|=1; round.det_at_expiry=true;
    world.round_sim.tick(world,nullptr);
    CHECK(!round.active && world.round_sim.find_guided(77)==nullptr);
    CHECK(!world.round_sim.impacts.empty());
    CHECK(world.round_sim.hits.empty()); // a visual round never applies authority damage
    world.round_sim.reset();
    CHECK(world.round_sim.find_guided(77)==nullptr);
}

// The armed-expiry head pushes the kill zone, and presents its obj row, only
// for an ammo with a kill-zone class; a kztype-0 armed round releases
// silently. [orig: Projectile_UpdatePhysics @0x4E9DC6 (flag), @0x4E9DDC..
// 0x4E9DE1 (kztype), push @0x4E9E03]
void test_armed_expiry_detonates_only_a_kill_zone_class() {
    for (int32_t kztype : {0, static_cast<int32_t>(ammo_kz::kBullets)}) {
        Rig r;
        AmmoTableEntry &ammo = r.world.tables.ammo.entries[0];
        ammo.kztype = kztype;
        ammo.kz_damage = 50;
        ammo.kz_maxradius = 4.0f;
        const int slot = r.fire();
        CHECK(slot >= 0);
        if (slot < 0) continue;
        r.world.round_sim.rounds[static_cast<size_t>(slot)].det_at_expiry = true;
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(!r.world.round_sim.rounds[static_cast<size_t>(slot)].active);
        if (kztype == 0) {
            CHECK(r.world.explosions.queue.empty());
            CHECK(r.world.round_sim.impacts.empty());
        } else {
            CHECK(r.world.explosions.queue.size() == 1);
            if (!r.world.explosions.queue.empty())
                CHECK(r.world.explosions.queue[0].type == ammo_kz::kBullets);
            CHECK(r.world.round_sim.impacts.size() == 1);
            if (!r.world.round_sim.impacts.empty())
                CHECK(r.world.round_sim.impacts[0].effect_tag == 4);
        }
    }
}

// Each impact handler has its own kill-zone producer gates and none of them
// filters the class beyond nonzero: the person handler needs the class, an
// armed round, the authority and a nonzero kz_damage; the terrain handler
// only the class and an armed round.
// [orig: Projectile_HandleTerrainImpact_0 @0x4E9AE2..0x4E9B08 -> @0x4E9B26;
//  Projectile_HandleTerrainImpact @0x4E928C..0x4E92A1 -> @0x4E92C0]
void test_impact_producers_apply_their_own_gates() {
    const auto person_queues = [](int32_t kztype, int32_t kz_damage, int32_t arm_age) {
        Rig r;
        AmmoTableEntry &ammo = r.world.tables.ammo.entries[0];
        ammo.kztype = kztype;
        ammo.kz_damage = kz_damage;
        ammo.kz_maxradius = 4.0f;
        ammo.arm_age_ticks = arm_age;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.active_count == (arm_age > 0 ? 1 : 0)); // a dud flies on
        return r.world.explosions.queue.size();
    };
    CHECK(person_queues(ammo_kz::kBullets, 50, 0) == 1);
    CHECK(person_queues(ammo_kz::kBullets, 0, 0) == 0); // @0x4E9B02
    CHECK(person_queues(0, 50, 0) == 0);                // @0x4E9AE2
    CHECK(person_queues(ammo_kz::kStandard, 50, 2) == 0); // pre-arm @0x4E9AE9

    const auto terrain_queues = [](int32_t kztype, int32_t kz_damage, int32_t arm_age) {
        HeapWorldFixture fixture;
        World &world = fixture.world;
        world.registry.configure_pool(0, 4);
        Entity shooter;
        shooter.kind = EntityKind::Organic;
        shooter.item_type = 3;
        shooter.position = {0.0f, 0.0f, 10.0f};
        const EntityHandle owner = world.registry.spawn(0, shooter);
        AmmoTableEntry ammo;
        ammo.name = "KZ_TERRAIN";
        ammo.valid = true;
        ammo.velocity = 620;
        ammo.max_age_ticks = 20;
        ammo.arm_age_ticks = arm_age;
        ammo.kztype = kztype;
        ammo.kz_damage = kz_damage;
        ammo.kz_maxradius = 4.0f;
        world.tables.ammo.entries.push_back(ammo);
        std::vector<uint16_t> heights(512u * 512u, 0);
        std::vector<int> sectors(256u, 1);
        opennova::terrain::TerrainHeightField flat;
        flat.heightmap = heights.data();
        flat.dim = 512;
        flat.layout.sector_grid = sectors.data();
        CollisionWorld collision;
        collision.terrain = &flat;
        collision.build_tick_tables(world);
        world.collision = &collision;
        RoundSpawnParams params;
        params.owner = owner;
        params.shooter_handle = owner.packed;
        params.origin = {10.0f, -20.0f, 2.0f};
        params.ammo_index = 0;
        const int slot = world.round_sim.spawn(world, params);
        CHECK(slot >= 0);
        if (slot < 0) return size_t{99};
        world.round_sim.rounds[static_cast<size_t>(slot)].vel = Vec3{0.0f, 0.0f, -10.0f};
        world.round_sim.tick(world, &flat, &collision);
        return world.explosions.queue.size();
    };
    CHECK(terrain_queues(ammo_kz::kBullets, 50, 0) == 1);
    CHECK(terrain_queues(ammo_kz::kBullets, 0, 0) == 1); // no kz_damage read
    CHECK(terrain_queues(ammo_kz::kStandard, 50, 5) == 0); // pre-arm @0x4E9292..0x4E92A1
    CHECK(terrain_queues(0, 50, 0) == 0);                 // no class @0x4E928C
}

// Stock JOX tank rounds carry the Bullets kill-zone class (kztype 6,
// rounds_kz_Bullets): M1Round authors kz_damage 205 over 6..12 units. The
// round's stop queues its kill zone and the drain's 2/5/6/7 arm splashes a
// bystander the round never touched.
// [orig: the kztype name table @0x8133E0 (rounds_kz_Bullets @0x813410);
//  Projectile_HandleTerrainImpact_0 @0x4E9B26; the drain jumptable
//  @0x4EADC6 -> @0x4EADCD, Entity_ApplyWeaponDamage @0x4E6820]
void test_jox_tank_round_bullets_class_splashes() {
    Rig r;
    AmmoTableEntry m1;
    m1.name = "M1Round";
    m1.valid = true;
    m1.velocity = 620;
    m1.max_age_ticks = 20;
    m1.weight_in_grains = 875;
    m1.kztype = ammo_kz::kBullets;
    m1.kz_damage = 205;
    m1.kz_minradius = 6.0f;
    m1.kz_maxradius = 12.0f;
    const int m1_index = static_cast<int>(r.world.tables.ammo.entries.size());
    r.world.tables.ammo.entries.push_back(m1);

    Entity bystander;
    bystander.kind = EntityKind::Organic;
    bystander.has_item_def = true;
    bystander.item_type = 3;
    bystander.position = {5.0f, 4.0f, 0.0f};
    bystander.bound_radius = 0.6f;
    bystander.health = bystander.health_max = 1000;
    const EntityHandle bystander_h = r.world.registry.spawn(0, bystander);

    RoundSpawnParams p;
    p.owner = r.shooter;
    p.shooter_handle = r.shooter.packed;
    p.origin = {0.0f, 0.0f, 0.9f};
    p.ammo_index = m1_index;
    CHECK(r.world.round_sim.spawn(r.world, p) >= 0);
    r.world.round_sim.tick(r.world, nullptr);
    CHECK(r.world.explosions.queue.size() == 1);
    if (!r.world.explosions.queue.empty())
        CHECK(r.world.explosions.queue[0].type == ammo_kz::kBullets);
    r.world.explosions.process(r.world, nullptr, nullptr, -1.0e9f, r.world.out.destruction);
    // Well inside kz_minradius: the full 205, no falloff.
    CHECK(r.world.registry.get(bystander_h)->health == 1000 - 205);
}

int main() {
    test_armed_expiry_detonates_only_a_kill_zone_class();
    test_impact_producers_apply_their_own_gates();
    test_jox_tank_round_bullets_class_splashes();
    test_guided_round_uses_live_target_ammo_and_pool_lifetime();
    test_projectile_indoor_terrain_gate();
    test_item_callbacks_receive_geometric_section_on_both_peers();
    test_projectile_stamps_burn_before_death_dispatch();
    test_arming_dud_and_armed_damage();
    test_missing_item_def_consumes_round_without_damage();
    test_damage_uses_retail_signed_wrap_and_ftol_cap();
    test_nodie_and_nontransparent_damage_gates();
    test_posed_head_zone_multiplier();
    test_item_type_zone_domain_and_attrib_0200_sections();
    test_shooter_damage_class_runs_after_zone_truncation();
    test_body_armor_energy_and_impact_row();
    test_person_hit_applies_the_surface_drag_leg();
    test_network_oneshot_authority_and_session_gate();
    test_visual_only_rounds_have_no_gameplay_consequences();
    test_visual_person_proxy_keeps_wire_identity_out_of_authority();
    test_knife_instant_kill_zone_raycast();
    test_bullet_building_material_is_plain_plus_four();
    test_material_15_breaks_the_building_glass_section();
    test_terrain_impact_samples_charmap_surface();
    test_terrain_impact_emits_permanent_scorch();
    test_terrain_stop_records_the_round();
    test_visual_dynamic_proxy_projects_decoded_pose_geometry();
    test_visual_dynamic_proxy_carrier_gate_and_unresolved_model_raises();
    test_visual_dynamic_proxy_excludes_shooter_self_slot();
    test_visual_throwable_motor_sweeps_wire_proxies();
    test_visual_infantry_proxy_joins_person_walk();
    test_person_walk_orders_local_player_by_its_server_handle();
    test_signed_armor_equality_and_damage_state_gates();
    test_signed_health_subtraction_wraps_at_entity_word();
    test_person_impact_tag_splits_on_identity_and_squad_health();
    test_exact_one_hop_vehicle_parent_damage_routing();
    test_vehicle_occupant_reduction_count_cap_and_depth();
    test_retail_force_order_and_stock_gates();
    test_retail_aerodynamic_drag_vectors();
    test_consumed_hit_skips_post_sweep_forces();
    test_move_effect_water_release_reads_pre_move_z();
    test_move_effect_ballistic_leg_ignores_the_water_plane();
    test_kill_cause_bits_latch_per_hit_and_clear_on_the_think_cadence();
    test_round_hit_records_the_round();
    test_round_hit_runs_the_player_waypoint_tail();
    test_same_projectile_second_player_kill_latches_0x100();
    test_kill_event_reads_the_pre_hit_health_and_the_dead_flag();
    test_visual_person_hit_on_the_joiners_body_stages_and_rolls_without_consequences();
    if (failures == 0) std::printf("projectile_combat_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
