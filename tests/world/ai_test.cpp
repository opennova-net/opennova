// AI subsystem foundation tests: state enum, struct layout, AIEvent ring, the
// AI_BeginUpdate budget gate, the infantry state-machine dispatcher + transitions,
// and the byte-exact trivial handler ports. Driven by a manual World + tick.
#include <cstdio>
#include <cstring>

#include "world/ai.h"
#include "world/world.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static bool streq(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

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
        e.brain.f[AiBrain::kCurState] = kAiGroundCombat; // 17, tick handler = not_yet_ported
        e.brain.f[AiBrain::kPendState] = kAiGroundCombat;
        TickContext ctx;
        ctx.world = &w;
        ctx.is_authority = true;
        sys.tick(w, ctx);
        CHECK(sys.unported_calls >= 1); // state-17 tick routed through the stub
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

    if (failures == 0) std::printf("ai: all tests passed\n");
    return failures ? 1 : 0;
}
