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

// The D-AI-6 muzzle seam: a FRESH host-fed posed muzzle replaces the chest-lift
// origin for spawned rounds; absent or stale stamps fall back. [orig: the
// anim-event fire spawns from Entity_GetAttachmentWorldPosition @0x4b2670 —
// the posed gun-flash userpoint; our host present layer feeds it back.]
static void test_fire_pass_uses_host_fed_muzzle() {
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
        e.relmat_id = 0x1234;
        e.net_id = 0x9999;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp; // 16 -> moveSpeed = kSpeedB
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 1;
        e.brain.f[AiBrain::kWpNode] = 0;
        e.brain.f[AiBrain::kSpeedB] = 20;
        e.brain.f[AiBrain::kStep] = 64;

        sys.update_waypoint_movement(e);

        // dist=600 (|dz|=600 base) < nodeVal=1000 -> advance.
        CHECK(e.brain.f[AiBrain::kWpNode] == 1);
        CHECK(e.brain.f[AiBrain::kStoredKeyTime] == 1000);
        CHECK(sys.relmat_calls.size() == 2);
        CHECK(sys.relmat_calls[0].which == 1);                    // SetBitB first
        CHECK(sys.relmat_calls[0].key == 0x1234);                 // relmat_id
        CHECK(sys.relmat_calls[1].which == 0);                    // SetBitA second
        CHECK(sys.relmat_calls[1].key == 0x9999);                 // net_id
        CHECK(sys.relmat_calls[0].channel == 1 && sys.relmat_calls[0].node == 0);
        // working transform from the resolved node; out-speed halved (timeDelta<step*speed).
        CHECK(e.brain.f[AiBrain::kWorkPosX] == 500);
        CHECK(e.brain.f[AiBrain::kWorkPosY] == 600);
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 10);               // 20 >> 1
    }

    // ---- path follower: loop-wrap vs one-shot terminate at path end ----
    {
        for (int loopflag = 0; loopflag <= 1; ++loopflag) {
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

            sys.update_waypoint_movement(e);

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
        sys.update_waypoint_movement(e);
        // dist=2000 >= nodeVal=1000 -> no advance. timeDelta=1000. state17 threshold 16*20=320;
        // 1000 >= 320 -> NOT halved -> speed stays 20 (proves kSpeedA source + 16x threshold).
        CHECK(e.brain.f[AiBrain::kOutSpeed] == 20);
        CHECK(e.brain.f[AiBrain::kWpNode] == 0); // never advanced
    }

    // ---- contrast: state-16 with the SAME timeDelta halves (threshold speed*step=1280) ----
    {
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
        sys.update_waypoint_movement(e);
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
        AiSystem sys;
        int idx = sys.attach(EntityHandle::make(0, 0));
        AiEntity &e = *sys.at(idx);
        e.pos[0] = 11; e.pos[1] = 22; e.pos[2] = 33;
        e.heading = 44; e.pitch = 55; e.roll = 66;
        e.brain.f[AiBrain::kCurState] = kAiGroundFollowWp;
        e.brain.f[AiBrain::kWpType] = 1;
        e.brain.f[AiBrain::kWpChannel] = 0; // navMeshId 0 -> solver returns -1 -> freeze
        e.brain.f[AiBrain::kSpeedB] = 20;
        sys.update_waypoint_movement(e);
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
    test_fire_pass_uses_host_fed_muzzle();

    if (failures == 0) std::printf("ai: all tests passed\n");
    return failures ? 1 : 0;
}
