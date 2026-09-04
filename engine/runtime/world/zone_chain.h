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
#pragma once

#include <cstdint>
#include <vector>

#include <runtime/world/entity.h>

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







// The 0x0D record's packed zone byte for one numbered zone entity: zoneNumber + 32 * rank
// (rank = the entity's descending index within its shared zone number; an entity outside
// the trigger chain carries rank 0 — its bare zone number).
// [orig: ZoneSlotChain_GetZoneInfo @0x503eeb feeding the 0x2000-gated byte in
//  serialize_entity_pool_to_packet_0 @0x503ecc; golden ASH_I5A bunker 0x22 = zone 2 rank 1]
uint8_t zone_chain_zone_info_byte(const ZoneChain &chain, const Entity &zone);

} // namespace opennova::world
