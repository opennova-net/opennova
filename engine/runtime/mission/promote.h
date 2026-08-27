// Mission -> world promotion: spawn a parsed BMS mission's entities into the world
// entity pools, allocate an AI brain per AI-controlled entity, and build the waypoint
// nav table, so the WAC / BMS / AI tick systems can simulate a real authored mission.
//
// This is the adapter between engine/runtime/mission (bms::File) and engine/runtime/world (World / AiSystem).
// It lives in engine/runtime/mission because engine/runtime/world is deliberately mission-agnostic
// (entity.h: "promotion adapts between them"); engine/runtime/mission already links engine/runtime/world.
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

#include <formats/aip/aip.h>

#include <formats/mission/bms.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/player_loadout.h>

#include <functional>
#include <string>
#include <utility>

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
    // Turret articulation limits in BAM, resolved from the primary weapon's
    // weapon.def rows (targetyawrange/targetpitchmax/targetpitchmin) once the
    // embedder's weapon table is loaded — the clamp window the emplaced-controls
    // derivation applies. All-zero = unresolved/not authored (no clamp).
    // [orig: Entity_GetWeaponTurretLimits fallback @0x540e35..0x540e58;
    //  clamp @0x441228..0x44128c via Math_ClampAngleToBounds @0x540cc0]
    int32_t turret_yaw_range_bam = 0;
    int32_t turret_pitch_max_bam = 0;
    int32_t turret_pitch_min_bam = 0;
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

    // The parsed .aip profiles, keyed by the BMS ai_textfile (name2, ASCII
    // case-insensitive, no extension). The parse (engine/formats/aip) carries
    // the witnessed GROUND-type set — speeds stay raw authored values; the
    // brain seed applies the x65536/225 scale (1000 x 1/225000 x 65536,
    // truncated). Hosts that can read loose/PFF .aip files pass rows here
    // before promotion; entities whose profile is absent keep the
    // default_speed stand-in.
    // [orig: AIProfile_ParseProperty @0x45de70 ("patrol_speed" -> profile+0xC0,
    //  "combat_speed" -> profile+0xC4, the GROUND weapon blocks +120/+152);
    //  Entity_InitVehicleAIFromDef seeds brain[49] = profile+0xC4 and
    //  brain[50] = profile+0xC0 @0x4688C7/@0x4688D3, profile name = the slot
    //  ai_textfile, def-level fallback itemDef+0x8B8, then "helo1"]
    struct AiProfileRow {
        std::string profile; // ai_textfile, lowercase
        aip::Profile data;
    };
    std::vector<AiProfileRow> ai_profiles;

    // Retail resolves a placed vehicle's profile NAME in three arms, and every
    // arm ends in a profile — a nameless vehicle never runs without one:
    //   * the helicopter AI-init family takes the record's ai_textfile, else
    //     "helo1" [orig: Entity_InitHelicopterAI @0x461F00 — the +0x9C test
    //      @0x461f2b, sprintf("%s.aip") @0x461f35, the "helo1.aip" arm @0x461f50;
    //      Entity_InitHelicopterAIFromDef @0x4683C0, the same pair @0x4684a4/
    //      @0x4684c9];
    //   * the vehicle AI-init family takes the ai_textfile, else the item def's
    //     own default_aip (+0x8B8), else "helo1" [orig: Entity_InitVehicleAIFromDef
    //      @0x4686C0 — @0x4687a3 record name, @0x4687c1 def+0x8B8, @0x4687d3 "helo1",
    //      then Path_ReplaceOrAppendExtension(".aip") @0x4687f4].
    // The embedder answers per items.def type id which arm the item's AI class
    // row runs and what its default_aip says; absent (tests, no item db) the
    // resolution is the ai_textfile alone, as before.
    struct AiProfileDefaults {
        bool known = false;          // the item db knew the type id
        bool helicopter_init = false; // the AI class row is the helicopter init family
        std::string default_aip;     // items.def default_aip (+0x8B8), may be empty
    };
    std::function<AiProfileDefaults(int32_t type_id)> ai_profile_defaults;

    // The record's name_index -> mission-RTXT [PeopleNames] STRNAME%03i display
    // name (empty = no entry; index 0 never resolves). The embedder builds this
    // from the same mission text table the boot installs; the promote truncates
    // to the retail 15-char copy. [orig: Entity_SpawnFromBMSRecord
    // @0x40ecbf..0x40ed0a — sprintf("STRNAME%03i", rec+4) ->
    // TextResource_FindEntryBySectionAndKey("PeopleNames") -> strncpy(+0xF4, 15)]
    std::function<std::string(int32_t)> people_name_resolver;

    // Per-pool registry capacities, defaulted to the witnessed retail g_pool_list
    // sizes [orig: EntityPool_Allocate @0x442168].
    size_t pool_capacities[world::kEntityPoolCount] = {
            world::kRetailPoolCapacity[0], world::kRetailPoolCapacity[1],
            world::kRetailPoolCapacity[2], world::kRetailPoolCapacity[3],
            world::kRetailPoolCapacity[4]};
};

// The profile NAME retail's AI init loads for a record (lowercase, no
// extension): the ai_textfile when authored; else, for a placed item whose AI
// class row the embedder knows (`placed_item` + a `known` answer), the
// helicopter family's "helo1" or the vehicle family's def default_aip then
// "helo1"; else empty (no profile row). Shared by the boot resolver (which
// loads the files) and the promote (which seeds the brain), so both agree on
// the fallback [orig: PromoteOptions::ai_profile_defaults' witness map].
std::string ai_profile_name_for(
        const bms::Entity &e, bool placed_item,
        const std::function<PromoteOptions::AiProfileDefaults(int32_t)> &defaults);

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

// The mission's loadout/availability chunks -> plain world rows, with retail's
// own atol truncation semantics on the string tuples (a non-numeric prefix
// parses as 0; trailing text is ignored) [orig: Mission_LoadBMSFile @ 0x40F4E0;
// the tuple parse over restrictionData @ 0x42cf7c]. The embedder stashes these
// at mission load and promotes at weapon-table time through
// world::local_loadout_promote_mission_rules (the witnessed SP-vs-net gate).
void stash_mission_loadout_rules(
        const bms::File &mission,
        std::vector<std::pair<std::string, int32_t>> &r_availability_rows,
        std::vector<world::WeaponKitEntry> &r_kit_rows);

} // namespace opennova::mission

#endif // OPENNOVA_MISSION_PROMOTE_H
