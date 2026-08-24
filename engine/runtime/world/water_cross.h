// Water-surface crossing events: the host-side queue behind the S2C 0x34 fan.
//
// Retail emits a positioned effect/sound the moment a body or a hull crosses the
// water plane — once per crossing, not per frame — and fans it to every alive
// player so their clients spawn the same splash. The motor detects the edge and
// records it here; the network layer drains the queue and encodes 0x34. Keeping
// the detection in the motor and the encoding in net/ preserves the engine's
// no-Godot, no-wire-knowledge boundary and lets the edge logic be unit-tested
// without a socket.
//
// [orig: the water block of Entity_ProcessWheeledVehiclePhysics and its
//  light/tracked twins -> CEffectWorld_SpawnEmitterAtPosition +
//  Server_SendOverlayActionToAlive @0x50a1b0 (send_mask 128 = alive players)]
#ifndef OPENNOVA_WORLD_WATER_CROSS_H
#define OPENNOVA_WORLD_WATER_CROSS_H

#include <cstdint>
#include <vector>

namespace opennova::world {

// One crossing. The position is the entity's horizontal position at the WATER
// PLANE height — retail places the effect on the surface, not at the entity's
// own Z [orig: `dest[6] = Env_WaterHeightFixed` in the water block].
struct WaterCrossEvent {
    int32_t x = 0; // 16.16 world
    int32_t y = 0;
    int32_t water_z = 0;
    // Which of the two authored sounds the fan should carry. The SELECTOR is
    // witnessed and is NOT hull-vs-body: every crossing site in the original —
    // wheeled, light, tracked, platform, aircraft, the player body and the AI
    // soldier alike — passes the same pair of sound globals and picks between
    // them on the crossing entity's own Flags & 0x2000 (kEntityFlagInAir), i.e.
    // whether the thing was AIRBORNE when it met the plane. A soldier who walks
    // into a river and a boat that idles onto a sandbar take the same sound; a
    // soldier who jumps off a bridge and a truck that launches off a bank take
    // the other. The name-to-global binding stays capture-derived (npwire's
    // kWaterCross* constants).
    // [orig: `if (entity->Flags & 0x2000) send(dword_24E09B0) else
    //  send(dword_24E09B4)` @0x482c8b (wheeled), @0x49254e (aircraft),
    //  @0x4b8020 (player body), @0x4bfb87 (AI soldier), and their twins]
    bool airborne = false;
};

// Per-tick queue; the host fan drains it after the motor pass, exactly as it
// drains the round ring. Cleared by the drain, so a frame without a listening
// host simply discards - a crossing is a presentation event, never simulation
// state anything else reads.
struct WaterCrossQueue {
    static constexpr size_t kMax = 32; // a frame never legitimately needs more

    std::vector<WaterCrossEvent> events;

    void add(int32_t x, int32_t y, int32_t water_z, bool airborne) {
        if (events.size() >= kMax) return;
        events.push_back(WaterCrossEvent{x, y, water_z, airborne});
    }
    void clear() { events.clear(); }
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WATER_CROSS_H
