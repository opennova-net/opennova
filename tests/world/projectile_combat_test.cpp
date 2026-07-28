// Observable projectile consequence tests through RoundSim's public seam:
// arming/dud substitution, NoDie, and geometric dead/indestructible blockers.
#include <cstdio>
#include <vector>

#include "terrain/height_field.h"
#include "world/collision.h"
#include "world/world.h"

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) {                                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                 \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

struct Rig {
    World world;
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

        world.ammo.entries.resize(2);
        AmmoTableEntry &armed = world.ammo.entries[0];
        armed.name = "ARMED";
        armed.valid = true;
        armed.velocity = 620; // 10 units/tick
        armed.max_age_ticks = 20;
        armed.arm_age_ticks = 2;
        armed.weight_in_grains = 875;
        armed.max_damage = 25;
        armed.notarmmed_ammo = "DUD";

        AmmoTableEntry &dud = world.ammo.entries[1];
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
struct PosedDamageRig {
    World world;
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
        world.ammo.entries.push_back(ammo);

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
    AmmoTableEntry &ammo() { return world.ammo.entries[0]; }

    void fire_and_tick() {
        RoundSpawnParams params;
        params.owner = shooter;
        params.shooter_handle = shooter.packed;
        params.origin = {0.0f, 0.0f, 0.9f};
        params.ammo_index = 0;
        CHECK(world.round_sim.spawn(world, params) >= 0);
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
    world.ammo.entries.push_back(ammo);

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
    r.world.ammo.entries[0].tracer_rate = 1;
    r.world.ammo.entries[0].tracer_type_friendly = 7;
    r.world.ammo.entries[0].tracer_type_enemy = 7;
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
    r.world.ammo.entries[0].arm_age_ticks = 0;
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
    r.world.ammo.entries[0].arm_age_ticks = 0;
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
    AmmoTableEntry &ammo = r.world.ammo.entries[0];
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
    r.world.ammo.entries[0].arm_age_ticks = 0;
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
    world.ammo.entries.push_back(ammo);

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
        PosedDamageRig r(0);
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 775); // person zone 0: 620 * 1.25
        CHECK(r.target_entity()->health == 4225);
        CHECK((r.target_entity()->flags & 0x800u) == 0);
    }
    {
        PosedDamageRig r(0);
        r.target_entity()->item_type = 5;
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 620); // zones are type-3-only
        CHECK(r.target_entity()->health == 4380);
    }
    {
        PosedDamageRig r(0);
        r.target_entity()->item_attrib = 0x200u;
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        // The 0x200 branch uses its own section-code domain; section 0 is not
        // interpreted as an ordinary person head zone.
        CHECK(r.world.round_sim.hits[0].damage == 620);
        CHECK((r.target_entity()->flags & 0x800u) == 0);
    }

    {
        PosedDamageRig r(13);
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 1860); // ordinary critical zone: x3
        CHECK((r.target_entity()->flags & 0x800u) != 0);
    }

    // ItemDefAttrib 0x200 selects the retail section-code switch. These four
    // codes receive 6.0x; the literal cases prevent a range approximation.
    const int special_sections[] = {2, 3, 6, 7};
    for (int section : special_sections) {
        PosedDamageRig r(section);
        r.target_entity()->item_attrib = 0x200u;
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 3720); // 620 * 6.0
        CHECK(r.target_entity()->health == 1280);
        CHECK((r.target_entity()->flags & 0x800u) != 0);
    }

    // A walk crossing TWO spheres splits the channels: ray[31] keeps the first/
    // highest overlap (7) while ray[32] ends at the final/lowest (4). The
    // attrib-0x200 seat switch reads ray[31] (hitZoneData+124 @0x4ec977).
    {
        PosedDamageRig r(7, 4);
        r.target_entity()->item_attrib = 0x200u;
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 3720); // seat code 7 via ray[31]
        CHECK((r.target_entity()->flags & 0x800u) != 0);
    }
    // The normal-infantry table keeps reading ray[32] (@0x4ec9bf): final zone 4
    // takes 1.25x even though the primary bone 7 sits in the 1.0x band.
    {
        PosedDamageRig r(7, 4);
        r.fire_and_tick();
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 775); // 620 * 1.25 via ray[32]
        CHECK((r.target_entity()->flags & 0x800u) == 0);
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

void test_network_oneshot_authority_and_session_gate() {
    {
        Rig r;
        r.world.ammo.entries[0].arm_age_ticks = 0;
        r.world.mp_session = true;
        r.world.projectile_authority = true;
        r.world.one_shot_kill = true;
        r.world.registry.get(r.target)->health = 3000;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 2000);
        CHECK(r.world.registry.get(r.target)->health == 1000);
        // The early MP option return bypasses the authored max_damage=25.
        CHECK(r.world.ammo.entries[0].max_damage == 25);
    }
    {
        Rig r;
        r.world.ammo.entries[0].arm_age_ticks = 0;
        r.world.mp_session = false;
        r.world.one_shot_kill = true;
        r.world.registry.get(r.target)->health = 3000;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.hits.size() == 1);
        CHECK(r.world.round_sim.hits[0].damage == 25);
        CHECK(r.world.registry.get(r.target)->health == 2975);
    }
    {
        Rig r;
        r.world.ammo.entries[0].arm_age_ticks = 0;
        r.world.mp_session = true;
        r.world.projectile_authority = false;
        r.world.one_shot_kill = true;
        // A visual client's only live LOCAL person is its own player L; other
        // local pool-0 slots are load-frozen ghosts excluded from the
        // projectile walks (their live poses arrive as wire proxies).
        r.world.cached.local_player = r.target;
        Entity *target = r.world.registry.get(r.target);
        CHECK(target != nullptr);
        if (target == nullptr) return;
        // Pin the independent retail world-role gate. Even if a client caller
        // forgets to mark its round VisualOnly, a non-authority network world
        // cannot normalize entity words or queue the impact kill zone.
        target->health = 0x1FFFF;
        target->health_max = 0x1FFFE;
        AmmoTableEntry &ammo = r.world.ammo.entries[0];
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
        const int instant_index = static_cast<int>(r.world.ammo.entries.size());
        r.world.ammo.entries.push_back(instant);
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
    r.world.mp_session = true;
    r.world.projectile_authority = false;
    r.world.one_shot_kill = true;
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

    AmmoTableEntry &bullet = r.world.ammo.entries[0];
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
    CHECK(target->death_anim_state == before.death_anim_state);
    CHECK(!r.world.relations.group_group(
        TriggerRelations::kShot, shooter->group_id, target->group_id));
    CHECK(!r.world.relations.single_group(
        TriggerRelations::kShot, shooter->net_id, target->group_id));
    CHECK(!r.world.relations.group_single(
        TriggerRelations::kShot, shooter->group_id, target->net_id));
    CHECK(!r.world.relations.single_single(
        TriggerRelations::kShot, shooter->net_id, target->net_id));
    CHECK(r.world.relations.group(target->group_id).alert ==
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
    const int instant_index = static_cast<int>(r.world.ammo.entries.size());
    r.world.ammo.entries.push_back(instant);
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
    const int charge_index = static_cast<int>(r.world.ammo.entries.size());
    r.world.ammo.entries.push_back(charge);
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
    std::vector<ProjectilePersonProxy> proxies{
        ProjectilePersonProxy{self_h, FixedVec3{2 * 65536, 0, 0}},
        ProjectilePersonProxy{local_l.packed, FixedVec3{5 * 65536, 0, 0}},
    };
    CollisionWorld collision;
    collision.replace_projectile_person_proxies(proxies, self_h);
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
    world.ammo.entries.push_back(ammo);
    world.mp_session = true;
    world.projectile_authority = false;

    RoundSpawnParams params;
    params.owner = local_l;
    params.shooter_handle = self_h;
    params.origin = {0.0f, 0.0f, kOrganicStandInCenterZ};
    params.ammo_index = 0;
    CHECK(world.round_sim.spawn(
              world, params, RoundConsequenceMode::VisualOnly) >= 0);
    world.round_sim.tick(world, nullptr, &collision);

    CHECK(world.round_sim.impacts.size() == 1);
    CHECK(world.round_sim.impacts[0].effect_tag == 2);
    CHECK(to_fixed(world.round_sim.impacts[0].position.x) == hit.position_q16.x);
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

// Moving decoded pool-1 movers collide through their wire-keyed authored
// geometry at the DECODED pose, while a visual client's load-frozen local
// pool-1 ghost stops serving projectile collision.
void test_visual_dynamic_proxy_projects_decoded_pose_geometry() {
    World world;
    world.registry.configure_pool(0, 8);
    world.registry.configure_pool(1, 8);
    world.mp_session = true;
    world.projectile_authority = false;

    Entity shooter;
    shooter.kind = EntityKind::Organic;
    shooter.item_type = 3;
    shooter.position = {0.0f, 0.0f, 0.0f};
    const EntityHandle sh = world.registry.spawn(0, shooter);
    world.cached.local_player = sh;

    // The load-frozen local ghost: the same vehicle the wire also carries, at
    // its authored spawn X=5 — the pose the host long since moved it away from.
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
    ProjectileDynamicProxy proxy;
    proxy.wire_handle = 0x1002; // host pool-1 slot 2
    proxy.model_id = model_id;
    proxy.position_q16 = FixedVec3{12 * 65536, 0, 0};
    proxy.bound_radius_q16 = 3 * 65536;
    collision.replace_projectile_dynamic_proxies({proxy});

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
    world.projectile_authority = true;
    CHECK(trace_down_at(5 * 65536, false).hit_class ==
          ProjectileHitClass::DynamicEntity);
    world.projectile_authority = false;

    // The decoded heading rotates the authored geometry with the visual: at
    // heading 90 deg the (+-1, +-1) quad still spans the section origin, but a
    // proxy REPOSED under pitch 90 deg turns the z=1 plane vertical and the
    // descending ray at its center now passes through where the flat plane
    // would have stopped it.
    ProjectileDynamicProxy pitched = proxy;
    pitched.pitch_bam = 0x40000000;
    collision.replace_projectile_dynamic_proxies({pitched});
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
// An unresolvable graphic keeps the bounded sphere stand-in.
void test_visual_dynamic_proxy_carrier_gate_and_sphere_standin() {
    World world;
    world.registry.configure_pool(0, 4);
    world.mp_session = true;
    world.projectile_authority = false;

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

    ProjectileDynamicProxy carrier;
    carrier.wire_handle = 0x1004;
    carrier.model_id = model_id;
    carrier.position_q16 = FixedVec3{6 * 65536, 0, 0};
    carrier.pitch_bam = 0x40000000; // vertical plane across the lane
    carrier.bound_radius_q16 = 3 * 65536;
    ProjectileDynamicProxy behind = carrier;
    behind.wire_handle = 0x1007;
    behind.position_q16 = FixedVec3{10 * 65536, 0, 0};
    collision.replace_projectile_dynamic_proxies({carrier, behind});

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

    // Unresolved graphic: the bounded compatibility sphere still stops rounds
    // (the D-ITEM-1 stand-in rule, applied wire-side).
    ProjectileDynamicProxy unresolved;
    unresolved.wire_handle = 0x1009;
    unresolved.model_id = -1;
    unresolved.position_q16 = FixedVec3{5 * 65536, 0, 0};
    unresolved.bound_radius_q16 = 0x18000; // 1.5 u
    collision.replace_projectile_dynamic_proxies({unresolved});
    const ProjectileHit sphere_hit = collision.trace_projectile(world, trace);
    CHECK(sphere_hit.hit_class == ProjectileHitClass::DynamicEntity);
    CHECK(!sphere_hit.geometry_entity.valid());
    CHECK(sphere_hit.position_q16.x <= 5 * 65536);
}

// A decoded vehicle/item shooter never clips its own wire slot: the dyn-proxy
// walk applies the same self-site immunity as the local tables (the retail
// four-slot ray[17..20] exclusion), and an unrelated proxy behind it still
// stops the round.
void test_visual_dynamic_proxy_excludes_shooter_self_slot() {
    World world;
    world.registry.configure_pool(0, 4);
    world.mp_session = true;
    world.projectile_authority = false;

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

    ProjectileDynamicProxy self;
    self.wire_handle = 0x1004; // the decoded shooter's own pool-1 slot
    self.model_id = model_id;
    self.position_q16 = FixedVec3{6 * 65536, 0, 0};
    self.pitch_bam = 0x40000000; // vertical plane across the lane
    self.bound_radius_q16 = 3 * 65536;
    ProjectileDynamicProxy behind = self;
    behind.wire_handle = 0x1007;
    behind.position_q16 = FixedVec3{10 * 65536, 0, 0};
    collision.replace_projectile_dynamic_proxies({self, behind});

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
    world.mp_session = true;
    world.projectile_authority = false;

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

    ProjectileDynamicProxy carrier;
    carrier.wire_handle = 0x1004; // the decoded shooter's own carrier
    carrier.model_id = model_id;
    carrier.position_q16 = FixedVec3{6 * 65536, 0, 0};
    carrier.pitch_bam = 0x40000000; // vertical plane across the lane
    carrier.bound_radius_q16 = 3 * 65536;
    ProjectileDynamicProxy target = carrier;
    target.wire_handle = 0x1007;
    target.position_q16 = FixedVec3{10 * 65536, 0, 0};
    collision.replace_projectile_dynamic_proxies({carrier, target});

    AmmoTableEntry nade;
    nade.name = "VISUAL_NADE";
    nade.valid = true;
    nade.flags = 0x2000u | 0x80u; // useownmove + nocollide-terrain
    nade.velocity = 124;          // 2 units/tick
    nade.max_age_ticks = 60;
    nade.drag_fp16 = 65536;       // dragless: keep the 2 u/tick lane speed
    const int nade_index = static_cast<int>(world.ammo.entries.size());
    world.ammo.entries.push_back(nade);

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
    world.mp_session = true;
    world.projectile_authority = false;

    Entity local;
    local.kind = EntityKind::Organic;
    local.item_type = 3;
    const EntityHandle local_l = world.registry.spawn(0, local);
    world.cached.local_player = local_l;

    // The load-frozen local mission AI at its authored spawn on the lane. On a
    // visual client this ghost no longer stops rounds; its live decoded proxy
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

    std::vector<ProjectilePersonProxy> proxies{
        ProjectilePersonProxy{ai_h.packed, FixedVec3{9 * 65536, 0, -58982}},
    };
    collision.replace_projectile_person_proxies(proxies, /*self H*/ 0x0002);

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
        world.mp_session = true;
        world.projectile_authority = false;

        // Load-frozen mission organics at local slots 0..3. A visual client
        // never collides them (their decoded proxies serve instead), but they
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
        std::vector<ProjectilePersonProxy> proxies{
            ProjectilePersonProxy{0x0002, FixedVec3{9 * 65536, 0, -58982}},
        };
        collision.replace_projectile_person_proxies(proxies, local_wire_h);

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
        r.world.ammo.entries[0].arm_age_ticks = 0;
        r.world.ammo.entries[0].penetration_impact = 10;
        Entity *target = r.world.registry.get(r.target);
        target->armor_impact = armor;
        target->damage_state = damage_state;
        CHECK(r.fire() >= 0);
        r.world.round_sim.tick(r.world, nullptr);
        CHECK(r.world.round_sim.impacts.size() == 1);
        CHECK(r.world.round_sim.active_count == 0);
        CHECK(target->armor_impact == expected_armor);
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

    AmmoTableEntry &ammo = r.world.ammo.entries[0];
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

void test_exact_one_hop_vehicle_parent_damage_routing() {
    const auto run_direct_case = [](uint8_t child_type, uint32_t child_attrib,
                                    uint8_t parent_type, bool expect_parent) {
        Rig r;
        r.world.ammo.entries[0].arm_age_ticks = 0;
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
            // not the vehicle that receives the routed damage.
            CHECK(r.world.round_sim.impacts[0].effect_tag == 2);
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
    r.world.ammo.entries[0].arm_age_ticks = 0;
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
    r.world.ammo.entries[0].arm_age_ticks = 0;
    r.world.ammo.entries[0].max_damage = 0;
    r.world.ammo.entries[0].weight_in_grains = 143; // floor(620*143/875) = 101

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
    child->mounted = true;            // the retail +40 attach the occupant scan reads
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
        CHECK(r.world.round_sim.impacts[0].effect_tag == 2);
    };
    // [orig: Entity_CountMountedEntities @ 0x435970] Occupants are counted by
    // the ATTACH parent, not by standing: candidate.mount_target == vehicle, or
    // the candidate's carrier stands on the vehicle (carrier.ground_target).
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
        return r.world.registry.spawn(pool, rider);
    };

    fire_expect(101); // the struck child is the sole direct occupant: no scale

    add_rider(vehicle_h);
    fire_expect(81); // count 2: trunc(101 * 0.20) = 20 reduction

    const EntityHandle nested = add_rider(r.target);
    fire_expect(76); // count 3: 0.30 capped to 0.25; trunc(25.25) = 25

    add_rider(vehicle_h, 0x2u);
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

    Entity deck_stander;
    deck_stander.kind = EntityKind::Organic;
    deck_stander.has_item_def = true;
    deck_stander.item_type = 3;
    deck_stander.position = {100.0f, 100.0f, 100.0f};
    deck_stander.health = 100;
    deck_stander.ground_target = vehicle_h; // standing on the deck, not attached
    CHECK(r.world.registry.spawn(0, deck_stander).valid());
    fire_expect(76); // retail counts the +40 attach chain, never plain standing

    add_rider(nested);
    fire_expect(76); // two intermediates deep is outside the retail count
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
    r.world.ammo.entries[0].arm_age_ticks = 0;
    const int slot = r.fire();
    CHECK(slot >= 0);
    if (slot < 0) return;
    r.world.round_sim.tick(r.world, nullptr);
    const LiveRound &round = r.world.round_sim.rounds[static_cast<size_t>(slot)];
    CHECK(!round.active);
    CHECK(to_fixed(round.vel.z) == 0); // no post-hit gravity or drag
}

} // namespace

int main() {
    test_arming_dud_and_armed_damage();
    test_missing_item_def_consumes_round_without_damage();
    test_damage_uses_retail_signed_wrap_and_ftol_cap();
    test_nodie_and_nontransparent_damage_gates();
    test_posed_head_zone_multiplier();
    test_item_type_zone_domain_and_attrib_0200_sections();
    test_shooter_damage_class_runs_after_zone_truncation();
    test_network_oneshot_authority_and_session_gate();
    test_visual_only_rounds_have_no_gameplay_consequences();
    test_visual_person_proxy_keeps_wire_identity_out_of_authority();
    test_visual_dynamic_proxy_projects_decoded_pose_geometry();
    test_visual_dynamic_proxy_carrier_gate_and_sphere_standin();
    test_visual_dynamic_proxy_excludes_shooter_self_slot();
    test_visual_throwable_motor_sweeps_wire_proxies();
    test_visual_infantry_proxy_joins_person_walk();
    test_person_walk_orders_local_player_by_its_server_handle();
    test_signed_armor_equality_and_damage_state_gates();
    test_signed_health_subtraction_wraps_at_entity_word();
    test_exact_one_hop_vehicle_parent_damage_routing();
    test_vehicle_occupant_reduction_count_cap_and_depth();
    test_retail_force_order_and_stock_gates();
    test_retail_aerodynamic_drag_vectors();
    test_consumed_hit_skips_post_sweep_forces();
    if (failures == 0) std::printf("projectile_combat_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
