// The client leg of the projectile damage pass over a wire-built person: a
// joiner's round striking a remote person it keeps as a replica row runs
// Projectile_ProcessDamageOnTarget's non-authority leg over that row's body
// (world/wire_person_hit.h) -- the friendly-fire gate, the hit record, the
// attacker stamp -- through RoundSim's public seam and a stand-in for the
// joiner role's row store.
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <runtime/replication/client_state.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
#include <runtime/world/infantry.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/wire_person_hit.h>
#include <runtime/world/world.h>

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

constexpr uint16_t kSelfH = 0x0000;   // the joiner's own wire identity
constexpr uint16_t kRemoteH = 0x0003; // a remote person row
constexpr int32_t kQ = 65536;

// A non-authority session world: the joiner's own body L, one remote person
// row at x = 5 in the collision world's person walk, and a row store the
// round sim's wire_person_body seam loads and stores, as the joiner role's.
struct Joiner {
    std::unique_ptr<World> owner = std::make_unique<World>();
    World &world = *owner;
    CollisionWorld collision;
    EntityHandle local;
    std::map<uint16_t, WirePersonBody> rows;
    int visits = 0;

    explicit Joiner(uint8_t self_team) {
        world.registry.configure_pool(0, 8);
        world.rules.mp_session = true;
        world.rules.projectile_authority = false;
        world.rules.logic_authority = false;
        Entity self;
        self.kind = EntityKind::Organic;
        self.item_type_index = 3;
        self.has_item_def = true;
        self.item_type = 3;
        self.team = self_team;
        self.flags = kEntityFlagPlayer;
        self.group_id = 1; // as world::spawn_player builds the joiner's body
        self.position = {-50.0f, 0.0f, 0.0f}; // out of the round's path
        self.health = 100;
        local = world.registry.spawn(0, self);
        world.cached.local_player = local;

        WirePersonCollisionProxy proxy;
        proxy.wire_handle = kRemoteH;
        proxy.position_q16 = FixedVec3{5 * kQ, 0, 0};
        collision.replace_wire_collision_proxies({proxy}, {}, kSelfH);
        collision.build_tick_tables(world);
        world.collision = &collision;

        AmmoTableEntry ammo;
        ammo.name = "PROXY_ROUND";
        ammo.valid = true;
        ammo.velocity = 620; // 10 units a tick
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        ammo.max_damage = 25;
        world.tables.ammo.entries.push_back(ammo);

        world.round_sim.wire_person_body = [this](uint16_t handle,
                const std::function<void(WirePersonBody &)> &visit) {
            const auto it = rows.find(handle);
            if (it == rows.end()) return false;
            ++visits;
            WirePersonBody body = it->second;
            visit(body);
            it->second = body;
            return true;
        };
    }

    WirePersonBody &remote(uint8_t team, uint32_t flags = kEntityFlagPlayer) {
        WirePersonBody &body = rows[kRemoteH];
        body.wire_handle = kRemoteH;
        body.has_item_def = true;
        body.team = team;
        body.flags = flags;
        return body;
    }

    // The joiner's own round along +x at the stand-in's centre height.
    void fire() {
        RoundSpawnParams params;
        params.owner = local;
        params.shooter_handle = kSelfH;
        params.origin = {0.0f, 0.0f, kOrganicStandInCenterZ};
        params.ammo_index = 0;
        CHECK(world.round_sim.spawn(world, params, RoundConsequenceMode::VisualOnly) >= 0);
        world.round_sim.tick(world, nullptr, &collision);
    }
};

// The round lands on the row: the hit record carries the round, a zero
// damage, the torso stand-in's section 1 and the row; the row takes the
// attacker stamp; nothing authoritative moves.
// [orig: Projectile_ProcessDamageOnTarget @0x4E81BA..0x4E81F9;
//  Weapon_CalcImpactDamage @0x4EC933..0x4EC93A]
void test_hit_records_and_stamps_the_row() {
    Joiner j(/*self_team=*/1);
    j.remote(/*team=*/2);
    j.world.round_sim.hit_record.section = 77; // a stale record
    j.fire();
    const HitRecord &record = j.world.round_sim.hit_record;
    CHECK(record.has_round);
    CHECK(record.damage == 0);
    CHECK(record.section == 1);
    CHECK(record.target_wire == kRemoteH);
    CHECK(record.round_shooter_wire == kSelfH);
    CHECK(record.round_owner == j.local);
    CHECK(record.round_vel_q16.x > 0);
    const WirePersonBody &row = j.rows[kRemoteH];
    CHECK(row.last_attacker == kSelfH);
    CHECK(row.last_attacker_ammo == 0);
    CHECK(j.world.round_sim.hits.empty());
    CHECK(j.world.round_sim.deaths.empty());
    CHECK(j.world.registry.get(j.local)->health == 100);
}

// A same-team pair under NoFriendlyFire is protected: the pass leaves before
// the record and the stamp [orig: @0x4E8081..0x4E808F ->
// Projectile_DamagePairEligible @0x4E74F0, the rule @0x4E7516, the team bytes
// @0x4E7528].
void test_protected_pair_lands_nothing() {
    Joiner j(/*self_team=*/1);
    j.world.rules.no_friendly_fire = true;
    j.remote(/*team=*/1);
    j.fire();
    CHECK(!j.world.round_sim.hit_record.has_round);
    CHECK(j.rows[kRemoteH].last_attacker == 0xFFFF);
    CHECK(j.rows[kRemoteH].last_attacker_ammo == -1);
}

// Without the rule a same-team pair is protected only when the shooter lacks
// the Player bit; the joiner's own body carries it, so the hit lands. Either
// side's BERSERK bit unprotects [orig: @0x4E7516, @0x4E753A / @0x4E754C].
void test_player_shooter_and_berserk_unprotect() {
    {
        Joiner j(/*self_team=*/1);
        j.remote(/*team=*/1);
        j.fire();
        CHECK(j.world.round_sim.hit_record.has_round);
        CHECK(j.rows[kRemoteH].last_attacker == kSelfH);
    }
    {
        Joiner j(/*self_team=*/1);
        j.world.registry.get(j.local)->flags = 0; // a shooter without the Player bit
        j.world.registry.get(j.local)->engine_flags = 0;
        j.remote(/*team=*/1);
        j.fire();
        CHECK(!j.world.round_sim.hit_record.has_round);
    }
    {
        Joiner j(/*self_team=*/1);
        j.world.registry.get(j.local)->flags = 0;
        j.world.registry.get(j.local)->engine_flags = 0;
        j.remote(/*team=*/1).berserk = true;
        j.fire();
        CHECK(j.world.round_sim.hit_record.has_round);
    }
}

// The EWEAP def skips the stamp, not the record [orig: @0x4E81E3..0x4E81E7];
// a row without a def returns at the pass's head [orig: @0x4E7FCB]; a zeroed
// NoDie body records the clamp's -1 [orig: @0x4E8059..0x4E8076].
void test_def_gates() {
    {
        Joiner j(/*self_team=*/1);
        WirePersonBody &row = j.remote(/*team=*/2);
        row.item_attrib = kItemAttribNoDie;
        row.health_zero = true;
        j.fire();
        CHECK(j.world.round_sim.hit_record.has_round);
        CHECK(j.world.round_sim.hit_record.damage == -1);
    }
    {
        Joiner j(/*self_team=*/1);
        j.remote(/*team=*/2).item_attrib = kItemAttribNoDie;
        j.fire();
        CHECK(j.world.round_sim.hit_record.damage == 0);
    }
    {
        Joiner j(/*self_team=*/1);
        j.remote(/*team=*/2).item_attrib = kItemAttribEweap;
        j.fire();
        CHECK(j.world.round_sim.hit_record.has_round);
        CHECK(j.rows[kRemoteH].last_attacker == 0xFFFF);
    }
    {
        Joiner j(/*self_team=*/1);
        j.remote(/*team=*/2).has_item_def = false;
        j.fire();
        CHECK(!j.world.round_sim.hit_record.has_round);
        CHECK(j.rows[kRemoteH].last_attacker == 0xFFFF);
    }
}

// A relayed round's owner is the decoded shooter's row: the gate reads that
// row's team and Player bit, and the stamp names it.
void test_relayed_round_reads_the_shooter_row() {
    constexpr uint16_t kShooterH = 0x0005;
    Joiner j(/*self_team=*/2);
    j.world.rules.no_friendly_fire = true;
    j.remote(/*team=*/1);
    WirePersonBody &shooter = j.rows[kShooterH];
    shooter.wire_handle = kShooterH;
    shooter.has_item_def = true;
    shooter.team = 1;
    shooter.flags = kEntityFlagPlayer;
    RoundSpawnParams params;
    params.shooter_handle = kShooterH;
    params.origin = {0.0f, 0.0f, kOrganicStandInCenterZ};
    params.ammo_index = 0;
    CHECK(j.world.round_sim.spawn(j.world, params, RoundConsequenceMode::VisualOnly) >= 0);
    j.world.round_sim.tick(j.world, nullptr, &j.collision);
    CHECK(!j.world.round_sim.hit_record.has_round); // same team under the rule

    j.rows[kShooterH].team = 2;
    CHECK(j.world.round_sim.spawn(j.world, params, RoundConsequenceMode::VisualOnly) >= 0);
    j.world.round_sim.tick(j.world, nullptr, &j.collision);
    CHECK(j.world.round_sim.hit_record.has_round);
    CHECK(j.world.round_sim.hit_record.round_shooter_wire == kShooterH);
    CHECK(!j.world.round_sim.hit_record.round_owner.valid());
    CHECK(j.rows[kRemoteH].last_attacker == kShooterH);
}

// The org class callback's event 1 over an infantry row (no Player bit, not
// the plyr row): the alert, then the round legs. The round flies +x into a
// body facing back along it (heading 0x7F000000), the front quadrant.
// [orig: OrganicClass_HandleEvent @0x4073BF..0x4076D5]
void test_org_callback_alerts_and_runs_the_round_legs() {
    Joiner j(/*self_team=*/1);
    WirePersonBody &row = j.remote(/*team=*/2, /*flags=*/0);
    row.heading_bam = 0x7F000000; // facing back along the round, front quadrant
    row.pos[0] = 5 * kQ;
    AmmoTableEntry &ammo = j.world.tables.ammo.entries[0];
    ammo.secondary_anim = 2; // the hit type
    ammo.kz_physics = 1;     // the walk push
    j.fire();
    const WirePersonBody &body = j.rows[kRemoteH];
    // The alert [orig: @0x4073DB; TriggerGroup_SetAlertRed @0x4073EA].
    CHECK(body.ai_alert == 2);
    CHECK(j.world.script.relations.group(0).alert == TriggerRelations::kAlertRed);
    // The death clip from section 1, the front quadrant, cause 1 [orig: @0x407483].
    CHECK(body.death_anim_state == compute_death_anim_state(1, 0, death_cause::kBullet));
    // The hit type and moveTimer: the round's owner is client-built, its
    // commandGroup 0 like the row's (L's world group_id 1 notwithstanding), so
    // the same-group duration (2 * 4) >> 2 = 2, the source lying behind the
    // body's yaw [orig: @0x4AF506, @0x4AF4E7, @0x4AF530].
    CHECK(body.hit_type == 2);
    CHECK(body.move_timer == 2);
    // The walk push: a live body's slideDecay +5120 and a planar kick away
    // from the source (+x) [orig: @0x4AF5E0..0x4AF602].
    CHECK(body.vel[2] == 5120);
    CHECK(body.vel[0] > 0);
    // The torso-stack roll, front quadrant [orig: @0x407564].
    CHECK(body.body_roll == 0x05B05B00);
    // Entity_OnDamageReceived's tail [orig: @0x4AF866..0x4AF878].
    CHECK(body.was_hit);
    CHECK(body.damage_timer == 10);
    CHECK(body.damage_attacker == kSelfH);
    // No hit emitter on an ammo without one.
    CHECK(j.world.out.destruction.effects.empty());
    // The rear quadrant rolls the other way [orig: @0x407575].
    Joiner k(/*self_team=*/1);
    k.remote(/*team=*/2, /*flags=*/0).heading_bam = 0;
    k.fire();
    CHECK(k.rows[kRemoteH].body_roll == -0x05B05B00);
    CHECK(k.rows[kRemoteH].death_anim_state ==
          compute_death_anim_state(1, 2, death_cause::kBullet));
}

// A relayed round from a non-person shooter row (a vehicle gun): the owner is
// the row's client-built entity, commandGroup 0, so the same-group duration.
void test_vehicle_shooter_owner_group() {
    static constexpr uint16_t kVehicleH = 0x1004;
    Joiner j(/*self_team=*/1);
    WirePersonBody &row = j.remote(/*team=*/2, /*flags=*/0);
    row.heading_bam = 0x7F000000;
    row.pos[0] = 5 * kQ;
    j.world.tables.ammo.entries[0].secondary_anim = 2;
    j.world.round_sim.wire_actor_provider = [](uint16_t handle, RoundSim::WireActor &out) {
        out = RoundSim::WireActor{};
        if (handle != kVehicleH) return false;
        out.team = 1;
        return true;
    };
    RoundSpawnParams params;
    params.shooter_handle = kVehicleH;
    params.origin = {0.0f, 0.0f, kOrganicStandInCenterZ};
    params.ammo_index = 0;
    CHECK(j.world.round_sim.spawn(j.world, params, RoundConsequenceMode::VisualOnly) >= 0);
    j.world.round_sim.tick(j.world, nullptr, &j.collision);
    CHECK(j.rows[kRemoteH].hit_type == 2);
    CHECK(j.rows[kRemoteH].move_timer == 2);
    CHECK(j.rows[kRemoteH].last_attacker == kVehicleH);
}

// A dead-flagged org body still takes the callback (the org row has no dead
// return): the alert and the clip, the force's smaller dead constants, but no
// hit-type stamp and no roll [orig: @0x4AF4FF, @0x4AF5A9, @0x40755E].
void test_org_callback_on_a_dead_body() {
    Joiner j(/*self_team=*/1);
    WirePersonBody &row = j.remote(/*team=*/2, /*flags=*/kEntityFlagDead);
    row.heading_bam = 0x7F000000;
    AmmoTableEntry &ammo = j.world.tables.ammo.entries[0];
    ammo.secondary_anim = 2;
    ammo.kz_physics = 1;
    j.fire();
    const WirePersonBody &body = j.rows[kRemoteH];
    CHECK(body.ai_alert == 2);
    CHECK(body.hit_type == 0);
    CHECK(body.move_timer == 0);
    CHECK(body.vel[2] == 2560);
    CHECK(body.body_roll == 0);
    CHECK(body.was_hit);
}

// The ammo's hit emitter: the body's +0x1CC slot is released, the effect
// spawns at the body tagged with it, oriented (0, 0, -0.5), and the clip
// becomes 173 [orig: @0x4074C7..0x40754A, @0x4076D3..0x4076D5].
void test_org_callback_hit_emitter() {
    Joiner j(/*self_team=*/1);
    j.remote(/*team=*/2, /*flags=*/0).pos[0] = 5 * kQ;
    j.world.tables.ammo.entries[0].secondary_effect = "Effect_Burn";
    j.fire();
    const auto &effects = j.world.out.destruction.effects;
    CHECK(effects.size() == 2);
    if (effects.size() == 2) {
        CHECK(effects[0].release);
        CHECK(effects[0].family == 1);
        CHECK(effects[0].attach_wire_handle == kRemoteH);
        CHECK(!effects[1].release);
        CHECK(effects[1].effect == "Effect_Burn");
        CHECK(effects[1].family == 1);
        CHECK(effects[1].attach_wire_handle == kRemoteH);
        CHECK(effects[1].section_tagged);
        CHECK(effects[1].positioned);
        CHECK(effects[1].pos.x == 5.0f);
        CHECK(effects[1].dir.z == -0.5f);
    }
    CHECK(j.rows[kRemoteH].death_anim_state == 173);
}

// The plyr class callback's event 1 over a remote player row: no alert for a
// Player body, the round legs, the think re-arm; a row whose Health is gone
// restages its death clip as the current state with no pending one, and the
// detach sends nothing for a body that is not the local player's.
// [orig: Entity_HandleDamageAndTriggerZones @0x407742..0x407A86, the tail
//  @0x407ACD..0x407B01, @0x407B5E; Entity_SendDetachPacket @0x43552A]
void test_plyr_callback_round_legs_and_restage() {
    {
        Joiner j(/*self_team=*/1);
        WirePersonBody &row = j.remote(/*team=*/2);
        row.plyr_class = true;
        row.heading_bam = 0x7F000000;
        row.anim_current = 43;
        row.anim_pending = 44;
        j.fire();
        const WirePersonBody &body = j.rows[kRemoteH];
        CHECK(body.ai_alert == 0);
        CHECK(j.world.script.relations.group(0).alert != TriggerRelations::kAlertRed);
        CHECK(body.death_anim_state == compute_death_anim_state(1, 0, death_cause::kBullet));
        CHECK(body.body_roll == 0x05B05B00);
        CHECK(body.was_hit);
        CHECK(body.spawn_phase == 64);
        // Health is up: no restage.
        CHECK(body.anim_current == 43);
        CHECK(body.anim_pending == 44);
    }
    {
        Joiner j(/*self_team=*/1);
        WirePersonBody &row = j.remote(/*team=*/2);
        row.plyr_class = true;
        row.health_zero = true;
        row.anim_current = 43;
        row.anim_pending = 44;
        j.fire();
        const WirePersonBody &body = j.rows[kRemoteH];
        CHECK(body.death_anim_state != 0);
        CHECK(body.anim_current == body.death_anim_state);
        CHECK(body.anim_pending == 0);
    }
}

// A dead-flagged plyr body returns at the callback's head: the record and the
// stamp land, the callback writes nothing [orig: @0x40772F].
void test_plyr_callback_dead_return() {
    Joiner j(/*self_team=*/1);
    WirePersonBody &row = j.remote(/*team=*/2, kEntityFlagPlayer | kEntityFlagDead);
    row.plyr_class = true;
    j.fire();
    const WirePersonBody &body = j.rows[kRemoteH];
    CHECK(j.world.round_sim.hit_record.has_round);
    CHECK(body.last_attacker == kSelfH);
    CHECK(body.death_anim_state == 0);
    CHECK(!body.was_hit);
    CHECK(body.spawn_phase == 0);
}

// A plyr row without the Player bit takes the org alert [orig: @0x407755..0x40776D].
void test_plyr_callback_without_the_player_bit_alerts() {
    Joiner j(/*self_team=*/1);
    WirePersonBody &row = j.remote(/*team=*/2, /*flags=*/0);
    row.plyr_class = true;
    j.fire();
    CHECK(j.rows[kRemoteH].ai_alert == 2);
    CHECK(j.world.script.relations.group(0).alert == TriggerRelations::kAlertRed);
}

// The row store: every field the leg writes lands on the replica row's own
// field, the ones the row already kept (the velocity, moveTimer, the death
// clip, the body states) included.
void test_row_body_round_trip() {
    opennova::replication::ClientEntityState row;
    row.handle = kRemoteH;
    row.team = 2;
    row.rm_entity_flags = kEntityFlagPlayer;
    row.x = 3 * kQ;
    row.heading_bam = 0x40000000;
    row.net_health_zero = true;
    row.net_anim_pending_boundary = 7;
    WirePersonBody body = opennova::replication::wire_person_body_of(
            row, /*has_item_def=*/true, /*item_attrib=*/0x20u, /*plyr_class=*/true);
    CHECK(body.wire_handle == kRemoteH && body.team == 2 && body.has_item_def);
    CHECK(body.item_attrib == 0x20u && body.plyr_class && body.health_zero);
    CHECK(body.flags == kEntityFlagPlayer && body.pos[0] == 3 * kQ);
    CHECK(body.heading_bam == 0x40000000);
    body.last_attacker = 9;
    body.last_attacker_ammo = 4;
    body.ai_alert = 2;
    body.death_anim_state = 120;
    body.anim_current = 120;
    body.anim_pending = 0;
    body.hit_type = 3;
    body.move_timer = 12;
    body.vel[0] = 1;
    body.vel[1] = 2;
    body.vel[2] = 3;
    body.body_roll = 0x05B05B00;
    body.was_hit = true;
    body.damage_timer = 10;
    body.damage_attacker = 9;
    body.spawn_phase = 64;
    opennova::replication::store_wire_person_body(body, row);
    CHECK(row.last_attacker == 9 && row.last_attacker_ammo == 4 && row.ai_alert == 2);
    CHECK(row.net_death_anim == 120 && row.net_anim_current == 120 && row.net_anim_pending == 0);
    CHECK(row.net_anim_pending_boundary == -1);
    CHECK(row.hit_type == 3 && row.net_corpse_timer == 12);
    CHECK(row.rm_vel_xy[0] == 1 && row.rm_vel_xy[1] == 2 && row.rm_vel_z == 3);
    CHECK(row.rm_body_roll == 0x05B05B00);
    CHECK(row.was_hit && row.damage_timer == 10 && row.damage_attacker == 9);
    CHECK(row.spawn_phase == 64);
}

struct NamedTest {
    const char *name;
    void (*fn)();
};

const NamedTest kTests[] = {
    {"test_hit_records_and_stamps_the_row", test_hit_records_and_stamps_the_row},
    {"test_protected_pair_lands_nothing", test_protected_pair_lands_nothing},
    {"test_player_shooter_and_berserk_unprotect", test_player_shooter_and_berserk_unprotect},
    {"test_def_gates", test_def_gates},
    {"test_relayed_round_reads_the_shooter_row", test_relayed_round_reads_the_shooter_row},
    {"test_org_callback_alerts_and_runs_the_round_legs", test_org_callback_alerts_and_runs_the_round_legs},
    {"test_org_callback_on_a_dead_body", test_org_callback_on_a_dead_body},
    {"test_vehicle_shooter_owner_group", test_vehicle_shooter_owner_group},
    {"test_org_callback_hit_emitter", test_org_callback_hit_emitter},
    {"test_row_body_round_trip", test_row_body_round_trip},
    {"test_plyr_callback_round_legs_and_restage", test_plyr_callback_round_legs_and_restage},
    {"test_plyr_callback_dead_return", test_plyr_callback_dead_return},
    {"test_plyr_callback_without_the_player_bit_alerts", test_plyr_callback_without_the_player_bit_alerts},
};

} // namespace

int main(int argc, char **argv) {
    const char *only = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--test") == 0 && i + 1 < argc) only = argv[++i];
    for (const NamedTest &t : kTests) {
        if (only != nullptr && std::strcmp(only, t.name) != 0) continue;
        t.fn();
    }
    if (failures == 0) std::printf("wire_person_hit_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
