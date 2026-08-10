// Advance & Secure zone-slot chain — the per-team zone-ownership/frontier model
// (net-re §5.61). A structural translation of the retail manager the kong IDB
// mis-labeled "CWeaponSlotManager" (renamed ZoneSlotChain_*, inline global
// @ 0x24D1EBC): five per-team [ownedMask, assignedSlot] pairs plus an ordered
// vector of the mission's capture-trigger zone entities.
//
// Zones are ordinary pools-1/2 entities whose ItemDef carries attrib 0x20000
// ("ChangeTeam" — Entity::is_capture_trigger) and a nonzero authored zone number
// (Entity::zone_number <- BMS byte 155 "lfp_group"). Pool-3 markers of the
// def-type families 6003/6096 (team 1), 6004/6097 (team 2), 6090/6098 (team 3),
// 6091/6099 (team 4) seed each team's assigned/base slot from THEIR zone number.
// [orig: ZoneSlotChain_BuildFromMission @ 0x4A2DE0]
#ifndef OPENNOVA_WORLD_ZONE_CHAIN_H
#define OPENNOVA_WORLD_ZONE_CHAIN_H

#include <cstdint>
#include <vector>

#include "world/entity.h"

namespace opennova::world {

class World;

// [orig: the inline manager struct @ 0x24D1EBC — this[2t]=mask, this[2t+1]=assigned,
//  vector of {entity*, u8 rank} wrappers at +0x28]
struct ZoneChain {
    static constexpr int kTeamCount = 5; // teams 0..4 [orig: teamIndex > 4 guards]
    uint32_t owned_mask[kTeamCount] = {0, 0, 0, 0, 0};
    int32_t assigned_slot[kTeamCount] = {0, 0, 0, 0, 0};
    std::vector<EntityHandle> zones; // registration order (pool 1 sweep, then pool 2)
    std::vector<uint8_t> ranks;      // descending index within a shared zone number
                                     // [orig: ZoneSlotChain_AssignZoneRanks @ 0x4A27F0]

    bool empty() const { return zones.empty(); }
    void clear() {
        for (int t = 0; t < kTeamCount; ++t) {
            owned_mask[t] = 0;
            assigned_slot[t] = 0;
        }
        zones.clear();
        ranks.clear();
    }
};

// Build the chain from the loaded world: pool-3 team markers seed assigned_slot,
// alive capture-trigger entities with a zone number join the vector (pool-1 order
// then pool-2, mirroring the original's two sweeps), then masks + ranks rebuild.
// Call AFTER item traits are stamped (is_capture_trigger comes from items.def).
// [orig: ZoneSlotChain_BuildFromMission @ 0x4A2DE0, from Game_StartMission @ 0x526126]
void zone_chain_build_from_mission(const World &world, ZoneChain &chain);

// ownedMask[team] = OR(1 << zone_number) over that team's registered zone entities.
// [orig: ZoneSlotChain_RebuildOwnershipMasks @ 0x4A26C0]
void zone_chain_rebuild_masks(const World &world, ZoneChain &chain);

// The AS frontier rule: team may capture zone Z iff Z == assigned_slot[team] and the
// entity is not already the team's, OR (1<<Z) & owned_mask[team] and not the team's,
// OR an ADJACENT zone number Z±1 inside the mask is held BY the team (walk the
// vector; an enemy-held adjacent entity kills that direction). Entities outside the
// chain are always capturable. GameType 0x50010 is exempt (handled by the caller —
// the chain itself is gametype-agnostic). [orig: ZoneSlotChain_IsZoneCapturableByTeam
// @ 0x4A2450]
bool zone_chain_is_capturable(const World &world, const ZoneChain &chain, uint8_t team,
                              const Entity &zone);

// First vector entry capturable by `team` -> its zone number; 0 = none. The
// "go capture zone N" deploy hint (S2C 0x1E event 0x3A) and the auto-deploy key.
// [orig: ZoneSlotChain_FindFrontierZone @ 0x4A2AC0]
uint8_t zone_chain_frontier_zone(const World &world, const ZoneChain &chain, uint8_t team);

// Bitmask of zone numbers wholly owned by `team` (a number with ANY entity on
// another team drops out). Rides the S2C 0x0F variant-0 u32 — the deploy-map
// owned-zone advertising. [orig: ZoneSlotChain_GetOwnedZoneMask @ 0x4A2620]
uint32_t zone_chain_owned_zone_mask(const World &world, const ZoneChain &chain, uint8_t team);

// The secure latch: every registered zone entity the ENEMY frontier cannot reach
// snaps to control = 0x10000 (fully secured). Seeds the initial control state at
// build; the 1 Hz capture loop (slice 2) re-runs it before each control delta.
// enemy_of(team): 1 -> 2, else -> 1 [orig: 2 - (team != 1) @ 0x51974a].
// [orig: Server_UpdateCaptureZoneEntities @ 0x519690 latch @ 0x51975B..0x519764]
void zone_chain_latch_control(World &world, const ZoneChain &chain);

// The 0x0D record's packed zone byte for one numbered zone entity: zoneNumber + 32 * rank
// (rank = the entity's descending index within its shared zone number; an entity outside
// the trigger chain carries rank 0 — its bare zone number).
// [orig: ZoneSlotChain_GetZoneInfo @0x503eeb feeding the 0x2000-gated byte in
//  serialize_entity_pool_to_packet_0 @0x503ecc; golden ASH_I5A bunker 0x22 = zone 2 rank 1]
uint8_t zone_chain_zone_info_byte(const ZoneChain &chain, const Entity &zone);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ZONE_CHAIN_H
