// The authoritative capture-zone transaction. A lightweight per-logic-tick
// contact pass feeds one persistent request/active-capture state; the 1 Hz pass
// performs secure/control updates, instant numbered flips, timed unnumbered
// captures, zone-team enforcement, and produces the exact wire-facing events.
// All of it models the retail
// `Server_TickUpdate @0x51D7E0` g_periodic_second_timer block (@0x51DF50..0x51DF8C):
// proximity -> secure pass (0x6F + 0x1E 0x3B/0x3C) -> team enforcement -> the
// timed-capture engine's queue drain (instant numbered flips + GameEvent_FlagCapture).
//
// The world side PRODUCES events; the host (npruntime Server_TickUpdate) encodes them
// onto the wire (0x6F / 0x53 / 0x1E / 0x40). [orig: Server_UpdateCaptureZoneProximity
// @0x5086A0; Server_UpdateCaptureZoneEntities @0x519690;
// calculate_capture_zone_control_delta @0x501120; Server_UpdateCaptureZones @0x53B8F0;
// GameEvent_FlagCapture @0x50F6F0; Server_EnforceZoneEntityTeams @0x519600]
// Contact production deliberately lives beside the host movement snapshots: a
// remote authority Player is net-snapped and does not traverse the local physics
// resolver, but its retail MoveOrder moving bit still gates the same overlap.
// This preserves the original collision semantics without a second remote-only
// objective path. [orig: Entity_MovementCollisionResolver @0x4B2BD0,
// capture callback callsite @0x4B2F90..0x4B2FD0]
#ifndef OPENNOVA_WORLD_ZONE_CAPTURE_H
#define OPENNOVA_WORLD_ZONE_CAPTURE_H

#include <cstdint>
#include <vector>

#include "world/entity.h"
#include "world/zone_chain.h"

namespace opennova::world {

class World;

// World-owned retail CaptureCtx state. Requests survive until the next 1 Hz
// drain; active entries survive until completion, cancellation, or mission reset.
// [orig: CaptureCtx_* @0x53B340..0x53B880]
struct ZoneCaptureState {
    struct Request {
        EntityHandle zone;
        uint8_t team = 0;
        EntityHandle capturer;
    };
    struct Active {
        EntityHandle zone;
        uint8_t team = 0;
        int32_t progress = 0;
        int32_t limit = 0;
        EntityHandle capturer;
        std::vector<EntityHandle> presence;
        uint8_t rate = 1;
    };

    std::vector<Request> requests;
    std::vector<Active> active;

    void clear() {
        requests.clear();
        active.clear();
    }
};

// One pass's outputs, drained by the host's 1 Hz wire block.
struct ZoneCaptureEvents {
    // Per registered zone, EVERY pass — the S2C 0x6F body fields (15 B:
    // [u16 handle][u8 team][i32 control][i32 0x10000][i16 delta][u8 friendlies]
    // [u8 enemies]) [orig: NetPacket_WriteZoneTimerValue @0x506E70, emit @0x5197D9].
    struct Control {
        EntityHandle zone;
        uint8_t team = 0;
        int32_t control = 0;   // 16.16 fraction 0..0x10000
        int16_t delta = 0;
        uint8_t friendlies = 0;
        uint8_t enemies = 0;
    };
    // Secure edges — 0x1E event 0x3B (59, became fully secured) / 0x3C (60, dropped
    // to zero): attacker byte = the zone's spawn-zone-list index, victim byte = the
    // zone's team [orig: @0x519839 / @0x51988E].
    struct Secure {
        EntityHandle zone;
        uint8_t zone_team = 0;
        bool secured = false; // true = 0x3B, false = 0x3C
    };
    // A numbered-zone INSTANT flip [orig: the queue drain @0x53B8F0 — numbered zones
    // flip immediately: team change (via neutral when previously owned), control = 0,
    // GameEvent_FlagCapture]. frontier_changed selects the 0x1E pair: 50/51 when the
    // frontier masks held, 52/53 carrying each side's NEW frontier number; a 56/57
    // banner keyed on the new owning team follows either way [orig: @0x50F6F0].
    struct Flip {
        EntityHandle zone;
        // Exact Player whose eligible touch queued the capture. Stable pool
        // order breaks ties between Players on the same uncontested team.
        // [orig: Server_OnPlayerTouchCaptureZone @0x500BA0 passes the Player to
        // Server_UpdateCaptureZones @0x53B8F0 / GameEvent_FlagCapture @0x50F6F0]
        EntityHandle capturer;
        // Every living teammate in the numbered zone when it flips receives
        // scorer event 24; this can include more Players than `capturer`.
        // [orig: CaptureZone_CheckProximityScoring @0x500C50, call @0x53BC94]
        std::vector<EntityHandle> scorers;
        uint8_t old_team = 0;
        uint8_t new_team = 0;
        uint8_t capturer_team = 0;  // the team whose presence drove the flip
        bool frontier_changed = false;
        uint8_t capturer_frontier = 0; // FindFrontierZone AFTER the flip
        uint8_t loser_frontier = 0;
        bool suppressed = false; // match decided (one team owns every zone)
        bool announce = false;   // ItemDefAttrib 0x40000 event gate
    };
    // Exact S2C 0x53 body for a timed unnumbered capture. Retail emits one at
    // start/restart and after every active 1 Hz advance, including completion.
    // [orig: NetPacket_WriteZoneTimerWindow @0x506D00;
    // Server_UpdateCaptureZones @0x53B8F0]
    struct TimerWindow {
        EntityHandle zone;
        uint8_t current_team = 0;
        uint8_t capturing_team = 0;
        uint16_t progress = 0;
        uint16_t limit = 0;
        uint8_t rate = 1;
    };
    // Exact S2C 0x6C body. The original compares the raw unique-presence count
    // with the stored rate, then clamps the advertised value to [1,32].
    // [orig: CaptureCtx_UpdateActiveCaptureRate @0x53B600;
    // NetPacket_WriteZonePresenceCount @0x506DE0]
    struct Presence {
        EntityHandle zone;
        uint8_t count = 1;
    };
    // Timed-capture start/restart produces 0x1E event 41/42; completion produces
    // 43/44. `capturer` is the pool-0 index source and remains the score actor.
    // [orig: Server_SendWeaponFireEvent @0x50F630;
    // GameEvent_FlagCapture @0x50F6F0]
    struct TimedStart {
        EntityHandle zone;
        EntityHandle capturer;
        uint8_t team = 0;
    };
    struct TimedCompletion {
        EntityHandle zone;
        EntityHandle capturer;
        uint8_t new_team = 0;
        bool announce = false; // ItemDefAttrib 0x40000 event gate
    };
    std::vector<Control> control;
    std::vector<Secure> secure_edges;
    std::vector<Flip> flips;
    std::vector<TimerWindow> timer_windows;
    std::vector<Presence> presence;
    std::vector<TimedStart> timed_starts;
    std::vector<TimedCompletion> timed_completions;

    void clear() {
        control.clear();
        secure_edges.clear();
        flips.clear();
        timer_windows.clear();
        presence.clear();
        timed_starts.clear();
        timed_completions.clear();
    }
};

struct ZoneCaptureDeltaInput {
    int presence = 0;
    int capturing_side_players = 0;
    int total_players = 0;
    int speed_setting = -1;
    int shared_zone_entities = 1;
    int capturing_side_zones = 0;
    int opposing_side_zones = 0;
    int numbered_spawn_zones = 0;
    int32_t remaining_ticks = -1;
    uint32_t game_time_minutes = 0;
};

// The complete control-delta formula [orig:
// calculate_capture_zone_control_delta @0x501120]: presence, player-count
// shaping, the late-round ownership-leader acceleration, shared-zone division,
// and minimum signed delta. Exposed as one input value for exact formula pins.
int32_t zone_capture_control_delta(const ZoneCaptureDeltaInput &input);

// Per-logic-tick movement-contact producer. It updates active presence and queues
// one request per zone/team; it performs no ownership transition itself.
void zone_capture_contact_tick(World &world);

// One 1 Hz capture transaction. Reads its configuration and persistent state
// from World, emits all semantic wire events, and drains pending requests.
void zone_capture_second_tick(World &world, ZoneCaptureEvents &out);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ZONE_CAPTURE_H
