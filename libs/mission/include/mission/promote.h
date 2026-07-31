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

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mission/bms.h"
#include "world/entity.h"
#include "world/geom.h"

namespace opennova::world {
class AiSystem;
class World;
} // namespace opennova::world

namespace opennova::mission {

enum class EmplacementAttachmentKind : uint8_t {
    Standard = 0,
    G = 1,
    C = 2,
};

struct ItemEmplacementAttachmentSpec {
    int32_t child_type_id = 0; // raw BMS type id (public item id minus 100000)
    EmplacementAttachmentKind kind = EmplacementAttachmentKind::Standard;
    uint8_t stored_slot = 0;   // 1-based fixed items.def attachment slot
    uint8_t attachment_flags = 0; // designated C bit 1 / G bit 2
    world::Seat anchor;        // authored parent userpoint frame; bone 0 = parent root
    bool anchor_found = false;
    uint8_t angle_count = 0;   // 0 = weapon.def fallback, 4 = explicit (even all-zero)
    int32_t down_limit_bam = 0;
    int32_t up_limit_bam = 0;
    int32_t right_limit_bam = 0;
    int32_t left_limit_bam = 0;
};

struct ItemSeatSpec {
    int32_t type_id = 0; // raw BMS/items.def id stored in bms::Entity::type_id
    // Target item definition phrase_set (+0x86C). Zero is valid when the flag is
    // true; false means the production metadata source was absent.
    bool mount_config_valid = false;
    int32_t mount_config = 0;
    std::vector<world::Seat> seats;
    // "armory*" userpoint locals (the embedder feeds them only for items.def Armory-attrib
    // 0x80000 items) + the ewep 'primary_weapon' link — the attach-label sources.
    std::vector<world::Vec3> armory_points;
    std::string primary_weapon;
    std::vector<ItemEmplacementAttachmentSpec> emplacement_attachments;
};

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

    // Modeled seat metadata, keyed by the raw BMS type_id. The original derives these from
    // model userpoints/seat bones in Entity_FindBestSeatSlot @0x4351f0; hosts that load models
    // pass the extracted seat list here before promotion.
    std::vector<ItemSeatSpec> item_seat_specs;

    // Command 123/124/125 authored spawn attachment: Entity_UpdateInfantryAI @0x4B9910
    // resolves slot+152 (BMS wp_number) as a target entity serial, then walks/attaches to a
    // vehicle seat. Until the full walk-to-seat staging is ported, promotion only attaches actors
    // already authored near the target; this mirrors EntityCommands::mount_best's proximity guard.
    float command_mount_radius = 20.0f;

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
