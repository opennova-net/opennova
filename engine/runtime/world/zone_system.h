// The zone system: the Advance & Secure zone-slot chain, the capture
// transaction, the deploy wave groups and the spawn round-robin, plus the
// zone-chain / capture / spawn-select verbs that reached the world through a
// `World &` first parameter before ADR 0043 slice E6. Bound to its world at
// construction; every method body still names that world `world`.
#pragma once

#include <runtime/world/entity.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/zone_capture.h>
#include <runtime/world/zone_chain.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace opennova::world {

class World;

class ZoneSystem {
public:
    explicit ZoneSystem(World &world) : world_(world) {}
    ZoneSystem(const ZoneSystem &) = delete;
    ZoneSystem &operator=(const ZoneSystem &) = delete;

    // The Advance & Secure zone-slot chain (empty until the host builds it after the
    // item-traits sweep — zone registration needs Entity::is_capture_trigger). Feeds
    // the 0x0F owned-zone mask, the 0x0E deploy gates, and the 0x1E frontier hint.
    // [orig: the inline manager @0x24D1EBC, ZoneSlotChain_BuildFromMission @0x4a2de0
    // from Game_StartMission; net-re §5.61]
    ZoneChain chain;
    // The capture request/active transaction is mission state, not host-wire
    // scratch. Keeping it beside the chain prevents a second lifecycle or a
    // static server singleton. [orig: CaptureCtx_Reset @0x53BD00]
    ZoneCaptureState capture;
    // Mission-built deploy wave groups. Keeping them beside spawn selection
    // gives immediate picks and timed releases one lifecycle and no host-only
    // shadow table. [orig: SpawnWaveList_BuildFromMission @0x52A920]
    SpawnWaveList spawn_waves;
    // One mission-global round-robin shared by default spawn selection and a
    // picked numbered zone's type-6007 scatter choices.
    // [orig: g_SpawnCycleCounter @0x24C10D0;
    // Server_PositionPlayerForSpawn @0x50CF60]
    uint32_t spawn_cycle_counter = 0;

    // Build the chain from the loaded world: pool-3 team markers seed assigned_slot,
    // alive capture-trigger entities with a zone number join the vector (pool-1 order
    // then pool-2, mirroring the original's two sweeps), then masks + ranks rebuild.
    // Call AFTER item traits are stamped (is_capture_trigger comes from items.def).
    // [orig: ZoneSlotChain_BuildFromMission @ 0x4A2DE0, from Game_StartMission @ 0x526126]
    void build_chain_from_mission();
    // ownedMask[team] = OR(1 << zone_number) over that team's registered zone entities.
    // Its only retail caller is the flip's refresh below, so the cached masks
    // (GetTeamMask) stay as the last numbered SpawnPoint flip left them.
    // [orig: ZoneSlotChain_RebuildOwnershipMasks @ 0x4A26C0]
    void rebuild_masks();
    // The numbered flip's mask refresh: save the capturer's ENEMY mask, rebuild
    // every mask, and report "unchanged" unless the zone's number was absent
    // from the saved mask or the enemy mask moved.
    // [orig: ZoneSlotChain_RebuildMasksAndCheckUnchanged @0x4A2B60]
    bool rebuild_masks_and_check_unchanged(uint8_t capturer_team, const Entity &zone);
    // The uniform-owner test: in A&S (0x10010) and C&C (0x50010) only, a
    // non-empty chain whose entries all carry one team returns that team (0
    // included); every other game type returns nothing.
    // [orig: ZoneSlotChain_GetWinningTeamIfAllOwned @0x4A2920]
    std::optional<uint8_t> winning_team_if_all_owned() const;
    // A registered entry's zone number and rank; false for an entity outside
    // the chain. [orig: ZoneSlotChain_GetZoneInfo @0x4A2750]
    bool zone_info(const Entity &zone, uint8_t &number, uint8_t &rank) const;
    // The cached owned mask for `team` (0 above team 4).
    // [orig: ZoneSlotChain_GetTeamMask @0x4A2350]
    uint32_t team_mask(uint8_t team) const;
    // The AS frontier rule: team may capture zone Z iff Z == assigned_slot[team] and the
    // entity is not already the team's, OR (1<<Z) & owned_mask[team] and not the team's,
    // OR an ADJACENT zone number Z±1 inside the mask is held BY the team (walk the
    // vector; an enemy-held adjacent entity kills that direction). Entities outside the
    // chain are always capturable, and so is every chain entry in C&C (0x50010).
    // [orig: ZoneSlotChain_IsZoneCapturableByTeam @ 0x4A2450, the C&C return
    // @0x4A2476..0x4A2480]
    bool is_capturable(uint8_t team, const Entity &zone) const;
    // First vector entry capturable by `team` -> its zone number; 0 = none. The
    // "go capture zone N" deploy hint (S2C 0x1E event 0x3A) and the auto-deploy key.
    // [orig: ZoneSlotChain_FindFrontierZone @ 0x4A2AC0]
    uint8_t frontier_zone(uint8_t team) const;
    // Bitmask of zone numbers wholly owned by `team` (a number with ANY entity on
    // another team drops out). Rides the S2C 0x0F variant-0 u32 — the deploy-map
    // owned-zone advertising. [orig: ZoneSlotChain_GetOwnedZoneMask @ 0x4A2620]
    uint32_t owned_zone_mask(uint8_t team) const;
    // The secure latch: every registered zone entity the ENEMY frontier cannot reach
    // snaps to control = 0x10000 (fully secured). Seeds the initial control state at
    // build; the 1 Hz capture loop (slice 2) re-runs it before each control delta.
    // enemy_of(team): 1 -> 2, else -> 1 [orig: 2 - (team != 1) @0x519745..0x519757].
    // [orig: Server_UpdateCaptureZoneEntities @ 0x519690 latch @ 0x51975B..0x519764]
    void latch_control();
    // Per-logic-tick consumer of the collision world's exact type-10 Change Team
    // contacts. It updates active presence and queues one request per zone/team;
    // it performs no ownership transition itself.
    void capture_contact_tick();
    // One 1 Hz capture transaction. Reads its configuration and persistent state
    // from World, emits all semantic wire events, and drains pending requests.
    void capture_second_tick(ZoneCaptureEvents &out);
    // Resolve a C2S 0x0E deploy pick to its target entity. Pools 0/1/2 only (pool 0 =
    // mobile spawn vehicles), the ItemDef must carry attrib 0x40000 "SpawnPoint"
    // (Entity::is_spawn_point), and the target's team must match the requester's — a
    // TEAMLESS requester may pick anything. nullptr = invalid pick.
    // [orig: Server_ResolveSpawnTargetHandle @0x4fe110]
    const Entity *resolve_spawn_target(uint8_t requester_team, uint16_t handle) const;
    // The world offers at least one registered spawn zone (an attrib-0x40000
    // "SpawnPoint" entity). Gates the join-time respawn-pending flag — the deploy screen only
    // holds when the mission has zones to pick [orig: Server_OnPlayerJoin @0x51a6f2
    // `|= 0x10 iff SpawnZoneList_GetCount() > 0`; same count gates the 0x0F game_flags bit0
    // @0x502da7; the client builds its own picker list from the exact S2C 0x10/0x0D
    // pool-2 + pool-1 rows, with the same def gate — Entity_BuildSpawnZoneList @0x43EAE0].
    bool has_spawn_zone() const;
    SpawnZoneRegistry build_spawn_zone_list() const;
    // Whether retail's target-less respawn gate considers this team to have an
    // available spawn zone. This walks SpawnZoneList, not the player roster: an
    // unnumbered same-team zone qualifies regardless of control; a numbered one
    // qualifies at full control. [orig: Entity_HasAliveEntityOfTeam @0x4FC7B0]
    bool team_has_available_spawn_zone(uint8_t team) const;
    // The 0xFFFE auto-deploy pick over the sorted SpawnZoneList: the requester
    // team's own numbered zone that sits ON the frontier — enemy-capturable, or
    // carrying the frontier number — with control fully secured (>= 0x10000);
    // when none does, the frontier number steps once in the direction the team's
    // cached mask continues and the walk retries while the stepped number stays
    // owned. Objective gametypes (game_type & 0x20000) take the LAST team-matching
    // UN-numbered list entry instead. nullptr = no zone spawn (the caller falls
    // back to the marker chain). [orig: Spawn_FindEntityForTeam @0x4fc810]
    const Entity *find_spawn_zone_for_team(uint8_t team, uint32_t game_type_value) const;

private:
    World &world_;
};

} // namespace opennova::world
