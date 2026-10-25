// Water-surface crossing events: the host-side queue behind the S2C 0x34 fan.
//
// Retail emits a positioned effect/sound the moment a body or a hull crosses the
// water plane, once per crossing and not per frame, and fans it to every
// in-match player, dead or alive, so their clients spawn the same splash. The
// motor detects the edge and records it here; the network layer drains the
// queue and encodes 0x34. Keeping the detection in the motor and the encoding
// in net/ preserves the engine's no-Godot, no-wire-knowledge boundary and lets
// the edge logic be unit-tested without a socket.
//
// [orig: the water block of Entity_ProcessWheeledVehiclePhysics and its
//  light/tracked twins -> CEffectWorld_SpawnEmitterAtPosition +
//  Server_SendOverlayActionToAlive @0x50a1b0 (send_mask 128 = slot state 6/7)]
#pragma once

#include <cstdint>
#include <vector>

namespace opennova::world {

// One crossing: the position triple the S2C 0x34 carries, whose three words are
// the high halves of these 16.16 values. Each site hands the sender its own
// triple. The infantry sites pass the body's own position at the send: the AI
// soldier its float target, stored just ahead of the splash, the player body its
// risen z (the dive splash, under the surface line) or its clamped one (the
// entry splash). The vehicle contact sites pass their effect's triple: the
// submerged pad's x and y at the water plane. Only the local effect spawn puts
// an infantry splash on the plane, and it is not this queue's.
// [orig: NetPacket_WriteOverlayAction @0x505D50 - the triple's high words
//  @0x505DB7 / @0x505DCB / @0x505DE1; Entity_UpdateInfantryAI - the store
//  @0x4BFB84, `lea eax, [esi+4]` @0x4BFC2B, the send @0x4BFC40;
//  Entity_UpdateInfantryPlayerBody - `lea ebp, [esi+4]` @0x4B7CB7 / @0x4B7CD6,
//  the dive send @0x4B8265..0x4B8267 on the branch that skips the clamp
//  @0x4B817D, the entry send @0x4B82EA..0x4B82FC;
//  Entity_ProcessVehicleSuspension - the triple
//  @0x464AC7..0x464AD5 passed @0x464B24 / @0x464B35]
struct WaterCrossEvent {
    int32_t x = 0; // 16.16 world
    int32_t y = 0;
    int32_t z = 0;
    // Which of the two authored sounds the fan should carry. The SELECTOR is
    // witnessed and is NOT hull-vs-body: every crossing site in the original —
    // wheeled, light, tracked, platform, aircraft, the player body and the AI
    // soldier alike — passes the same pair of sound globals and picks between
    // them on the crossing entity's own Flags & 0x2000 (kEntityFlagInAir), i.e.
    // whether the thing was AIRBORNE when it met the plane. A soldier who walks
    // into a river and a boat that idles onto a sandbar take the same sound; a
    // soldier who jumps off a bridge and a truck that launches off a bank take
    // the other. The airborne arm plays BODYWATER1, the other SURFACE_WTR
    // (npwire's kWaterCross* constants, the registry's binding of the two slots).
    // [orig: `if (entity->Flags & 0x2000) send(g_SndBodyWater1 @0x24E09B0) else
    //  send(g_SndSurfaceWtr @0x24E09B4)` @0x47758c (wheeled), @0x482c69..0x482c8b
    //  (platform), @0x4802f5 (aircraft contact, called @0x49254e), @0x4b82e3
    //  (player body), @0x4bfc24 (AI soldier), and their twins]
    bool airborne = false;
};

// Per-tick queue; the host fan drains it after the motor pass, exactly as it
// drains the round ring. Cleared by the drain, so a frame without a listening
// host simply discards - a crossing is a presentation event, never simulation
// state anything else reads.
struct WaterCrossQueue {
    static constexpr size_t kMax = 32; // a frame never legitimately needs more

    std::vector<WaterCrossEvent> events;

    void add(int32_t x, int32_t y, int32_t z, bool airborne) {
        if (events.size() >= kMax) return;
        events.push_back(WaterCrossEvent{x, y, z, airborne});
    }
    void clear() { events.clear(); }
};

} // namespace opennova::world
