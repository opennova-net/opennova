// The authoritative capture-zone transaction. A lightweight per-logic-tick
// contact pass feeds one persistent request/active-capture state; the 1 Hz pass
// performs secure/control updates, instant numbered flips, timed unnumbered
// captures, zone-team enforcement, and produces the exact wire-facing events.
// All of it models the retail
// `Server_TickUpdate @0x51D7E0` g_periodic_second_timer block (@0x51DF50..0x51DF8C):
// proximity -> secure pass (0x6F + 0x1E 0x3B/0x3C) -> team enforcement -> the
// timed-capture engine's queue drain (instant numbered flips + GameEvent_FlagCapture).
//
// The world side PRODUCES events; the host (inmatch Server_TickUpdate) encodes them
// onto the wire (0x6F / 0x50 / 0x53 / 0x6C / 0x1E). [orig: Server_UpdateCaptureZoneProximity
// @0x5086A0; Server_UpdateCaptureZoneEntities @0x519690;
// calculate_capture_zone_control_delta @0x501120; Server_UpdateCaptureZones @0x53B8F0;
// GameEvent_FlagCapture @0x50F6F0; Server_ChangeEntityTeam @0x518D70;
// Server_EnforceZoneEntityTeams @0x519600]
// Contact production deliberately lives beside the host movement snapshots: a
// remote authority Player is net-snapped and does not traverse the local physics
// resolver, but its retail MoveOrder moving bit still gates the same overlap.
// This preserves the original collision semantics without a second remote-only
// objective path. [orig: Entity_MovementCollisionResolver @0x4B2BD0 — the
// overlap test @0x4B2F8D..0x4B2FA5 feeding the capture touch
// @0x4B31DD..0x4B3238]
#pragma once

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

#include <runtime/world/entity.h>
#include <runtime/world/zone_chain.h>

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
    // Players whose touch of a numbered zone queued nothing because neither
    // team can capture it, in contact order; the host arms their slot's nag.
    // [orig: Server_OnPlayerTouchCaptureZone @0x500BA0 — the failed
    //  CaptureCtx_QueueCaptureRequest @0x500BFF..0x500C06, the numbered test
    //  @0x500C08..0x500C0E]
    std::vector<EntityHandle> refused_touches;

    void clear() {
        requests.clear();
        active.clear();
        refused_touches.clear();
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
    // Immutable S2C 0x50 snapshot. A capture can transition owned -> neutral ->
    // new owner in one drain, so looking the entity up after the transaction
    // would collapse two distinct records into the final team. The identity
    // pair is live only for Flags & 0x100 Players and zero for every objective.
    // [orig: Server_ChangeEntityTeam @0x518D70;
    // write_entity_handle_packet @0x506AD0]
    struct TeamChange {
        EntityHandle entity;
        uint8_t team = 0;
        uint16_t net_id = 0;
        uint8_t anim_slot = 0;
    };
    // An INSTANT flip (a numbered zone, or any zone under a zero capture
    // duration): team change (via neutral when previously owned), the capture
    // scoring, control = 0, then GameEvent_FlagCapture.
    // [orig: Server_UpdateCaptureZones @0x53BC46..0x53BC94]
    struct Flip {
        EntityHandle zone;
        // Exact Player whose eligible touch queued the capture. Stable pool
        // order breaks ties between Players on the same uncontested team.
        // [orig: Server_OnPlayerTouchCaptureZone @0x500BA0 passes the Player to
        // Server_UpdateCaptureZones @0x53B8F0 / GameEvent_FlagCapture @0x50F6F0]
        EntityHandle capturer;
        // A numbered zone's scorer event 24 recipients: every live roster
        // Player of the capturer's team inside the zone radius. An unnumbered
        // zone scores event 14 on the capturer instead (`takeover`). Both only
        // in team games, for a registered capturer, on an owned zone.
        // [orig: CaptureZone_CheckProximityScoring @0x500C50, the call @0x53BC72]
        std::vector<EntityHandle> scorers;
        bool takeover = false;
        uint8_t old_team = 0;
        uint8_t new_team = 0;       // the owner after the flip (the banner key)
        uint8_t capturer_team = 0;  // the capturer's entity team (the pair's filter)
        bool numbered = false;
        bool announce = false;      // ItemDefAttrib 0x40000 event gate [orig: @0x50F70B]
        // A numbered flip's S2C 0x1E pair, sent unless the match is decided or
        // the capturer is no registered Player: to the capturer's team, 51
        // [zone][rank] when the enemy mask held (`unchanged`) else 53
        // [zone][frontier]; to enemy_of(capturer team), 50 / 52 with the same
        // bytes. [orig: GameEvent_FlagCapture @0x50F737..0x50F912]
        bool decided = false;
        bool capturer_is_player = false;
        bool unchanged = false;
        uint8_t zone_number = 0;
        uint8_t rank = 0;
        uint8_t frontier = 0;       // FindFrontierZone(capturer team) after the rebuild
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
        // The completion's capture scoring: event 14 on the capturer (the zone
        // is unnumbered), under the same gates as a flip's.
        // [orig: CaptureZone_CheckProximityScoring @0x500C50, the call @0x53BA7D]
        bool takeover = false;
    };
    // A single sequence is load-bearing wire state. Retail sends directly from
    // each mutation callsite; parallel per-kind buckets lose neutral/new pairs
    // and reorder 0x50 relative to 0x53/0x1E.
    using Event = std::variant<Control, Secure, TeamChange, Flip, TimerWindow,
                               Presence, TimedStart, TimedCompletion>;
    std::vector<Event> ordered;

    void clear() {
        ordered.clear();
    }
};

// The inputs calculate_capture_zone_control_delta reads once its player census
// is done: the signed presence, the zone's owner, the in-game census by team,
// the capture speed setting, the sorted SpawnZoneList's numbered ownership
// counts and this zone's shared-number count, and the round clock.
// [orig: calculate_capture_zone_control_delta @0x501120]
struct ZoneCaptureDeltaInput {
    int presence = 0;                  // same-team minus capturable others in radius
    uint8_t zone_team = 0;             // entity+354
    std::array<int, 5> team_players{}; // in-game Players by team
    int speed_setting = -1;            // g_capture_speed_setting
    int spawn_zone_count = 0;          // SpawnZoneList_GetCount()
    int team1_zones = 0;               // numbered list entries owned by team 1
    int team2_zones = 0;               // numbered list entries owned by team 2
    int numbered_zones = 0;            // numbered list entries
    int shared_zone_entities = 0;      // numbered entries carrying this zone's number
    int32_t remaining_ticks = -1;      // g_round_time_remaining
    uint32_t game_time_minutes = 0;    // g_respawn_time (SET GameTime)
};

// The complete control-delta formula [orig:
// calculate_capture_zone_control_delta @0x501120]: the capturing side, its
// player-count shaping, the late-round acceleration for the side holding more
// numbered zones, the shared-number division, the x87 conversion, and the
// minimum signed delta. Exposed as one input value for exact formula pins.
int32_t zone_capture_control_delta(const ZoneCaptureDeltaInput &input);



} // namespace opennova::world
