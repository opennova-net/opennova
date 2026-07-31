// AI subsystem foundation tests: state enum, struct layout, AIEvent ring, the
// AI_BeginUpdate budget gate, the infantry state-machine dispatcher + transitions,
// and the byte-exact trivial handler ports. Driven by a manual World + tick.
#include <cstdio>
#include <memory>
#include <cstring>

#include "world/ai.h"
#include "world/body_anim.h"
#include "world/world.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool streq(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

// The vehicle death chain rows (world-wac-ai-re section 19) - own function per the
// main()-frame __chkstk overflow gotcha.
void test_vehicle_death_rows() {
    // ---- the vehicle death chain: rows 21 (DYING) and 23 (GROUND_DEAD) ----
    // [orig: AI_TransitionToDeath_GroundVehicle @0x467b20 / AI_TickState_VehicleDying
    // @0x467cd0 / AI_HandleEvent_VehicleDying @0x457f50 / AI_TransitionToDestroyed_
    // Vehicle @0x467de0 / AI_HandleEvent_ConsumeAll @0x458080; world-wac-ai-re §19]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(1, 4);
        Entity seed;
        seed.team = 1;
        seed.group_id = 3;
        seed.health = 0;
        const EntityHandle h = w.registry.spawn(1, seed);
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        sys.is_authority = true;
        int idx = sys.attach(h);
        AiEntity &e = *sys.at(idx);
        e.team = 1;
        e.health = 0;
        e.vel_x = 100; e.vel_z = 0; // slow (< 1057): the enter queues the destroy event
        e.brain.f[AiBrain::kCurState] = 21;

        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(21).enter(ctx);
        CHECK(e.brain.f[AiBrain::kAlert] == 2);            // the alert block ran
        CHECK(e.brain.f[AiBrain::kStep] == 16);            // [orig: ai_data[7] = 16]
        CHECK(w.relations.group(3).alert == TriggerRelations::kAlertRed);
        bool queued4 = false;
        for (int i = 0; i < sys.events.count(); ++i)
            if (sys.events.at(i).type() == 4) queued4 = true;
        CHECK(queued4);                                    // slow -> destroy event now

        // The dying event handler routes ONLY type 4 -> pending 23.
        AiEventEntry dmg{}; dmg.f[0] = 1;
        AiThinkCtx ctx_dmg{&sys, &e, &w, &dmg};
        sys.row(21).event(ctx_dmg);
        CHECK(e.brain.f[AiBrain::kPendState] != 23);
        AiEventEntry destroy{}; destroy.f[0] = 4;
        AiThinkCtx ctx_dst{&sys, &e, &w, &destroy};
        sys.row(21).event(ctx_dst);
        CHECK(e.brain.f[AiBrain::kPendState] == 23);

        // Enter 23: team cleared (a wreck goes teamless), moveStep 62, corpse timer 0.
        w.registry.get(h)->corpse_timer = 500;
        sys.row(23).enter(ctx);
        CHECK(e.brain.f[AiBrain::kStep] == 62);
        CHECK(e.team == 0);
        CHECK(w.registry.get(h)->team == 0);
        CHECK(w.registry.get(h)->corpse_timer == 0);       // wrecks never expire

        // The dead row swallows everything: no pending change from any event.
        e.brain.f[AiBrain::kPendState] = 0;
        AiEventEntry late{}; late.f[0] = 3;
        AiThinkCtx ctx_late{&sys, &e, &w, &late};
        sys.row(23).event(ctx_late);
        CHECK(e.brain.f[AiBrain::kPendState] == 0);
    }

    // ---- the dying tick: still-moving clears the work fields; stopping queues 4 ----
    // [orig: AI_TickState_VehicleDying @0x467cd0]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        auto sys_heap = std::make_unique<AiSystem>();
        AiSystem &sys = *sys_heap;
        int idx = sys.attach(EntityHandle::make(1, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = 21;
        e.vel_x = 5000; e.vel_z = 0;             // fast
        e.pos[0] = 100000;                        // far from the (0-init) death pose
        e.net_saved_live_pose[0] = 0;
        e.brain.f[AiBrain::kWorkPitch] = 7;
        e.brain.f[AiBrain::kWorkRoll] = 7;
        e.brain.f[AiBrain::kOutSpeed] = 7;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(21).tick(ctx);
        CHECK(sys.events.count() == 0);           // still crashing: no event yet
        CHECK(e.brain.f[AiBrain::kWorkPitch] == 0);
        CHECK(e.brain.f[AiBrain::kWorkRoll] == 0);
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 0);
        e.vel_x = 100;                            // stopped
        sys.row(21).tick(ctx);
        bool queued4 = false;
        for (int i = 0; i < sys.events.count(); ++i)
            if (sys.events.at(i).type() == 4) queued4 = true;
        CHECK(queued4);
    }
}

// The D-AI-6 muzzle seam: a FRESH embedder-fed posed muzzle replaces the chest-lift
// origin for spawned rounds; absent or stale stamps fall back. [orig: the
// anim-event fire spawns from Entity_GetAttachmentWorldPosition @0x4b2670 —
// the posed gun-flash userpoint; our embedder present layer feeds it back.]
static void test_fire_pass_uses_embedder_fed_muzzle() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    Entity seed{};
    seed.net_id = 900;
    seed.alive = true;
    seed.health = 100;
    EntityHandle h = w->registry.spawn(0, seed);

    w->ammo.entries.resize(2);
    w->ammo.entries[1].valid = true;
    w->ammo.entries[1].velocity = 62; // 1 u/tick; pos is asserted at SPAWN (no tick runs)
    w->ammo.entries[1].max_age_ticks = 100;

    AiSystem sys;
    int idx = sys.attach(h);
    AiEntity &e = *sys.at(idx);
    e.pos[0] = 10 << 16;
    e.pos[1] = 20 << 16;
    e.pos[2] = 5 << 16;
    e.profile.ammo_primary = 1;

    auto near_f = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };

    // No stamp yet -> the chest-lift stand-in (entity pos + 0.9 u).
    e.inf.last_events = 0x4; // the primary anim-fire event bit
    sys.infantry_fire_pass(e, *w, /*logic_tick=*/1);
    CHECK(w->round_sim.active_count == 1);
    CHECK(near_f(w->round_sim.rounds[0].pos.x, 10.0f));
    CHECK(near_f(w->round_sim.rounds[0].pos.z, 5.9f));

    // Fresh stamp -> rounds spawn from the posed muzzle.
    const int32_t muz[3] = {(10 << 16) + 0x8000, (20 << 16) - 0x4000, (5 << 16) + 0x4000};
    sys.set_entity_muzzle(h, muz, /*logic_tick=*/2);
    e.inf.last_events = 0x4;
    sys.infantry_fire_pass(e, *w, 3);
    CHECK(w->round_sim.active_count == 2);
    CHECK(near_f(w->round_sim.rounds[1].pos.x, 10.5f));
    CHECK(near_f(w->round_sim.rounds[1].pos.y, 19.75f));
    CHECK(near_f(w->round_sim.rounds[1].pos.z, 5.25f));

    // Stale stamp (older than the 4-tick freshness window) -> fallback again.
    e.inf.last_events = 0x4;
    sys.infantry_fire_pass(e, *w, 9); // 9 - 2 = 7 ticks stale
    CHECK(w->round_sim.active_count == 3);
    CHECK(near_f(w->round_sim.rounds[2].pos.z, 5.9f));
}

// Compact attack-animation source for the behavioral regressions below. Retail
// infantry fires from .bad event bit 0x4, so these drive the actual attack path.
struct AttackEventSource final : IRootMotionSource {
    bool has_clip(int, int id) const override {
        return id == anim_state::kIdle || id == anim_state::kAttack ||
               (id >= 67 && id <= 75) || (id >= 173 && id <= 239);
    }
    int32_t clip_length_ticks(int, int) const override { return -1; }
    bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
        if (!has_clip(0, id)) return false;
        ++phase;
        out = RootMotionFrame{};
        if (id == anim_state::kAttack) out.events = 0x4;
        return true;
    }
};

static void seed_test_rifle_ammo(World &w) {
    w.ammo.entries.resize(2);
    w.ammo.entries[1].velocity = 800;
    w.ammo.entries[1].max_age_ticks = 124;
    w.ammo.entries[1].weight_in_grains = 875;
    w.ammo.entries[1].min_damage = 10;
    w.ammo.entries[1].max_damage = 40;
    w.ammo.entries[1].valid = true;
}

// Exact-symptom feedback loop for a lethal hit at the live -> death-animation edge.
// The root values are the first half-frame samples measured from E_STAND.adm's
// I_idle1.bad and Dt2DeTFC.bad tracks. Retail retains the old clip for the death-edge
// tick, then advances both channels and blends their five numeric root lanes at 0.1.
struct DeathTransitionRootSource final : IRootMotionSource {
    static constexpr int32_t kIdleBottom = 66245;
    static constexpr int32_t kDeathBottom = 60145;
    static constexpr int32_t kDeathDx = -412;
    static constexpr int32_t kDeathDz = -555;
    static constexpr int32_t kFirstBlendDx = -41;
    static constexpr int32_t kFirstBlendBottom = 65635;
    static constexpr int32_t kFirstBlendDz = kFirstBlendBottom - kIdleBottom;

    bool has_clip(int, int id) const override {
        return id == anim_state::kIdle || (id >= 173 && id <= 239);
    }
    int32_t clip_length_ticks(int, int) const override { return -1; }
    bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
        if (!has_clip(0, id)) return false;
        ++phase;
        out = RootMotionFrame{};
        if (id == anim_state::kIdle) {
            out.capsule_bottom = kIdleBottom;
        } else {
            out.dx = kDeathDx;
            out.dz = kDeathDz;
            out.capsule_bottom = kDeathBottom;
        }
        return true;
    }
};

static void test_lethal_hit_blends_into_death_animation_without_position_jump() {
    World w;
    w.registry.configure_pool(0, 8);
    seed_test_rifle_ammo(w);

    Entity shooter_seed;
    shooter_seed.team = 1;
    const EntityHandle shooter_h = w.registry.spawn(0, shooter_seed);

    Entity victim_seed;
    victim_seed.kind = EntityKind::Organic;
    victim_seed.has_item_def = true;
    victim_seed.item_type = 3;
    victim_seed.team = 2;
    victim_seed.health = 10;
    victim_seed.health_max = 10;
    victim_seed.net_id = 0x21;
    victim_seed.group_id = 2;
    victim_seed.position = {20.0f, 0.0f, 0.0f};
    const EntityHandle victim_h = w.registry.spawn(0, victim_seed);

    DeathTransitionRootSource root;
    AiSystem ai;
    ai.root_motion = &root;
    const int victim_index = ai.attach(victim_h);
    AiEntity &victim = *ai.at(victim_index);
    victim.inf.active = true;
    victim.inf.anim_state = anim_state::kIdle;
    victim.health = 10;
    victim.team = 2;
    victim.net_id = 0x21;
    victim.pos[0] = 20 << 16;

    TickContext tick{};
    tick.world = &w;
    tick.is_authority = true; // the SP/listen-server authority path the NPC runs on.
    tick.logic_tick = 1;
    ai.tick(w, tick);

    RoundSpawnParams shot;
    shot.owner = shooter_h;
    shot.shooter_handle = shooter_h.packed;
    shot.origin = {0.0f, 0.0f, w.registry.get(victim_h)->position.z + 0.9f};
    shot.ammo_index = 1;
    CHECK(w.round_sim.spawn(w, shot) >= 0);
    for (int i = 0; i < 4 && w.registry.get(victim_h)->health > 0; ++i)
        w.round_sim.tick(w, nullptr, nullptr);
    CHECK(w.registry.get(victim_h)->health == 0);
    CHECK(!w.round_sim.deaths.empty());

    const int32_t transition_x = victim.pos[0];
    const int32_t transition_z = victim.pos[2];
    tick.logic_tick = 2;
    ai.tick(w, tick);

    CHECK(victim.inf.anim_state >= 173 && victim.inf.anim_state <= 239);
    CHECK(victim.inf.clip_phase == 0);
    CHECK(victim.inf.anim_blend_weight == 0.0f);
    CHECK(victim.pos[0] == transition_x);
    CHECK(victim.pos[2] == transition_z);

    const int32_t blend_x = victim.pos[0];
    const int32_t blend_z = victim.pos[2];
    tick.logic_tick = 3;
    ai.tick(w, tick);

    CHECK(victim.inf.clip_phase == 1);
    CHECK(victim.inf.anim_blend_weight == 0.1f);
    CHECK(victim.pos[0] - blend_x == DeathTransitionRootSource::kFirstBlendDx);
    if (victim.pos[2] - blend_z != DeathTransitionRootSource::kFirstBlendDz) {
        std::printf("first death blend position jump: dz=%d, expected blended dz=%d\n",
                    victim.pos[2] - blend_z, DeathTransitionRootSource::kFirstBlendDz);
        ++failures;
    }
}

static void configure_test_emplacement_weapon(WeaponTableEntry &weapon) {
    weapon.clipsize = -1;
    weapon.action_fsm.clip_capacity = -1;
    for (int action = 0; action < weapon_action::kCount; ++action)
        weapon.action_fsm.actions[action].id = action;
    weapon.action_fsm.actions[weapon_action::kRecoil].delay_end = 1;
}

static void configure_rifleman(AiEntity &npc, uint16_t net_id, uint8_t team) {
    npc.inf.active = true;
    npc.team = team;
    npc.net_id = net_id;
    npc.health = 100;
    npc.profile.ammo_primary = 1;
    npc.profile.clip_size = 30;
    npc.inf.magazine = 30;
    npc.slot.f[10] = 0;
    npc.slot.f[11] = 0;
    npc.slot.f[15] = 60 << 16;
    npc.slot.f[16] = 10 << 16;
    npc.slot.f[17] = 100 << 16;
    npc.slot.f[22] = 62;
}

static void test_world_feed_never_engages_same_team() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    seed_test_rifle_ammo(*w);

    Entity ally_seed{};
    ally_seed.kind = EntityKind::Organic;
    ally_seed.has_item_def = true;
    ally_seed.item_type = 3;
    ally_seed.team = 1;
    ally_seed.health = 100;
    ally_seed.net_id = 0x22;
    ally_seed.group_id = 2;
    ally_seed.position = Vec3{20.0f, 0.0f, 0.0f};
    const EntityHandle ally_h = w->registry.spawn(0, ally_seed);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.group_id = 1;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    w->ai = &ai;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    for (uint32_t tick = 0; tick < 512; ++tick) {
        ctx.logic_tick = tick;
        ai.tick(*w, ctx);
        w->round_sim.tick(*w, nullptr, nullptr);
    }

    CHECK(!npc.inf.combat_target.valid());
    CHECK(w->rounds.count == 0);
    CHECK(w->registry.get(ally_h)->health == 100);
}

static void test_berserk_candidate_is_intentional_team_exception() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);

    Entity candidate_seed{};
    candidate_seed.kind = EntityKind::Organic;
    candidate_seed.team = 1;
    candidate_seed.health = 100;
    candidate_seed.net_id = 0x22;
    candidate_seed.position = Vec3{20.0f, 0.0f, 0.0f};
    const EntityHandle candidate_h = w->registry.spawn(0, candidate_seed);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    w->ai = &ai;
    const int npc_index = ai.attach(npc_h);
    const int candidate_index = ai.attach(candidate_h);
    AiEntity &npc = *ai.at(npc_index);
    configure_rifleman(npc, 0x11, 1);
    npc.profile.ammo_primary = -1;
    AiEntity &candidate = *ai.at(candidate_index);
    configure_rifleman(candidate, 0x22, 1);
    candidate.profile.ammo_primary = -1;
    candidate.slot.f[1] |= 0x200;

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 28; // 28 + 36*0x11 is the scanner's 32-tick phase
    ai.tick(*w, ctx);

    // Shared infantry targeting accepts a same-team candidate carrying Berserk.
    // [orig: Entity_FindTargets @0x53a7ea..0x53a824]
    CHECK(npc.inf.combat_target == candidate_h);

    // The exception is symmetric: a Berserk scanner may also select an ordinary
    // same-team candidate.
    candidate.slot.f[1] &= ~0x200;
    npc.slot.f[1] |= 0x200;
    ai.ai_set_target(*w, npc, EntityHandle{});
    npc.inf.damage_timer = 0;
    npc.inf.was_hit = false;
    ctx.logic_tick = 156; // key 768: 32-tick scan, full-range phase
    ai.tick(*w, ctx);
    CHECK(npc.inf.combat_target == candidate_h);
}

static void test_damage_hit_sets_retail_alert_state() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);

    Entity shooter_seed{};
    shooter_seed.kind = EntityKind::Organic;
    shooter_seed.team = 2;
    shooter_seed.health = 100;
    shooter_seed.net_id = 0x21;
    shooter_seed.group_id = 2;
    const EntityHandle shooter_h = w->registry.spawn(0, shooter_seed);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.group_id = 1;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    w->ai = &ai;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);

    // The sim/AI seam carries the already-processed hit. Retail's damage callback
    // writes these stamps inline before the next infantry update.
    // [orig: Entity_HandleDamageTrigger @0x4073db..0x4073ea;
    //  Entity_OnDamageReceived @0x4af85b..0x4af878]
    w->round_sim.hits.push_back(RoundHit{npc_h, shooter_h, 10, 1, 1});

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1; // not a 32-tick perception scan: preserve lastAttacker
    ai.tick(*w, ctx);

    CHECK(npc.slot.bytes()[AiSlot::kMoveFlagByte] == 2);
    CHECK(w->relations.group(1).alert == TriggerRelations::kAlertRed);
    CHECK(npc.inf.damage_timer == 9); // +10 on hit, then the body tick decays once
    CHECK(npc.inf.was_hit);
    CHECK(npc.inf.last_attacker == shooter_h);

    npc.inf.was_hit = false;
    npc.inf.damage_timer = 0;
    npc.inf.last_attacker = EntityHandle{};
    w->round_sim.hits.push_back(RoundHit{npc_h, npc_h, 1, 1, 2});
    ctx.logic_tick = 2;
    ai.tick(*w, ctx);
    CHECK(!npc.inf.was_hit);
    CHECK(npc.inf.damage_timer == 0);
    CHECK(!npc.inf.last_attacker.valid());

    npc.inf.damage_timer = 24;
    w->round_sim.hits.push_back(RoundHit{npc_h, shooter_h, 1, 1, 3});
    ctx.logic_tick = 3;
    ai.tick(*w, ctx);
    CHECK(npc.inf.damage_timer == 33); // callback reaches 34, body tick decays once
}

static void test_remote_player_hit_skips_npc_group_alert() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);

    Entity shooter_seed{};
    shooter_seed.kind = EntityKind::Organic;
    shooter_seed.health = 100;
    const EntityHandle shooter_h = w->registry.spawn(0, shooter_seed);

    Entity player_seed{};
    player_seed.kind = EntityKind::Organic;
    player_seed.health = 100;
    player_seed.net_id = 0x41;
    player_seed.group_id = 3;
    player_seed.player_class = 8;
    player_seed.engine_flags = 0x100u;
    const EntityHandle player_h = w->registry.spawn(0, player_seed);

    AiSystem ai;
    ai.is_authority = true;
    w->ai = &ai;
    AiEntity &player = *ai.at(ai.attach(player_h));
    configure_rifleman(player, 0x41, 1);
    player.inf.is_local_player = false;
    w->round_sim.hits.push_back(RoundHit{player_h, shooter_h, 10, 1, 1});

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1;
    ai.tick(*w, ctx);

    CHECK(player.slot.bytes()[AiSlot::kMoveFlagByte] == 0);
    CHECK(w->relations.group(3).alert != TriggerRelations::kAlertRed);
    CHECK(player.inf.was_hit);
    CHECK(player.inf.last_attacker == shooter_h);
}

static void test_mounted_gunner_acquires_and_fires() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity enemy_seed{};
    enemy_seed.kind = EntityKind::Organic;
    enemy_seed.has_item_def = true;
    enemy_seed.item_type = 3;
    enemy_seed.team = 2;
    enemy_seed.health = 100;
    enemy_seed.net_id = 0x21;
    enemy_seed.group_id = 2;
    enemy_seed.position = Vec3{20.0f, 0.0f, 0.0f};
    const EntityHandle enemy_h = w->registry.spawn(0, enemy_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90; // engine heading 0: faces the enemy on +X.
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    w->registry.spawn(1, gun);
    w->weapons.entries.resize(2);
    w->weapons.entries[1].name.assign(1, 'x');
    w->weapons.entries[1].ammo_index = 1;
    w->weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.group_id = 1;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    w->ai = &ai;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    w->add_system(&ai);
    npc.profile.ammo_primary = -1; // mounted fire must not use the personal rifle slot
    CHECK(w->commands.mount(0x11, 0x31));

    bool acquired = false;
    bool fired = false;
    for (uint32_t tick = 0; tick < 1000 && !fired; ++tick) {
        // Exercise the production phase order: entity AI queues FIRE, the global
        // action pump consumes it, then the projectile pass steps the new round.
        // [orig: Entity_UpdateAllEntities @0x52674b, WeaponAction_ProcessAllEntities
        //  @0x526786, Weapon_UpdateAllProjectiles @0x4ec020]
        w->run_logic_tick(true, false);
        acquired = acquired || npc.inf.combat_target == enemy_h;
        fired = fired || w->rounds.count > 0;
    }
    CHECK(acquired);
    CHECK(fired);
}

static void test_mounted_fire_uses_retail_range_and_spatial_stagger() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity target_seed{};
    target_seed.kind = EntityKind::Organic;
    target_seed.team = 2;
    target_seed.health = 100;
    target_seed.net_id = 0x21;
    target_seed.position = Vec3{1.0f, 0.0f, 10.0f};
    const EntityHandle target_h = w->registry.spawn(0, target_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90;
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    w->registry.spawn(1, gun);
    w->weapons.entries.resize(2);
    w->weapons.entries[1].name.assign(1, 'x');
    w->weapons.entries[1].ammo_index = 1;
    w->weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AiSystem ai;
    ai.is_authority = true;
    w->ai = &ai;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    npc.profile.ammo_primary = -1;
    npc.slot.f[15] = 6 << 16;
    npc.inf.combat_target = target_h;
    npc.inf.aim_valid = true;
    npc.inf.aim_heading = 0;
    CHECK(w->commands.mount(0x11, 0x31));

    // Retail halves dz before the 3-D range test: sqrt(1^2 + (10/2)^2) < 6.
    // [orig: Entity_UpdateInfantryAI sar dz,1 @0x4bf515]
    ai.infantry_mounted_fire_pass(npc, *w, 0, 0);
    Entity *gun_live = w->registry.get(w->registry.find_by_net_id(0x31));
    CHECK(gun_live != nullptr);
    CHECK(gun_live->primary_weapon_slot.next == weapon_action::kFire);
    CHECK(w->rounds.count == 0);
    gun_live->posed_muzzle_world[0] = 0x12345;
    gun_live->posed_muzzle_world[1] = -0x23456;
    gun_live->posed_muzzle_world[2] = 0x34567;
    gun_live->posed_muzzle_tick = 0;
    gun_live->posed_muzzle_valid = true;
    // Merely caching an owner as local cannot disable the standalone World's
    // global slot pump. Only an adapter that explicitly supplies its own pump
    // may take ownership.
    w->cached.local_player = npc_h;
    ai.pump_mounted_weapon_slots(*w, 0);
    CHECK(w->rounds.count == 1);
    CHECK(w->rounds.records[0].shooter_handle == npc_h.packed);
    CHECK(w->rounds.records[0].origin_x == 0x12345);
    CHECK(w->rounds.records[0].origin_y == -0x23456);
    CHECK(w->rounds.records[0].origin_z == 0x34567);
    CHECK(gun_live->primary_weapon_slot.current == weapon_action::kFire);
    const int32_t busy_next = gun_live->primary_weapon_slot.next;
    ai.infantry_mounted_fire_pass(npc, *w, 4, 4);
    CHECK(gun_live->primary_weapon_slot.next == busy_next);
    CHECK(w->rounds.count == 1);

    // The four-tick gate is followed by a spatial stagger. Whole-unit positions
    // leave bit 0x40 to the stagger key here, so key 64 suppresses the request.
    // [orig: Entity_UpdateInfantryAI @0x4bf4e3..0x4bf4ee]
    w->registry.get(target_h)->position.z = 0.0f;
    const int before = w->rounds.count;
    gun_live->primary_weapon_slot = WeaponSlotState{};
    ai.infantry_mounted_fire_pass(npc, *w, 64, 64);
    CHECK(gun_live->primary_weapon_slot.next == weapon_action::kIdle);
    ai.pump_mounted_weapon_slots(*w, 64);
    CHECK(w->rounds.count == before);

    gun_live->primary_weapon_slot = WeaponSlotState{};
    gun_live->primary_weapon_slot.next = weapon_action::kFire;
    w->external_local_mounted_weapon_pump = true;
    ai.pump_mounted_weapon_slots(*w, 68);
    CHECK(w->rounds.count == before);
    CHECK(gun_live->primary_weapon_slot.next == weapon_action::kFire);
}

static void test_mounted_look_traverses_before_fire_request() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);
    seed_test_rifle_ammo(*w);

    Entity target_seed{};
    target_seed.kind = EntityKind::Organic;
    target_seed.team = 2;
    target_seed.health = 100;
    target_seed.net_id = 0x21;
    target_seed.position = Vec3{20.0f, 20.0f, 0.0f};
    const EntityHandle target_h = w->registry.spawn(0, target_seed);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.team = 1;
    gun.net_id = 0x31;
    gun.yaw = 90;
    gun.primary_weapon.assign(1, 'x');
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w->registry.spawn(1, gun);
    w->weapons.entries.resize(2);
    w->weapons.entries[1].name.assign(1, 'x');
    w->weapons.entries[1].ammo_index = 1;
    w->weapons.entries[1].valid = true;
    configure_test_emplacement_weapon(w->weapons.entries[1]);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AiSystem ai;
    ai.is_authority = true;
    w->ai = &ai;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x11, 1);
    npc.inf.combat_target = target_h;
    npc.inf.aim_valid = true;
    npc.inf.aim_heading = 0x20000000;
    npc.inf.aim_pitch = 0;
    npc.heading = 0;
    CHECK(w->commands.mount(0x11, 0x31));

    CHECK(ai.pose_if_mounted(npc, *w));
    CHECK(npc.heading == 0x02000000);
    ai.infantry_mounted_fire_pass(npc, *w, 0, 0);
    CHECK(w->registry.get(gun_h)->primary_weapon_slot.next == weapon_action::kIdle);

    bool queued = false;
    for (uint32_t phase = 4; phase <= 60 && !queued; phase += 4) {
        CHECK(ai.pose_if_mounted(npc, *w));
        ai.infantry_mounted_fire_pass(npc, *w, phase, phase);
        queued = w->registry.get(gun_h)->primary_weapon_slot.next ==
                weapon_action::kFire;
    }
    CHECK(queued);
}

static void test_mounted_gunner_dismounts_into_death_animation() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 8);
    w->registry.configure_pool(1, 8);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.net_id = 0x31;
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    const EntityHandle gun_h = w->registry.spawn(1, gun);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x11;
    npc_seed.deathtime_ticks = 124;
    npc_seed.equipped_adm_index = 7;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    AttackEventSource clips;
    AiSystem ai;
    ai.is_authority = true;
    ai.root_motion = &clips;
    w->ai = &ai;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    npc.inf.active = true;
    npc.health = 100;
    CHECK(w->commands.mount(0x11, 0x31));

    constexpr int kSelectedDeath = 184;
    w->registry.get(npc_h)->health = 0;
    w->registry.get(npc_h)->death_anim_state = kSelectedDeath;
    npc.health = 0;

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1;
    ai.tick(*w, ctx);

    CHECK(!w->registry.get(npc_h)->mounted);
    CHECK(!w->registry.get(gun_h)->seats[0].occupant.valid());
    CHECK(w->registry.get(npc_h)->equipped_adm_index == 0xFF);
    CHECK(!w->registry.get(npc_h)->use_gun_slot_swapped);
    CHECK(!w->registry.get(gun_h)->primary_weapon_owner.valid());
    CHECK(npc.inf.anim_state == kSelectedDeath);
    CHECK(w->registry.get(npc_h)->death_anim_state == 0);
    CHECK(w->registry.get(npc_h)->corpse_timer == 123);
    CHECK(npc.inf.clip_phase == 0); // death edge retains the mounted channel this tick
    CHECK(npc.inf.anim_blend_weight == 0.0f);
}

static void test_mounted_collision_tail_uses_retail_eight_tick_phase_without_models() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 4);

    Entity gun{};
    gun.kind = EntityKind::Item;
    gun.net_id = 0x20;
    Seat seat{};
    seat.type = SeatType::Gunner;
    gun.seats.push_back(seat);
    w->registry.spawn(1, gun);

    Entity npc_seed{};
    npc_seed.kind = EntityKind::Organic;
    npc_seed.team = 1;
    npc_seed.health = 100;
    npc_seed.net_id = 0x10;
    const EntityHandle npc_h = w->registry.spawn(0, npc_seed);

    CollisionWorld collision;
    AiSystem ai;
    ai.is_authority = true;
    ai.collision = &collision;
    w->ai = &ai;
    AiEntity &npc = *ai.at(ai.attach(npc_h));
    configure_rifleman(npc, 0x10, 1);
    CHECK(w->commands.mount(0x10, 0x20));
    CHECK(collision.instance_count() == 0);

    Entity *npc_live = w->registry.get(npc_h);
    CHECK(npc_live != nullptr);
    npc_live->flags |= kEntityFlagArmoryZone;

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = true;
    ctx.logic_tick = 1;
    ai.tick(*w, ctx);
    CHECK((npc_live->flags & kEntityFlagArmoryZone) != 0);

    // key = tick + 36*net_id; net id 0x10 leaves the low three bits unchanged.
    // The mounted tail therefore resolves at tick 8 even with no model instances,
    // clearing the resolver's transient contact flags only on that retail phase.
    // [orig: Entity_UpdateInfantryAI @0x4bf5a5..0x4bf5c3]
    ctx.logic_tick = 8;
    ai.tick(*w, ctx);
    CHECK((npc_live->flags & kEntityFlagArmoryZone) == 0);
}

// A joiner still runs each vehicle's retail physics callback for presentation
// evaluation, but its compact-record copy is wire-posed rather than motor-integrated.
// Sound therefore reads the current replicated speed/claimant state without advancing
// the vehicle transform. [orig: Entity_UpdateVehiclePhysics @0x48af00; movement-sound
// call @0x48d181..0x48d1c4]
static void test_joiner_evaluates_vehicle_idle_without_integrating_motor() {
    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    w->registry.configure_pool(1, 4);

    static constexpr char kProfile[] =
            "begin \"SP_JoinerTruck\"\n"
            "  Soundloop_1 V_TRUCK_ILP .8 1.2\n"
            "end\n";
    CHECK(w->sound_profiles.parse(kProfile, sizeof(kProfile) - 1) == 1);

    Entity npc{};
    npc.kind = EntityKind::Organic;
    npc.health = 100;
    npc.alive = true;
    const EntityHandle npc_h = w->registry.spawn(0, npc);

    Entity vehicle{};
    vehicle.kind = EntityKind::Item;
    vehicle.item_id = 1291;
    vehicle.health = 3000;
    vehicle.health_max = 3000;
    vehicle.alive = true;
    vehicle.position = Vec3{100.0f, 200.0f, 10.0f};
    vehicle.yaw = 37;
    vehicle.veh.speed = 0;
    vehicle.primary_occupant = npc_h; // NPC claimant: the PlayerControl loop gate
    const EntityHandle vehicle_h = w->registry.spawn(1, vehicle);
    Entity *npc_live = w->registry.get(npc_h);
    CHECK(npc_live != nullptr);
    npc_live->mounted = true;
    npc_live->mount_target = vehicle_h;

    VehicleTraits traits{};
    traits.physics = 1;
    traits.player_speed = 94 * 293;
    traits.player_control = true;
    traits.sound_profile = "SP_JoinerTruck";
    w->vehicle_traits.set(vehicle.item_id, traits);

    AiSystem ai;
    w->ai = &ai;
    w->add_system(&ai);

    const Vec3 before = w->registry.get(vehicle_h)->position;
    const int16_t yaw_before = w->registry.get(vehicle_h)->yaw;
    w->run_logic_tick(false, false);

    const Entity *after = w->registry.get(vehicle_h);
    CHECK(after != nullptr);
    CHECK(after->position.x == before.x);
    CHECK(after->position.y == before.y);
    CHECK(after->position.z == before.z);
    CHECK(after->yaw == yaw_before);

    CHECK(w->sound_emitters.size() == 1);
    if (w->sound_emitters.size() == 1) {
        const SoundEmitterEvent &idle = w->sound_emitters[0];
        CHECK(idle.source_spawn_id == after->registry_spawn_id);
        CHECK(idle.source_handle == vehicle_h.packed);
        CHECK(idle.pos.x == before.x);
        CHECK(idle.pos.y == before.y);
        CHECK(idle.pos.z == before.z);
        CHECK(idle.lane == 0);
        CHECK(idle.slot == 0);
        CHECK(idle.lifetime_ticks == 30);
        CHECK(idle.pitch_q16 == 0x10000);
        CHECK(idle.volume_q8_8 == 0xFFFF);
        CHECK(idle.set_name == "V_TRUCK_ILP");
    }

    // Client presentation validates the full mount relationship instead of
    // trusting a non-null replicated handle forever. A stale claimant may keep
    // moving a residual lane's source anchor during its original lifetime, but
    // it must not refresh the idle registration indefinitely.
    w->sound_emitters.clear();
    npc_live->mounted = false;
    w->run_logic_tick(false, false);
    CHECK(w->sound_emitters.size() == 1);
    if (w->sound_emitters.size() == 1) {
        CHECK(w->sound_emitters[0].source_only);
    }
    for (int i = 0; i < 30; ++i) {
        w->sound_emitters.clear();
        w->run_logic_tick(false, false);
    }
    CHECK(w->sound_emitters.empty());
}

int main() {
    // ---- struct layout (byte-exact strides) ----
    CHECK(sizeof(AiBrain) == 812);
    CHECK(sizeof(AiSlot) == 172);

    // ---- state name table (Entity_LookupAIStateName) ----
    CHECK(streq(ai_state_name(kAiGroundFollowWp), "GROUND_FOLLOWWP"));
    CHECK(streq(ai_state_name(kAiGroundDead), "GROUND_DEAD"));
    CHECK(streq(ai_state_name(kAiHeloCombat), "HELO_COMBAT"));
    CHECK(streq(ai_state_name(13), "?")); // transitional gap
    CHECK(streq(ai_state_name(21), "?"));

    // ---- AI_BeginUpdate budget gate ----
    {
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kStep] = 64;
        e.brain.f[AiBrain::kSpeedA] = 7;
        e.heading = 123;

        // Under budget: accumulates and proceeds; working fields copied.
        sys.scheduler.budget = 0;
        CHECK(sys.begin_update(e) == true);
        CHECK(sys.scheduler.budget == 64);          // += step
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 7);  // [128] = [49]
        CHECK(e.brain.f[132] == 123);               // = heading

        // Over budget, profile flag clear -> forced pending state 8.
        sys.scheduler.budget = 500;
        e.brain.f[AiBrain::kPendState] = 0;
        e.profile.flags100 = 0;
        CHECK(sys.begin_update(e) == false);
        CHECK(sys.scheduler.budget == 500);         // unchanged
        CHECK(e.brain.f[AiBrain::kPendState] == 8);

        // Over budget, profile flag set -> forced pending = fallback.
        sys.scheduler.budget = 500;
        e.profile.flags100 = 2;
        e.brain.f[AiBrain::kFallback] = 17;
        CHECK(sys.begin_update(e) == false);
        CHECK(e.brain.f[AiBrain::kPendState] == 17);
    }

    // ---- handle lookup index ----
    {
        AiSystem sys;
        EntityHandle h0 = EntityHandle::make(0, 4);
        EntityHandle h1 = EntityHandle::make(2, 9);
        EntityHandle h2 = EntityHandle::make(3, 7);
        CHECK(sys.for_handle(h0) == nullptr);
        int i0 = sys.attach(h0);
        int i1 = sys.attach(h1);
        CHECK(sys.for_handle(h0) == sys.at(i0));
        CHECK(sys.for_handle(h1) == sys.at(i1));
        CHECK(sys.for_handle(EntityHandle{}) == nullptr);

        sys.capture_spawn_baseline();
        sys.attach(h2);
        CHECK(sys.for_handle(h2) != nullptr);
        World w;
        sys.on_load(w);
        CHECK(sys.for_handle(h0) == sys.at(i0));
        CHECK(sys.for_handle(h1) == sys.at(i1));
        CHECK(sys.for_handle(h2) == nullptr);
    }

    // ---- state-machine dispatcher transition (authority) ----
    {
        World w;
        AiSystem sys;
        sys.is_authority = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.has_physics = false; // avoid the alert-edge forcing pending=10
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // 16
        e.brain.f[AiBrain::kPendState] = kAiGroundFormation; // 19 (enter = full_reset_to_idle)
        e.brain.f[AiBrain::kStep] = 999;
        int32_t tick_before = e.brain.f[AiBrain::kTick];

        sys.process_infantry_state_machine(e, w, 0);

        CHECK(e.brain.f[AiBrain::kTick] == tick_before + 1); // ++ each update
        CHECK(e.brain.f[AiBrain::kCurState] == kAiGroundFormation); // committed
        CHECK(e.brain.f[AiBrain::kStep] == 16); // enter[19]=AI_FullResetToIdle set step 16
    }

    // ---- reset-to-patrol enter handler (byte-exact: step=64, alert cleared) ----
    {
        World w;
        AiSystem sys;
        sys.is_authority = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.has_physics = false;
        e.brain.f[AiBrain::kAlert] = 5;
        e.brain.f[AiBrain::kPrevAlert] = 5;
        e.brain.f[AiBrain::kCurState] = 0;
        e.brain.f[AiBrain::kPendState] = kAiHeloPretty; // 14, enter = AI_ResetToPatrol
        sys.apply_transition(e, w);
        CHECK(e.brain.f[AiBrain::kCurState] == 14);
        CHECK(e.brain.f[AiBrain::kStep] == 64);
        CHECK(e.brain.f[AiBrain::kAlert] == 0);
        CHECK(e.brain.f[AiBrain::kPrevAlert] == 0);
    }

    // ---- AIEvent ring: timer decrement, expiry dispatch, transition, compaction ----
    {
        World w;
        AiSystem sys;
        sys.is_authority = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = 13; // event handler = AI_HandleAlertEvent
        e.brain.f[AiBrain::kPendState] = 13;

        // Event that fires after 2 frames (timer 0.03 > 0.016 once, expires twice).
        AiEventEntry ev{};
        ev.f[0] = 4;                 // type 4 (alert)
        ev.f[1] = 9 | (idx << 16);   // channel 9 | entity index
        ev.set_timer(0.03f);
        sys.events.queue(ev);
        CHECK(sys.events.count() == 1);

        sys.events.process_timed(sys, w);   // frame 1: 0.03 -> 0.014, not expired
        CHECK(sys.events.count() == 1);
        CHECK(e.brain.f[AiBrain::kCurState] == 13); // not fired yet

        sys.events.process_timed(sys, w);   // frame 2: 0.014 -> <0, expires
        CHECK(sys.events.count() == 0);     // compacted out
        // AI_HandleAlertEvent set pending=15, then the ring applied the transition.
        CHECK(e.brain.f[AiBrain::kCurState] == 15);
    }

    // ---- not_yet_ported coverage counter via full tick ----
    {
        World w;
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.has_physics = false;
        e.brain.f[AiBrain::kCurState] = kAiHeloLand; // 6 — a still-unported tick row
        e.brain.f[AiBrain::kPendState] = kAiHeloLand;
        TickContext ctx;
        ctx.world = &w;
        ctx.is_authority = true;
        sys.tick(w, ctx);
        CHECK(sys.unported_calls >= 1); // the HELO tick routed through the stub
    }

    // ---- body-anim slot selection from movement (update_body_anim_slot) ----
    {
        // body_anim_adm_key maps slots to the AI .adm key namespace.
        CHECK(streq(body_anim_adm_key(kBodyAnimIdle), "anim_idle"));
        CHECK(streq(body_anim_adm_key(kBodyAnimWalkForward), "anim_walk_forward"));
        CHECK(streq(body_anim_adm_key(kBodyAnimRunForward), "anim_run_forward"));
        CHECK(streq(body_anim_adm_key(-1), ""));

        World w;
        w.registry.configure_pool(0, 8);
        Entity seed;
        seed.alive = true;
        seed.health = 100;
        EntityHandle h = w.registry.spawn(0, seed);
        CHECK(h != EntityHandle{});

        AiSystem sys;
        sys.is_authority = true;
        int idx = sys.attach(h);
        AiEntity &e = *sys.at(idx);
        e.has_physics = false;
        // State 20's row is all no-ops, so nothing clobbers kOutSpeed -- isolating the
        // movement->slot mapping (which keys off kOutSpeed + kAlert, not the state id).
        e.brain.f[AiBrain::kCurState] = kAiGroundReturnToBase;  // 20
        e.brain.f[AiBrain::kPendState] = kAiGroundReturnToBase;

        // Moving, not alert -> walk_forward.
        e.brain.f[AiBrain::kOutSpeed] = 10;
        e.brain.f[AiBrain::kAlert] = 0;
        sys.process_infantry_state_machine(e, w, 0);
        CHECK(w.registry.get(h)->body_anim_slot == kBodyAnimWalkForward);

        // Moving + alert -> run_forward.
        e.brain.f[AiBrain::kOutSpeed] = 10;
        e.brain.f[AiBrain::kAlert] = 2;
        sys.process_infantry_state_machine(e, w, 0);
        CHECK(w.registry.get(h)->body_anim_slot == kBodyAnimRunForward);

        // Stopped -> idle.
        e.brain.f[AiBrain::kOutSpeed] = 0;
        sys.process_infantry_state_machine(e, w, 0);
        CHECK(w.registry.get(h)->body_anim_slot == kBodyAnimIdle);

        // Dead -> slot left as-is (present pass hides it); not overwritten to idle.
        w.registry.get(h)->body_anim_slot = kBodyAnimWalkForward;
        w.registry.get(h)->alive = false;
        w.registry.get(h)->health = 0;
        sys.process_infantry_state_machine(e, w, 0);
        CHECK(w.registry.get(h)->body_anim_slot == kBodyAnimWalkForward);
    }

    // ======================= P1: GROUND_FOLLOWWP movement =======================

    // ---- waypoint solver: type 3 (literal coord), byte-exact dist + bearing ----
    {
        NavNodeTable nav; // unused for type 3
        AiBrain b;
        // Target straight along +dx (pos+4): atan2(dz=0, dx=100) = 0 -> bearing 0.
        b.f[AiBrain::kWpType] = 3;
        b.f[AiBrain::kWpCoordX] = 100;
        b.f[AiBrain::kWpCoordY] = 0;
        b.f[AiBrain::kWpCoordZ] = 0;
        b.f[AiBrain::kWpCoordSrc] = 777;
        int32_t pos[3] = {0, 0, 0};
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpDistance] == 100);  // base=|dx|=100, cross term 0
        CHECK(b.f[AiBrain::kWpBearing] == 0);     // atan2(0,+) == 0  (verifies arg order)
        CHECK(b.f[AiBrain::kWpExtra] == 0);
        CHECK(b.f[AiBrain::kWpNodeVal] == 777);   // = kWpCoordSrc

        // Target along +dz (pos+8): atan2(dz=100, dx=0) = +pi/2 -> large positive bearing.
        b.f[AiBrain::kWpCoordX] = 0;
        b.f[AiBrain::kWpCoordY] = 100;
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpDistance] == 100);
        CHECK(b.f[AiBrain::kWpBearing] > 1000000000); // ~2^30 quarter turn
    }

    // ---- waypoint solver: type 1 (nav node) resolves a pool-3 entry ----
    {
        NavNodeTable nav;
        nav.channels.resize(2);              // channel id 1 (0 is the navMeshId==0 sentinel)
        nav.channels[1].count = 2;
        nav.channels[1].entries[0] = 0;      // -> nodes[0]
        nav.channels[1].entries[1] = 1;
        nav.nodes.resize(2);
        nav.nodes[0] = NavEntry{{7, 100, 0, 0, 9}}; // {payload0, x, y, z, payload4}
        AiBrain b;
        b.f[AiBrain::kWpType] = 1;
        b.f[AiBrain::kWpChannel] = 1;
        b.f[AiBrain::kWpNode] = 0;
        int32_t pos[3] = {0, 0, 0};
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpResolved] == 0);    // pool-3 index stored (deviation: idx not ptr)
        CHECK(b.f[AiBrain::kWpDistance] == 100);
        CHECK(b.f[AiBrain::kWpBearing] == 0);     // atan2(0,100)=0
        CHECK(b.f[AiBrain::kWpNodeVal] == 7);     // navEntry[0]
        CHECK(b.f[AiBrain::kWpExtra] == 9);       // navEntry[4]
    }

    // ---- waypoint solver: -1 sentinels + type-2 no-op ----
    {
        NavNodeTable nav;
        nav.channels.resize(2);
        nav.channels[1].count = 0;          // present but empty
        int32_t pos[3] = {0, 0, 0};
        AiBrain b;
        b.f[AiBrain::kWpType] = 1;
        b.f[AiBrain::kWpChannel] = 0;       // navMeshId == 0 -> -1
        CHECK(ai_waypoint_update_target(b, pos, nav) == -1);
        b.f[AiBrain::kWpChannel] = 1;       // channel present but count==0 -> -1
        CHECK(ai_waypoint_update_target(b, pos, nav) == -1);
        b.f[AiBrain::kWpType] = 5;          // unknown type -> -1
        CHECK(ai_waypoint_update_target(b, pos, nav) == -1);
        b.f[AiBrain::kWpType] = 2;          // type 2 -> 0 (no-op)
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
    }

    // ---- path follower: arrival advances the node + records relmat + outputs ----
    {
        World w;
        AiSystem sys;
        sys.nav.channels.resize(2);
        sys.nav.channels[1].count = 3;
        sys.nav.channels[1].loopflag = 0;
        sys.nav.channels[1].entries[0] = 10;
        sys.nav.channels[1].entries[1] = 11;
        sys.nav.channels[1].entries[2] = 12;
        sys.nav.nodes.resize(13);
        sys.nav.nodes[10] = NavEntry{{1000, 500, 600, 0, 0}}; // payload0=1000 (arrival radius)
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.relmat_id = 4;
        e.net_id = 9;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // 16 -> moveSpeed = kSpeedB
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 1;
        e.brain.f[AiBrain::kWpNode] = 0;
        e.brain.f[AiBrain::kSpeedB] = 20;
        e.brain.f[AiBrain::kStep] = 64;

        sys.update_waypoint_movement(e, w);

        // dist=600 (|dz|=600 base) < nodeVal=1000 -> advance.
        CHECK(e.brain.f[AiBrain::kWpNode] == 1);
        CHECK(e.brain.f[AiBrain::kStoredKeyTime] == 1000);
        CHECK(sys.relmat_calls.size() == 2);
        CHECK(sys.relmat_calls[0].which == 1);                    // SetBitB first
        CHECK(sys.relmat_calls[0].key == 4);                      // group / SetBitB key
        CHECK(sys.relmat_calls[1].which == 0);                    // SetBitA second
        CHECK(sys.relmat_calls[1].key == 9);                      // SSN / SetBitA key
        CHECK(sys.relmat_calls[0].channel == 1 && sys.relmat_calls[0].node == 0);
        CHECK(w.relations.group_visited(4, 1, 0));
        CHECK(w.relations.single_visited(9, 1, 0));
        // working transform from the resolved node; out-speed halved (timeDelta<step*speed).
        CHECK(e.brain.f[AiBrain::kWorkPosX] == 500);
        CHECK(e.brain.f[AiBrain::kWorkPosY] == 600);
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 10);               // 20 >> 1
    }

    // ---- path follower: loop-wrap vs one-shot terminate at path end ----
    {
        for (int loopflag = 0; loopflag <= 1; ++loopflag) {
            World w;
            AiSystem sys;
            sys.nav.channels.resize(2);
            sys.nav.channels[1].count = 3;
            sys.nav.channels[1].loopflag = loopflag;
            sys.nav.channels[1].entries[2] = 12;
            sys.nav.nodes.resize(13);
            sys.nav.nodes[12] = NavEntry{{2000, 700, 800, 0, 0}};
            int idx = sys.attach(EntityHandle::make(0, 0));
            AiEntity &e = *sys.at(idx);
            e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
            e.brain.f[AiBrain::kWpType] = 1;
            e.brain.f[AiBrain::kWpChannel] = 1;
            e.brain.f[AiBrain::kWpNode] = 2; // last node -> ++ hits count(3)
            e.brain.f[AiBrain::kSpeedB] = 20;
            e.brain.f[AiBrain::kStep] = 64;

            sys.update_waypoint_movement(e, w);

            if (loopflag & 1) {
                // one-shot: terminate, clear waypoint type, clamp node, freeze.
                CHECK(e.brain.f[AiBrain::kWpType] == 0);
                CHECK(e.brain.f[AiBrain::kWpNode] == 2);   // count-1
                CHECK(e.brain.f[AiBrain::kOutSpeed] == 0);
            } else {
                // loop: wrap to 0, keep moving (out-speed set from the resolved node).
                CHECK(e.brain.f[AiBrain::kWpNode] == 0);
                CHECK(e.brain.f[AiBrain::kWpType] == 1);
                CHECK(e.brain.f[AiBrain::kWorkPosX] == 700);
            }
        }
    }

    // ---- state-16 tick: alive + no target -> walks the path ----
    {
        World w;
        AiSystem sys;
        sys.nav.nodes.resize(1);
        sys.nav.nodes[0] = NavEntry{{0, 100, 0, 0, 0}};
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kWpType] = 3;       // literal coord toward (100,0,0)
        e.brain.f[AiBrain::kWpCoordX] = 100;
        e.brain.f[AiBrain::kWpResolved] = 0;   // resolves nodes[0]
        e.brain.f[AiBrain::kSpeedB] = 20;
        e.brain.f[AiBrain::kStep] = 64;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(sys.find_target_calls == 1);          // alive path attempted acquisition
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 10); // mover ran (20 >> 1)
        CHECK(e.brain.f[AiBrain::kWorkPosX] == 100);
    }

    // ---- state-16 tick: death queues crash (3) vs still (4) by speed ----
    {
        World w;
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.health = 0;
        e.vel_x = 2000; e.vel_z = 0;  // |v| = 2000 >= 1057 -> crash death (3)
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(sys.events.count() == 1);
        CHECK(sys.events.at(0).type() == 3);
        CHECK(sys.events.at(0).entity_index() == idx);

        e.vel_x = 100; e.vel_z = 0;   // |v| = 100 < 1057 -> still death (4)
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(sys.events.count() == 2);
        CHECK(sys.events.at(1).type() == 4);
    }

    // ---- path follower: state-17 uses 16*speed threshold + kSpeedA; un-halved output ----
    {
        World w;
        AiSystem sys;
        sys.nav.channels.resize(2);
        sys.nav.channels[1].count = 3;
        sys.nav.channels[1].entries[0] = 10;
        sys.nav.nodes.resize(11);
        sys.nav.nodes[10] = NavEntry{{1000, 2000, 0, 0, 0}}; // nodeVal=1000, X=2000 -> dist 2000
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundCombat; // 17 -> moveSpeed = kSpeedA
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 1;
        e.brain.f[AiBrain::kWpNode] = 0;
        e.brain.f[AiBrain::kSpeedA] = 20;
        e.brain.f[AiBrain::kSpeedB] = 999; // must NOT be used in state 17
        e.brain.f[AiBrain::kStep] = 64;
        sys.update_waypoint_movement(e, w);
        // dist=2000 >= nodeVal=1000 -> no advance. timeDelta=1000. state17 threshold 16*20=320;
        // 1000 >= 320 -> NOT halved -> speed stays 20 (proves kSpeedA source + 16x threshold).
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 20);
        CHECK(e.brain.f[AiBrain::kWpNode] == 0); // never advanced
    }

    // ---- contrast: state-16 with the SAME timeDelta halves (threshold speed*step=1280) ----
    {
        World w;
        AiSystem sys;
        sys.nav.channels.resize(2);
        sys.nav.channels[1].count = 3;
        sys.nav.channels[1].entries[0] = 10;
        sys.nav.nodes.resize(11);
        sys.nav.nodes[10] = NavEntry{{1000, 2000, 0, 0, 0}};
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // 16 -> moveSpeed = kSpeedB
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 1;
        e.brain.f[AiBrain::kSpeedB] = 20;
        e.brain.f[AiBrain::kStep] = 64;
        sys.update_waypoint_movement(e, w);
        // state16 threshold 20*64=1280; timeDelta 1000 < 1280 -> halve -> 10.
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 10);
    }

    // ---- waypoint solver: distance approximation tail (5*cross>>16) + |dy| term ----
    {
        NavNodeTable nav;
        AiBrain b;
        b.f[AiBrain::kWpType] = 3;
        b.f[AiBrain::kWpCoordX] = 20000; // dx
        b.f[AiBrain::kWpCoordY] = 20001; // dz (max axis -> base)
        b.f[AiBrain::kWpCoordZ] = 20000; // dy (enters the cross sum)
        int32_t pos[3] = {0, 0, 0};
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        // base=|dz|=20001; cross=|dx|+|dy|=40000; term=(5*40000)>>16=200000>>16=3; dist=20004.
        CHECK(b.f[AiBrain::kWpDistance] == 20004);
    }

    // ---- path follower: unresolvable waypoint -> freeze at current transform ----
    {
        World w;
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.pos[0] = 11; e.pos[1] = 22; e.pos[2] = 33;
        e.heading = 44; e.pitch = 55; e.roll = 66;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 0; // navMeshId 0 -> solver returns -1 -> freeze
        e.brain.f[AiBrain::kSpeedB] = 20;
        sys.update_waypoint_movement(e, w);
        CHECK(e.brain.f[AiBrain::kWorkPosX] == 11);
        CHECK(e.brain.f[AiBrain::kWorkPosY] == 22);
        CHECK(e.brain.f[AiBrain::kWorkPosZ] == 33);
        CHECK(e.brain.f[AiBrain::kWorkHeading] == 44);
        CHECK(e.brain.f[AiBrain::kWorkPitch] == 55);
        CHECK(e.brain.f[AiBrain::kWorkRoll] == 66);
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 0);
    }

    // ---- waypoint solver: bearing quadrant + sign + truncation-toward-zero ----
    {
        NavNodeTable nav;
        AiBrain b;
        int32_t pos[3] = {0, 0, 0};
        b.f[AiBrain::kWpType] = 3;
        b.f[AiBrain::kWpCoordX] = 100; // dx
        b.f[AiBrain::kWpCoordY] = 100; // dz -> atan2(100,100)=+pi/4 ~ 2^29 = 536870912
        b.f[AiBrain::kWpCoordZ] = 0;
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpBearing] > 536000000 && b.f[AiBrain::kWpBearing] < 537000000);
        // negative dz -> -pi/4: pins the sign + chop-toward-zero (same magnitude, negative).
        b.f[AiBrain::kWpCoordY] = -100;
        CHECK(ai_waypoint_update_target(b, pos, nav) == 0);
        CHECK(b.f[AiBrain::kWpBearing] < -536000000 && b.f[AiBrain::kWpBearing] > -537000000);
    }

    // ---- state-16 tick: can-fire + fire timer decrements by step ----
    {
        World w;
        AiSystem sys;
        sys.nav.nodes.resize(1);
        sys.nav.nodes[0] = NavEntry{{0, 0, 0, 0, 0}};
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kWpType] = 3;
        e.brain.f[AiBrain::kFireTimer] = 50;
        e.brain.f[AiBrain::kStep] = 64;
        e.profile.flags96 = 0x10; // can-fire
        int before = sys.unported_calls;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(sys.unported_calls == before + 1);         // compute-fire-positions stub (P2)
        CHECK(e.brain.f[AiBrain::kFireTimer] == 50 - 64); // -= kStep
    }

    // ======================= P2: GROUND combat + targeting =======================

    // ---- PRNG: the rotate-LCG (dword_31BFBB8 / PRNG_Next16) is byte-exact + independent ----
    {
        AiSystem sys;
        sys.prng_a = 1;
        // s = rotl(1+rotl(1,11),4)^1 = rotl(0x801,4)^1 = 0x8010^1 = 0x8011 = 32785.
        CHECK(static_cast<uint32_t>(sys.prng_step_a()) == 0x8011u);
        CHECK(sys.prng_a == 0x8011u);             // state advanced
        CHECK((0x8011u & 0xFFFFu) % 62 == 49);    // the %62 jitter the engagement adds

        // PRNG_Next16 is the same algorithm over a separate stream (dword_31BFBB0).
        sys.prng16 = 1;
        CHECK(static_cast<uint32_t>(sys.prng_step16()) == 0x8011u);
        CHECK(sys.prng_a == 0x8011u);             // stepping 16 did NOT touch stream a (independent)
    }

    // ---- ai_score_target: byte-exact FOV/range/stealth/priority scoring ----
    {
        // Centered (angle 0), close (dist 50), fully visible: primary FOV path.
        // primary_fov=0x41 (=65), gate (65|2)>>1=33; angle 0<33 -> angle_score=((65)<<16)/65=0x10000.
        // range=0x640000/50=0x20000; stealth=0x10000; priority=0x10000 -> score 0x20000=131072.
        CHECK(ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 0, 0) == 131072);

        // Outside both FOV gates -> -1 (gate-fail sentinel, distinct from an in-gate score of 0).
        CHECK(ai_score_target(50, 50, 0x41, 0x41, 100, 100, 100, 100, 0, 0) == -1);

        // Beyond range -> -1 even when perfectly centered.
        CHECK(ai_score_target(0, 200, 0x41, 0x41, 100, 100, 100, 100, 0, 0) == -1);

        // Secondary FOV path (fails primary angle, passes secondary): scores > 0, < the centered max.
        int sec = ai_score_target(35, 50, 0x41, 0x51, 100, 100, 100, 100, 0, 0);
        CHECK(sec > 0 && sec < 131072);

        // Stealth: visibility >= 16 zeroes the score (in-gate 0, NOT gate-fail -1); partial scales it.
        CHECK(ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 16, 0) == 0);
        int dim = ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 8, 0);
        CHECK(dim > 0 && dim < 131072);                            // stealth 0xFF00 < 0x10000

        // Priority flag (&0x4000) applies the 6.0x weight (393216/0x10000).
        CHECK(ai_score_target(0, 50, 0x41, 0x41, 100, 100, 100, 100, 0, 0x4000) == 131072 * 6);
    }

    // ---- acquire_target: best-of, team filter, LOS, priority bypass ----
    {
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.heading = 0;
        e.team = 1;
        e.profile.fov_primary = 0x40;     // -> 0x41
        e.profile.fov_secondary = 0x40;
        e.profile.range_primary = 1000;
        e.profile.range_secondary = 1000;

        // Far enemy (dist large but in range) and near enemy (better range_score); both centered.
        AiCandidate far{};
        far.handle = EntityHandle::make(1, 5);
        far.team = 2; far.pos[0] = 500 << 16; far.health = 100;
        far.range_primary = 1000; far.range_secondary = 1000;
        far.relmat_id = 0x11; far.net_id = 0x111;
        AiCandidate near{};
        near.handle = EntityHandle::make(1, 6);
        near.team = 2; near.pos[0] = 100 << 16; near.health = 100;
        near.range_primary = 1000; near.range_secondary = 1000;
        near.relmat_id = 0x22; near.net_id = 0x222; near.has_controller = true;

        AiTarget out{};
        CHECK(sys.acquire_target_from(e, {far, near}, out) == true);
        CHECK(sys.find_target_calls == 1);
        CHECK(out.net_id == 0x222);          // nearer -> higher range_score -> chosen
        CHECK(out.relmat_id == 0x22);
        CHECK(out.has_controller == true);

        // Same-team candidate is filtered out -> no target.
        AiCandidate same{};
        same.handle = EntityHandle::make(1, 7);
        same.team = 1;          // same team as e
        same.pos[0] = 100 << 16; same.health = 100;
        same.range_primary = 1000; same.range_secondary = 1000;
        AiTarget none{};
        CHECK(sys.acquire_target_from(e, {same}, none) == false);

        // LOS blocked on the only enemy -> no target.
        AiCandidate blocked = near;
        blocked.los_blocked = true;
        CHECK(sys.acquire_target_from(e, {blocked}, none) == false);

        // Priority target (in-gate) bypasses scoring and returns immediately on LOS.
        AiCandidate prio = near;
        prio.is_priority = true; prio.net_id = 0x333; prio.relmat_id = 0x33;
        prio.pos[0] = 900 << 16; // would score worse than a closer one, but priority wins
        AiCandidate closer = near;
        closer.net_id = 0x444; closer.pos[0] = 50 << 16;
        AiTarget pout{};
        CHECK(sys.acquire_target_from(e, {closer, prio}, pout) == true);
        CHECK(pout.net_id == 0x333);         // priority bypass beats the closer non-priority

        // [grill fix: priority bypass @0x467350 is reached only PAST the FOV/range gate]
        // A priority target OUTSIDE the engage range is skipped; an in-range non-priority enemy wins.
        AiCandidate prio_far{};
        prio_far.handle = EntityHandle::make(1, 20);
        prio_far.team = 2; prio_far.pos[0] = 5000 << 16; prio_far.health = 100;
        prio_far.range_primary = 1000; prio_far.range_secondary = 1000;
        prio_far.is_priority = true; prio_far.net_id = 0x888;  // dist 5000 > range 1000 -> gate-fail
        AiCandidate inrange{};
        inrange.handle = EntityHandle::make(1, 21);
        inrange.team = 2; inrange.pos[0] = 100 << 16; inrange.health = 100;
        inrange.range_primary = 1000; inrange.range_secondary = 1000;
        inrange.net_id = 0x999;
        AiTarget gout{};
        CHECK(sys.acquire_target_from(e, {prio_far, inrange}, gout) == true);
        CHECK(gout.net_id == 0x999);         // out-of-gate priority skipped; in-range enemy chosen
    }

    // ---- death event on a non-authority in-session client zeroes health before the death tick ----
    {
        World w;
        AiSystem sys;
        sys.is_authority = false;
        sys.is_in_session = true;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.has_physics = false;
        e.health = 100;                                    // still "alive" coming in
        e.vel_x = 2000; e.vel_z = 0;                       // crash speed (>= 1057)
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // tick = h_ground_followwp_tick
        sys.process_infantry_state_machine(e, w, 4);
        CHECK(e.health == 0);                              // zeroed before the tick [orig @0x45827f]
        bool saw_death = false, saw_20 = false;            // death tick (3) + SM's type-20 event
        for (int i = 0; i < sys.events.count(); ++i) {
            if (sys.events.at(i).type() == 3) saw_death = true;
            if (sys.events.at(i).type() == 20) saw_20 = true;
        }
        CHECK(saw_death);                                  // death PATH ran (not the alive path)
        CHECK(saw_20);
    }

    // ---- engage_target: 8 relation ops in order, target set, fire-delay jitter, pending 17 ----
    {
        World w;
        AiSystem sys;
        sys.prng16 = 1;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.relmat_id = 0x1234;
        e.net_id = 0x9999;
        e.profile.field104 = 0;
        AiTarget t{0x55, 0x66, /*has_controller=*/false};

        sys.engage_target(w, e, t);

        CHECK(e.brain.f[AiBrain::kPendState] == 17);   // GROUND_COMBAT
        CHECK(e.brain.f[AiBrain::kCombatTimer] == 0);
        // branch B: always jitter via PRNG_Next16; field104=0 -> delay = 0 + 49.
        CHECK(e.brain.f[AiBrain::kFireDelay] == 49);
        CHECK(sys.target_set_calls.size() == 1 && sys.target_set_calls[0] == 0x66);
        CHECK(sys.rel_ops.size() == 8);
        CHECK(sys.rel_ops[0].op == kRelEventSpecial && sys.rel_ops[0].a == 0x1234 && sys.rel_ops[0].b == 0x55);
        CHECK(sys.rel_ops[1].op == kRelSharedMem    && sys.rel_ops[1].a == 0x9999 && sys.rel_ops[1].b == 0x55);
        CHECK(sys.rel_ops[2].op == kRelProximity    && sys.rel_ops[2].a == 0x1234 && sys.rel_ops[2].b == 0x66);
        CHECK(sys.rel_ops[3].op == kRelEnemy        && sys.rel_ops[3].a == 0x9999 && sys.rel_ops[3].b == 0x66);
        CHECK(sys.rel_ops[4].op == kRelAllied       && sys.rel_ops[4].a == 0x1234 && sys.rel_ops[4].b == 0x55);
        CHECK(sys.rel_ops[5].op == kRel452B30       && sys.rel_ops[5].a == 0x9999 && sys.rel_ops[5].b == 0x55);
        CHECK(sys.rel_ops[6].op == kRelDamaged      && sys.rel_ops[6].a == 0x1234 && sys.rel_ops[6].b == 0x66);
        CHECK(sys.rel_ops[7].op == kRelSpotted      && sys.rel_ops[7].a == 0x9999 && sys.rel_ops[7].b == 0x66);
    }

    // ---- engage_target: branch A (has_controller) guards jitter by base-delay; sign-extends relmat ----
    {
        World w;
        AiSystem sys;
        sys.prng_a = 1;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.relmat_id = 0x8000;             // high bit set -> (int16) sign-extends to -32768
        e.profile.field104 = 0;           // base delay 0 -> branch A skips jitter entirely
        AiTarget t{0x55, 0x66, /*has_controller=*/true};
        sys.engage_target(w, e, t);
        CHECK(e.brain.f[AiBrain::kFireDelay] == 0);     // no jitter when base_delay == 0
        CHECK(sys.prng_a == 1);                          // stream A untouched (jitter skipped)
        CHECK(sys.rel_ops[0].a == -32768);               // movsx of 0x8000

        // Now with a base delay: branch A jitters via stream A (49) and advances it.
        AiSystem sys2;
        sys2.prng_a = 1;
        int i2 = sys2.attach(EntityHandle::make(0, 0));
        AiEntity &e2 = *sys2.at(i2);
        e2.profile.field104 = 100;
        AiTarget t2{1, 2, /*has_controller=*/true};
        sys2.engage_target(w, e2, t2);
        CHECK(e2.brain.f[AiBrain::kFireDelay] == 149);   // 100 + 49
        CHECK(sys2.prng_a == 0x8011u);                   // stream A advanced
    }

    // ---- state-16 tick: a visible enemy -> engage (pending 17) instead of walking ----
    // The feed now scans the registry (D-AI-1): spawn a real pool-1 enemy.
    {
        World w;
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 16);
        Entity self_seed;
        self_seed.team = 1;
        self_seed.health = 100;
        EntityHandle self_h = w.registry.spawn(0, self_seed);
        Entity enemy_seed;
        enemy_seed.team = 2;
        enemy_seed.health = 100;
        enemy_seed.position = Vec3{100.0f, 0.0f, 0.0f};
        enemy_seed.net_id = 0x77; // single key (SSN)
        enemy_seed.group_id = 3;
        EntityHandle enemy_h = w.registry.spawn(1, enemy_seed);
        CHECK(enemy_h.valid());

        AiSystem sys;
        int idx = sys.attach(self_h);
        AiEntity &e = *sys.at(idx);
        e.team = 1;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.profile.fov_primary = 0x40;
        e.profile.fov_secondary = 0x40;
        e.profile.range_primary = 1000;
        e.profile.range_secondary = 1000;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundFollowWp).tick(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 17);     // engaged
        CHECK(sys.rel_ops.size() == 8);
        CHECK(sys.target_set_calls.size() == 1 && sys.target_set_calls[0] == 0x77);
        CHECK(e.brain.f[AiBrain::kOutSpeed] != 10);      // mover did NOT run (no walk)
        // D-AI-3 closed: the engage APPLIED the sees+targeted quads (keys: group/SSN).
        const Entity *self_e = w.registry.get(self_h);
        CHECK(w.relations.single_single(TriggerRelations::kSees, self_e->net_id, 0x77));
        CHECK(w.relations.single_single(TriggerRelations::kTargeted, self_e->net_id, 0x77));
        // Entity_SetAITarget maintained the target's +530 refcount.
        CHECK(w.registry.get(enemy_h)->ai_target_refcount == 1);
    }

    // ---- state-18 patrol tick: arrival clears the goal; otherwise fallback vs engage ----
    {
        World w;
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundEvade; // 18
        e.brain.f[AiBrain::kWorkHeading] = 1000;        // brain[132] target heading
        e.arrival_prox = 100;
        e.heading = 1050;                                // within [900,1100] -> arrived
        e.patrol_goal = 1;
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.patrol_goal == 0);                       // cleared on arrival
        CHECK(e.brain.f[AiBrain::kPendState] == 0);      // no transition yet (goal-clear branch)

        // Goal still set but heading out of range -> goal stays, no transition.
        e.patrol_goal = 1;
        e.heading = 2000;
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.patrol_goal == 1);
        CHECK(e.brain.f[AiBrain::kPendState] == 0);

        // No goal + fallback flag -> pending = fallback state (brain[6]).
        e.patrol_goal = 0;
        e.profile.flags100 = 2;
        e.brain.f[AiBrain::kFallback] = 22;
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 22);

        // No goal + no fallback flag -> pending = 17 (GROUND_COMBAT).
        e.patrol_goal = 0;
        e.profile.flags100 = 0;
        e.brain.f[AiBrain::kPendState] = 0;
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 17);
    }

    // ---- state-18 patrol tick: death queues an event; fire timer decrements ----
    {
        World w;
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.brain.f[AiBrain::kCurState] = kAiGroundEvade;
        e.health = 0;
        e.vel_x = 2000; e.vel_z = 0;          // >= 1057 -> crash death (3)
        AiThinkCtx ctx{&sys, &e, &w, nullptr};
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(sys.events.count() == 1 && sys.events.at(0).type() == 3);

        e.health = 100;
        e.profile.flags96 = 0x10;             // can-fire
        e.brain.f[AiBrain::kFireTimer] = 50;
        e.brain.f[AiBrain::kStep] = 64;
        e.patrol_goal = 1;                    // stay in the goal branch (no transition)
        e.brain.f[AiBrain::kWorkHeading] = 0; e.arrival_prox = 1; e.heading = 1000; // not arrived
        int before = sys.unported_calls;
        sys.row(kAiGroundEvade).tick(ctx);
        CHECK(sys.unported_calls == before + 1);          // compute-fire-positions stub
        CHECK(e.brain.f[AiBrain::kFireTimer] == 50 - 64); // -= step
    }

    // ---- combat event handler (states 16/17/18 event): damage/death/destroy transitions ----
    {
        World w;
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);

        // type 1 (damage): pending = 18 + brain[39] = event[3], when not dead/suppressed.
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kPendState] = 0;
        e.profile.flags96 = 0; // not suppressed
        AiEventEntry dmg{};
        dmg.f[0] = 1; dmg.f[3] = 0xDEAD;
        AiThinkCtx ctx{&sys, &e, &w, &dmg};
        sys.row(kAiGroundFollowWp).event(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 18);
        CHECK(e.brain.f[AiBrain::kDamageInfo] == 0xDEAD);

        // type 1 but already transitioning to 21 -> no 18 (still stashes damage info).
        e.brain.f[AiBrain::kPendState] = 21;
        e.brain.f[AiBrain::kDamageInfo] = 0;
        sys.row(kAiGroundCombat).event(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 21);  // unchanged
        CHECK(e.brain.f[AiBrain::kDamageInfo] == 0xDEAD);

        // type 1 but suppressed (flags96 & 2) -> no 18.
        e.brain.f[AiBrain::kPendState] = 0;
        e.profile.flags96 = 2;
        sys.row(kAiGroundEvade).event(ctx);
        CHECK(e.brain.f[AiBrain::kPendState] == 0);

        // type 3 (death) -> 21; type 4 (destroy) -> 23.
        AiEventEntry death{}; death.f[0] = 3;
        AiThinkCtx ctx3{&sys, &e, &w, &death};
        sys.row(kAiGroundFollowWp).event(ctx3);
        CHECK(e.brain.f[AiBrain::kPendState] == 21);
        AiEventEntry destroy{}; destroy.f[0] = 4;
        AiThinkCtx ctx4{&sys, &e, &w, &destroy};
        sys.row(kAiGroundCombat).event(ctx4);
        CHECK(e.brain.f[AiBrain::kPendState] == 23);
    }

    // ---- locomotion: apply the mover output (advance toward target, clamp, face heading) ----
    {
        AiSystem sys;
        sys.loco_scale = 65536; // 1.0 in 16.16: out_speed N -> N world-units / tick
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.pos[0] = 0; e.pos[1] = 0;
        e.brain.f[AiBrain::kOutSpeed] = 3;          // 3 units this tick
        e.brain.f[AiBrain::kWorkPosX] = 100 << 16;  // target X
        e.brain.f[AiBrain::kWorkPosY] = 0;
        e.brain.f[AiBrain::kWorkHeading] = 12345;   // BAM heading from the mover
        sys.apply_locomotion(e);
        CHECK(e.pos[0] == (3 << 16));   // advanced 3 units toward the target
        CHECK(e.pos[1] == 0);
        CHECK(e.heading == 12345);      // entity now faces the mover heading

        // A large speed arrives exactly at the target (clamp, no overshoot).
        e.brain.f[AiBrain::kOutSpeed] = 100000;
        sys.apply_locomotion(e);
        CHECK(e.pos[0] == (100 << 16));

        // Out-speed 0 (frozen / engaging) leaves the entity put.
        e.brain.f[AiBrain::kOutSpeed] = 0;
        e.pos[0] = 42;
        sys.apply_locomotion(e);
        CHECK(e.pos[0] == 42);
    }

    // ---- slice-1 exit: an NPC rifleman kills the player through the authoritative
    // round path (perception -> attack anim -> .bad fire events -> ring + RoundSim ->
    // damage -> death), on the listen-server tick shape (AI tick + round tick). ----
    // [witness: world-wac-ai-re §17; net-re §5.60]
    {
        // A root-motion double whose attack clip carries the .bad fire trigger (bit 0x4)
        // every frame; idle carries none. [orig: g_animEventTriggerBits @0xA2ED08]
        struct FiringSource : IRootMotionSource {
            bool has_clip(int, int id) const override {
                return id == anim_state::kIdle || id == anim_state::kAttack;
            }
            int32_t clip_length_ticks(int, int) const override { return -1; }
            bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
                if (!has_clip(0, id)) return false;
                ++phase;
                out = RootMotionFrame{};
                if (id == anim_state::kAttack) out.events = 0x4; // trigger-pull frames
                return true;
            }
        };
        static FiringSource fire_src;

        World w;
        w.registry.configure_pool(0, 8);
        w.registry.configure_pool(1, 8);
        // One rifle round in the ammo table (index 0 is the null entry by convention;
        // use index 1). Velocity 800 u/s, heavy enough to kill in a few hits.
        w.ammo.entries.resize(2);
        w.ammo.entries[1].name = "AMMO_TEST_556";
        w.ammo.entries[1].velocity = 800;
        w.ammo.entries[1].max_age_ticks = 124;
        w.ammo.entries[1].weight_in_grains = 875; // damage = speed_scaled (clamped 1219)
        w.ammo.entries[1].min_damage = 10;
        w.ammo.entries[1].max_damage = 40;
        w.ammo.entries[1].valid = true;

        // The player: pool 0, team 2, 20 u east of the NPC, at ground height 0.
        Entity player_seed;
        player_seed.kind = EntityKind::Organic;
        player_seed.has_item_def = true;
        player_seed.item_type = 3;
        player_seed.team = 2;
        player_seed.health = 100;
        player_seed.net_id = 0x21;
        player_seed.group_id = 2;
        player_seed.position = Vec3{20.0f, 0.0f, 0.0f};
        EntityHandle player_h = w.registry.spawn(0, player_seed);
        CHECK(player_h.valid());

        // The NPC rifleman: pool 0, team 1, armed via the D-AI-5 profile seed.
        Entity npc_seed;
        npc_seed.kind = EntityKind::Organic;
        npc_seed.team = 1;
        npc_seed.health = 100;
        npc_seed.net_id = 0x11;
        npc_seed.group_id = 1;
        npc_seed.position = Vec3{0.0f, 0.0f, 0.0f};
        EntityHandle npc_h = w.registry.spawn(0, npc_seed);
        CHECK(npc_h.valid());

        AiSystem sys;
        sys.is_authority = true;
        sys.root_motion = &fire_src;
        int idx = sys.attach(npc_h);
        AiEntity &npc = *sys.at(idx);
        npc.inf.active = true;
        npc.team = 1;
        npc.net_id = 0x11;
        npc.pos[0] = 0; npc.pos[1] = 0; npc.pos[2] = 0;
        npc.profile.ammo_primary = 1;          // -> w.ammo[1]
        npc.profile.clip_size = 30;
        npc.inf.magazine = 30;
        npc.slot.f[10] = 0;                    // perfect accuracy (w_accuracy 100)
        npc.slot.f[11] = 0;
        npc.slot.f[15] = 60 << 16;             // attack range 60 u
        npc.slot.f[16] = 10 << 16;             // approach range
        npc.slot.f[17] = 100 << 16;            // sight range 100 u
        npc.slot.f[22] = 62;                   // cooldown base (~1 s)

        TickContext tctx;
        tctx.world = &w;
        tctx.is_authority = true;
        bool acquired = false, fired = false, killed = false;
        for (uint32_t t = 0; t < 2000 && !killed; ++t) {
            tctx.logic_tick = t;
            sys.tick(w, tctx);
            w.round_sim.tick(w, nullptr, nullptr);
            if (npc.inf.combat_target == player_h) acquired = true;
            if (w.rounds.count > 0) fired = true;
            for (const RoundDeath &d : w.round_sim.deaths)
                if (d.victim == player_h && d.killer == npc_h) killed = true;
        }
        CHECK(acquired);                        // the 32-tick perception found the player
        CHECK(fired);                           // rounds entered the ring (the 0x0A fan-out)
        CHECK(w.round_sim.deaths.size() >= 1);  // the damage pass detected the death
        CHECK(killed);                          // ...credited NPC -> player
        CHECK(w.registry.get(player_h)->health <= 0);
        // The engagement left the witnessed side effects: the sees+targeted quads
        // (group/SSN keys) and the shooter's priority mark from firing.
        CHECK(w.relations.single_single(TriggerRelations::kSees, 0x11, 0x21));
        CHECK(w.relations.single_single(TriggerRelations::kTargeted, 0x11, 0x21));
        CHECK(w.relations.group_group(TriggerRelations::kSees, 1, 2));
        CHECK((w.registry.get(npc_h)->engine_flags & 0x4000u) != 0);
        // The kill staged the bullet death-anim selection on the victim at damage
        // time: torso group (the bone stand-in, D-AI-9) = 184..187 by quadrant, and
        // the victim's group went alert red. [orig: Entity_HandleDamageTrigger
        // @0x407478/@0x4073ea; world-wac-ai-re §19]
        const int sel = w.registry.get(player_h)->death_anim_state;
        CHECK(sel >= 184 && sel <= 187);
        CHECK(w.relations.group(2).alert == TriggerRelations::kAlertRed);
    }

    test_vehicle_death_rows();
    test_fire_pass_uses_embedder_fed_muzzle();
    test_world_feed_never_engages_same_team();
    test_berserk_candidate_is_intentional_team_exception();
    test_damage_hit_sets_retail_alert_state();
    test_remote_player_hit_skips_npc_group_alert();
    test_mounted_gunner_acquires_and_fires();
    test_mounted_fire_uses_retail_range_and_spatial_stagger();
    test_mounted_look_traverses_before_fire_request();
    test_mounted_gunner_dismounts_into_death_animation();
    test_mounted_collision_tail_uses_retail_eight_tick_phase_without_models();
    test_joiner_evaluates_vehicle_idle_without_integrating_motor();
    test_lethal_hit_blends_into_death_animation_without_position_jump();

    if (failures == 0) std::printf("ai: all tests passed\n");
    return failures ? 1 : 0;
}
