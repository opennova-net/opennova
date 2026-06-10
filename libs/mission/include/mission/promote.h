// Mission -> world promotion: spawn a parsed BMS mission's entities into the world
// entity pools, allocate an AI brain per AI-controlled entity, and build the waypoint
// nav table, so the WAC / BMS / AI tick systems can simulate a real authored mission.
//
// This is the adapter between libs/mission (bms::File) and libs/world (World / AiSystem).
// It lives in libs/mission because libs/world is deliberately mission-agnostic
// (entity.h: "promotion adapts between them"); libs/mission already links libs/world.
//
// Grounding (Jointops.exe): the AI-brain allocation + init mirrors Entity_InitVehicleAI
// @0x460200 (the generic AI initializer: scan unk_AED380 for a free 812-byte slot,
// profile-by-name, a PER-ENTITY 32-byte scheduler at unk_B1FF80+32*slot, initial state 0,
// spawn-transform copy). The nav table mirrors the waypoint populator XML_ParseGroupAction
// @0x4cc450 (34-dword channel records over pool-3 marker nodes). See
// notes/world/ai_movement.md §4 / §10 for the RE map + the tracked deviations below.
#ifndef OPENNOVA_MISSION_PROMOTE_H
#define OPENNOVA_MISSION_PROMOTE_H

#include <cstdint>

#include "mission/bms.h"
#include "world/ai.h"
#include "world/world.h"

namespace opennova::mission {

struct PromoteOptions {
    // NavEntry f[0] (arrival/advance threshold) for markers whose authored wp_distance is 0.
    // [orig: Entity_SpawnFromBMSRecord @0x40e9f0 item 6005 defaults the radius to 0x8000
    // (0.5u); an authored wp_distance is used directly (<<16).]
    int32_t arrival_radius = 0x8000;

    // AiBrain kSpeedA/kSpeedB (the per-node mover speed) when the real profile speed is
    // unmodeled. Nonzero so routed entities visibly advance once locomotion (step 2) lands.
    int32_t default_speed = 20;

    // Init waypoint-followers straight into GROUND_FOLLOWWP (state 16). Tracked deviation:
    // Entity_InitVehicleAI inits state 0 and the engine transitions to 16 via the (still
    // unwitnessed) waypoint-assignment path; setting 16 here lets routed entities patrol on
    // spawn. With this false, entities stay in state 0 (faithful init, no movement).
    bool patrol_on_spawn = true;

    size_t actor_pool_capacity = 1024;
    size_t marker_pool_capacity = 4096;
};

struct PromoteResult {
    int spawned = 0;      // entities placed into the actor/static pools
    int brains = 0;       // AI brains attached (AiSystem entities)
    int nav_channels = 0; // waypoint channels built
    int nav_nodes = 0;    // pool-3 marker nodes built
    int dropped = 0;      // spawn failures (pool full)
};

// Promote a parsed mission into a live world. Populates `world.registry` (entity pools),
// `ai.nav` (the waypoint channel/node table), and `ai` entities (one AI brain per organic).
// Does NOT register `ai` as a World system or tick it — the caller wires + drives the sim.
PromoteResult promote_mission(const bms::File &mission, world::World &world,
                              world::AiSystem &ai, const PromoteOptions &opts = {});

} // namespace opennova::mission

#endif // OPENNOVA_MISSION_PROMOTE_H
