// The Advance & Secure 1 Hz capture loop — slice 2 of the §5.61 witness: the
// per-second secure/control pass, the instant numbered-zone flips, the flip/secure
// event stream, and zone-team enforcement. All of it models the retail
// `Server_TickUpdate @0x51D7E0` g_periodic_second_timer block (@0x51DF50..0x51DF8C):
// proximity -> secure pass (0x6F + 0x1E 0x3B/0x3C) -> team enforcement -> the
// timed-capture engine's queue drain (instant numbered flips + GameEvent_FlagCapture).
//
// The world side PRODUCES events; the host (npruntime Server_TickUpdate) encodes them
// onto the wire (0x6F / 0x53 / 0x1E / 0x40). [orig: Server_UpdateCaptureZoneProximity
// @0x5086A0; Server_UpdateCaptureZoneEntities @0x519690;
// calculate_capture_zone_control_delta @0x501120; Server_UpdateCaptureZones @0x53B8F0;
// GameEvent_FlagCapture @0x50F6F0; Server_EnforceZoneEntityTeams @0x519600]
//
// Tracked deferrals (D-NET-162): the timed-capture engine's ACTIVE entries
// (un-numbered flag zones — ASH_I5A authors numbered bunkers only; 0x6C presence
// counts ride those entries), the spawn-wave reset on flip, per-touch capture
// requests through the physics pass (our request source is the same 1 Hz proximity
// sample the drain consumes), the underdog catch-up term (needs the round clock —
// unplumbed), the def+88&2 in-radius team conversion (attrib2 untracked on entities),
// proximity scoring / 0x81 render-state sync, and the win-condition suppression's
// round-end handoff (we only suppress events once one team owns every zone).
#ifndef OPENNOVA_WORLD_ZONE_CAPTURE_H
#define OPENNOVA_WORLD_ZONE_CAPTURE_H

#include <cstdint>
#include <vector>

#include "world/entity.h"
#include "world/zone_chain.h"

namespace opennova::world {

class World;

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
        uint8_t old_team = 0;
        uint8_t new_team = 0;       // 0 = neutralized (was enemy-owned)
        uint8_t capturer_team = 0;  // the team whose presence drove the flip
        bool frontier_changed = false;
        uint8_t capturer_frontier = 0; // FindFrontierZone AFTER the flip
        uint8_t loser_frontier = 0;
        bool suppressed = false; // match decided (one team owns every zone)
    };
    std::vector<Control> control;
    std::vector<Secure> secure_edges;
    std::vector<Flip> flips;

    void clear() {
        control.clear();
        secure_edges.clear();
        flips.clear();
    }
};

// The control-delta formula [orig: calculate_capture_zone_control_delta @0x501120]:
// presence = friendlies - frontier-eligible enemies (in radius); teamSize = the
// capturing side's playing count + (6 - total)/2 when total < 6, soft-capped
// x -> cap + (x - cap)/2 at 20/40/60; speed = teamSize * base (capture-speed setting
// 1 -> 24, 2 -> 48, else 12), divided by the zone-number share count; delta =
// 65536 * presence / speed, minimum magnitude 1. The underdog catch-up term is
// deferred (needs the round clock; D-NET-162). Exposed for the test pins.
int32_t zone_capture_control_delta(int presence, int capturing_side_players,
                                   int total_players, int speed_setting, int shared_n);

// One 1 Hz capture pass over the chain. Reads/writes Entity::zone_control and zone
// teams, rebuilds the chain masks on flips, enforces zone-numbered entity teams, and
// fills `out`. No-op on an empty chain.
void zone_capture_tick(World &world, ZoneChain &chain, ZoneCaptureEvents &out,
                       int capture_speed_setting = -1);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ZONE_CAPTURE_H
