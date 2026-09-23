// The SM target feed and its engage bookkeeping against the retail witnesses:
// the class walk's candidate facts [orig: AI_FindBestTargetB @0x466F60] and
// its variant A [orig: AI_FindBestTarget @0x465A50], the state-16 engage
// [orig: AI_HandleEvent_HelicopterCombatD @0x467730], the weapon validator's
// head [orig: Entity_ValidateWeaponTarget @0x53A400] and the fired mark every
// spawned round leaves on its shooter [orig: RoundData_SpawnRound @0x4EC0D0].
#include <cstdio>
#include <memory>

#include <base/gameprofile/game_type.h>
#include <formats/def/def.h>
#include <runtime/world/ai.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/entity_commands.h>
#include <runtime/world/match.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

// One SM ground brain (pool 1, team 1) at the origin facing +X, with wide
// arcs, 1000 u engage ranges and a nonzero engage fire delay.
struct Scanner {
    std::unique_ptr<World> w = std::make_unique<World>();
    int index = -1;

    AiEntity &e() { return *w->ai.at(index); }

    Scanner() {
        w->registry.configure_pool(0, 8);
        w->registry.configure_pool(1, 8);
        Entity self{};
        self.alive = true;
        self.health = 100;
        self.team = 1;
        const EntityHandle h = w->registry.spawn(1, self);
        w->ai.is_authority = true;
        index = w->ai.attach(h);
        AiEntity &ai = e();
        ai.team = 1;
        ai.heading = 0;
        ai.profile.fov_primary = 0xFF;
        ai.profile.fov_secondary = 0xFF;
        ai.profile.range_primary = 1000;
        ai.profile.range_secondary = 1000;
        ai.profile.field104 = 31;
    }

    // A live team-2 candidate 20 u ahead, inside both of its own signature caps.
    EntityHandle spawn_enemy(int pool, uint32_t engine_flags, uint32_t owner_connection_id) {
        Entity seed{};
        seed.alive = true;
        seed.health = 100;
        seed.team = 2;
        seed.radar_sig = 1000;
        seed.heat_sig = 1000;
        seed.engine_flags = engine_flags;
        seed.owner_connection_id = owner_connection_id;
        seed.position = Vec3{20.0f, 0.0f, 0.0f};
        return w->registry.spawn(pool, seed);
    }
};

} // namespace

// R2-6: the engage picks its fire-delay jitter by the target's SM brain pointer
// (entity+0x64), not by a network owner. A brained vehicle takes the guarded
// inline-LCG branch A (dword_31BFBB8); a brainless remote player takes the
// PRNG_Next16 branch B, whatever its connection.
// [orig: AI_HandleEvent_HelicopterCombatD `cmp [ebx+64h],ebp; jz loc_4678C2`
//  @0x4677EA..0x4677EF; branch A @0x467803..0x467866; branch B
//  `call PRNG_Next16` @0x4678D9]
static void test_engage_jitter_keys_on_the_target_brain() {
    for (const bool brained : {true, false}) {
        Scanner s;
        World &w = *s.w;
        EntityHandle target;
        if (brained) {
            // A crewless SM vehicle: class 1 walks pool 1's non-helo brains.
            s.e().profile.class_priority[1] = 1;
            target = s.spawn_enemy(1, 0, 0);
            w.ai.attach(target); // an SM brain, profile type 0
        } else {
            // A remote player's body: class 0 ends on pool 0's Player-flagged rows.
            s.e().profile.class_priority[0] = 1;
            target = s.spawn_enemy(0, kEntityFlagPlayer, 3);
        }
        AiTarget found{};
        CHECK(w.ai.acquire_target(w, s.e(), found));
        CHECK(found.handle == target);
        CHECK(found.has_brain == brained);
        const uint32_t stream_a = w.ai.prng_a;
        const uint32_t stream_b = w.prng16_state;
        w.ai.engage_target(w, s.e(), found);
        CHECK((w.ai.prng_a != stream_a) == brained);
        CHECK((w.prng16_state != stream_b) == !brained);
        CHECK(s.e().brain.f[AiBrain::kFireDelay] >= 31);
    }
}

// R2-10: every non-silenced round a shooter spawns marks it with Flags
// 0x4000, the SM scan's x6 priority weight, players included; a SILENCED
// (ammo flag 8) round leaves no mark, and the AI fire path rides the same
// spawn. [orig: RoundData_SpawnRound `test byte ptr [edi],8; jnz; or dword
//  ptr [ebp+24h],4000h` @0x4EC842..0x4EC847; the shotgun fan
//  Weapon_SpawnProjectileBurstWithSpread @0x4EBE61..0x4EBE66]
static void test_rounds_mark_their_shooter() {
    auto owned = std::make_unique<World>();
    World &w = *owned;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    w.tables.ammo.entries.resize(4);
    for (int i = 1; i <= 3; ++i) {
        w.tables.ammo.entries[i].valid = true;
        w.tables.ammo.entries[i].velocity = 620;
        w.tables.ammo.entries[i].max_age_ticks = 100;
    }
    w.tables.ammo.entries[2].flags = opennova::def::DEF_AMMO_FLAG_SILENCED;
    w.tables.ammo.entries[3].flags = kAmmoFlagShotgun;
    w.tables.ammo.entries[3].spread_count = 4;
    Entity player{};
    player.alive = true;
    player.health = 100;
    player.engine_flags = kEntityFlagPlayer;
    const EntityHandle h = w.registry.spawn(0, player);
    const auto marked_after = [&](int ammo) {
        Entity &p = *w.registry.get(h);
        p.flags &= ~kEntityFlagPriorityTarget;
        p.engine_flags &= ~kEntityFlagPriorityTarget;
        RoundSpawnParams rp;
        rp.owner = h;
        rp.shooter_handle = h.packed;
        rp.origin = Vec3{0.0f, 0.0f, 1.0f};
        rp.ammo_index = ammo;
        CHECK(w.round_sim.spawn(w, rp) >= 0);
        return (w.registry.get(h)->engine_flags & kEntityFlagPriorityTarget) != 0;
    };
    CHECK(marked_after(1));  // a ballistic round
    CHECK(!marked_after(2)); // a silenced round
    CHECK(marked_after(3));  // the shotgun fan

    // The SM fire path: a silenced block leaves its gun unmarked.
    Entity gun{};
    gun.alive = true;
    gun.health = 100;
    const EntityHandle gun_h = w.registry.spawn(1, gun);
    AiEntity &sm = *w.ai.at(w.ai.attach(gun_h));
    const int32_t origin[3] = {0, 0, 1 << 16};
    CHECK(w.ai.fire_ai_round(w, sm, origin, 0, 0, 2));
    CHECK((w.registry.get(gun_h)->engine_flags & kEntityFlagPriorityTarget) == 0);
    CHECK(w.ai.fire_ai_round(w, sm, origin, 0, 0, 1));
    CHECK((w.registry.get(gun_h)->engine_flags & kEntityFlagPriorityTarget) != 0);
}

// R2-11: BERSERK is read live from AiSlot[1] & 0x200, so a scripted ChangeAI
// Berserk (which writes only the slot word) opens the SM feed to a same-team
// candidate. The kill feed's event type reads the same bit, but the scorer
// does not: a same-team kill stays a team kill.
// [orig: AI_FindBestTargetB `test dword ptr [eax+4],200h` @0x466FAB,
//  @0x4670CD, @0x4670E1; Entity_ApplyCommand case 0x10 @0x43AEF6;
//  GameEvent_PlayerDeath feed gate @0x5170BE..0x5170DA]
static void test_scripted_berserk_reaches_the_readers() {
    {
        Scanner s;
        World &w = *s.w;
        s.e().profile.class_priority[1] = 1;
        const EntityHandle ally = s.spawn_enemy(1, 0, 0);
        w.registry.get(ally)->team = 1; // the scanner's own team
        AiTarget found{};
        CHECK(!w.ai.acquire_target(w, s.e(), found));
        const EntityHandle self = s.e().handle;
        CHECK(w.commands.apply_ai_command(self, EntityCommands::kBerserkBit, 1, 0, 0));
        CHECK(w.ai.acquire_target(w, s.e(), found));
        CHECK(found.handle == ally);
        CHECK(w.commands.apply_ai_command(self, EntityCommands::kBerserkBit, 0, 0, 0));
        CHECK(!w.ai.acquire_target(w, s.e(), found));
    }
    {
        auto owned = std::make_unique<World>();
        World &w = *owned;
        w.registry.configure_pool(0, 4);
        MatchRules rules;
        rules.game_type = opennova::game_type::kTeamDeathmatch;
        w.match.configure(rules);
        const auto spawn_player = [&w](uint8_t slot) {
            Entity seed{};
            seed.kind = EntityKind::Organic;
            seed.alive = true;
            seed.health = 100;
            seed.team = 1;
            seed.flags = kEntityFlagPlayer;
            seed.engine_flags = kEntityFlagPlayer;
            const EntityHandle h = w.registry.spawn(0, seed);
            MatchPlayerIdentity identity;
            identity.entity = h;
            identity.slot = slot;
            w.match.upsert_player(identity);
            return h;
        };
        const EntityHandle killer = spawn_player(0);
        const EntityHandle victim = spawn_player(1);
        w.ai.attach(killer);
        CHECK(w.commands.apply_ai_command(killer, EntityCommands::kBerserkBit, 1, 0, 0));
        w.match.record_death(w, victim, killer);
        // The scorer itself has no see-all exemption: a same-team kill stays a
        // team kill; BERSERK only changes the kill feed's event type.
        // [orig: GameEvent_ProcessScoring @0x5301E4..0x5301EE; the feed-only
        //  test GameEvent_PlayerDeath @0x5170A6..0x517113]
        CHECK(w.match.player(killer)->stats[MatchStats::kTeamKills] == 1);
        CHECK(w.match.player(killer)->stats[MatchStats::kEnemyKills] == 0);
    }
}

// R2-13: the aircraft sites search with AI_FindBestTarget (variant A), which
// reads both arc bytes signed: a byte of 0x80 or more goes negative, so the
// unsigned arc gate admits every bearing while the unsigned divide scores 0
// and the brain never engages. The ground feed (variant B) zero-extends the
// same byte and takes the candidate. [orig: AI_FindBestTarget `movsx`
//  @0x465A8C / @0x465A9B against AI_FindBestTargetB `movzx` @0x466FBC /
//  @0x466FCB; AI_HandleEvent_VehicleWithDamageC @0x466460 (the AI_FindBestTarget
//  call @0x46648F)]
static void test_helo_search_reads_the_arc_signed() {
    for (const bool helo : {false, true}) {
        Scanner s;
        World &w = *s.w;
        AiEntity &e = s.e();
        e.profile.fov_primary = 0xC0; // a 270 deg arc
        e.profile.fov_secondary = 0xC0;
        e.profile.class_priority[1] = 1;
        e.profile.type = helo ? 1 : 2;
        const int32_t state = helo ? kAiHeloFollowWp : kAiGroundFollowWp;
        e.brain.f[AiBrain::kCurState] = state;
        e.brain.f[AiBrain::kPendState] = state;
        const EntityHandle target = s.spawn_enemy(1, 0, 0);
        w.ai.attach(target); // a ground SM brain: class 1 walks it
        AiThinkCtx ctx{&w.ai, &s.e(), &w, nullptr};
        w.ai.row(state).tick(ctx);
        AiBrain &b = s.e().brain;
        if (helo) {
            CHECK(b.f[AiBrain::kPendState] == kAiHeloFollowWp);
            CHECK(b.f[AiBrain::kTargetSlot] == 0);
        } else {
            CHECK(b.f[AiBrain::kPendState] == kAiGroundCombat);
            CHECK(b.f[AiBrain::kTargetSlot] == static_cast<int32_t>(target.packed) + 1);
        }
    }
}

// R2-14: the validator keeps a destroyed (Flags & 2) or dead target for the
// 16 ticks after its death tick only, and rejects a Player target while the
// local cheat word's 0x800 bit is up.
// [orig: Entity_ValidateWeaponTarget `test cl,2; jnz` @0x53A425..0x53A428,
//  the window @0x53A434..0x53A443; `test dword_24C1930,800h`
//  @0x53A46E..0x53A478]
static void test_validator_gates_destroyed_and_cheat_targets() {
    Scanner s;
    World &w = *s.w;
    s.e().profile.radar_fov_bam = INT32_MAX;
    s.e().profile.approach_cap = 1000 << 16;
    const EntityHandle t = s.spawn_enemy(1, 0, 0);
    Entity &target = *w.registry.get(t);
    const int32_t pose[6] = {};
    int32_t metrics[6];
    const auto valid = [&]() {
        return w.ai.weapon_target_metrics(w, s.e(), target, pose, 0, /*skip_los=*/true, metrics);
    };
    w.logic_tick = 100;
    CHECK(valid());
    target.engine_flags |= kEntityFlagDead; // destroyed, health still positive
    target.death_tick = 90;
    CHECK(valid()); // inside the 16-tick window
    target.death_tick = 80;
    CHECK(!valid());
    target.engine_flags &= ~kEntityFlagDead;
    target.engine_flags |= kEntityFlagPlayer;
    CHECK(valid());
    w.rules.ai_rules_skip_local_player = true;
    CHECK(!valid());
}

int main() {
    test_engage_jitter_keys_on_the_target_brain();
    test_rounds_mark_their_shooter();
    test_scripted_berserk_reaches_the_readers();
    test_helo_search_reads_the_arc_signed();
    test_validator_gates_destroyed_and_cheat_targets();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ai_targeting: all passed\n");
    return 0;
}
