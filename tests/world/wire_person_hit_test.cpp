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

#include <runtime/world/ammo_table.h>
#include <runtime/world/collision.h>
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
