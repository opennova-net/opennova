// Advance & Secure zone-slot chain (net-re §5.61). Structural translation of the
// retail ZoneSlotChain_* cluster (ex-"CWeaponSlotManager", inline @ 0x24D1EBC).
#include "world/zone_chain.h"

#include "world/world.h"

namespace opennova::world {

namespace {

// Pool-3 team-marker def-type -> the team whose assigned/base slot it seeds.
// [orig: ZoneSlotChain_BuildFromMission @ 0x4A2DE0 switch on def+80:
//  6003/6096 -> this[3] (team 1), 6004/6097 -> this[5] (team 2),
//  6090/6098 -> this[7] (team 3), 6091/6099 -> this[9] (team 4), default -> this[1]]
int assigned_slot_team_for_marker(int32_t type_id) {
    switch (type_id) {
    case 6003:
    case 6096:
        return 1;
    case 6004:
    case 6097:
        return 2;
    case 6090:
    case 6098:
        return 3;
    case 6091:
    case 6099:
        return 4;
    default:
        return 0;
    }
}

bool chain_contains(const ZoneChain &chain, EntityHandle h) {
    for (const EntityHandle z : chain.zones)
        if (z.packed == h.packed) return true;
    return false;
}

// [orig: ZoneSlotChain_AssignZoneRanks @ 0x4A27F0 — first pass counts entities per
//  zone number, second pass stamps a DESCENDING per-number rank into entry+4]
void assign_ranks(const World &world, ZoneChain &chain) {
    uint8_t bucket_counts[256] = {};
    for (const EntityHandle h : chain.zones) {
        const Entity *e = world.registry.get(h);
        if (e != nullptr) ++bucket_counts[e->zone_number];
    }
    chain.ranks.assign(chain.zones.size(), 0);
    for (size_t i = 0; i < chain.zones.size(); ++i) {
        const Entity *e = world.registry.get(chain.zones[i]);
        if (e != nullptr) chain.ranks[i] = --bucket_counts[e->zone_number];
    }
}

} // namespace

void zone_chain_rebuild_masks(const World &world, ZoneChain &chain) {
    // [orig: ZoneSlotChain_RebuildOwnershipMasks @ 0x4A26C0 — clear the five masks,
    //  then mask[team] |= 1 << entity+538 per vector entry (team <= 4)]
    for (int t = 0; t < ZoneChain::kTeamCount; ++t) chain.owned_mask[t] = 0;
    for (const EntityHandle h : chain.zones) {
        const Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        if (e->team < ZoneChain::kTeamCount)
            chain.owned_mask[e->team] |= 1u << e->zone_number;
    }
}

void zone_chain_build_from_mission(const World &world, ZoneChain &chain) {
    chain.clear(); // [orig: ZoneSlotChain_Reset @ 0x4A2C30 precedes the sweeps]

    // Pool-3 marker sweep: a NUMBERED marker's zone number seeds its family team's
    // assigned slot. [orig: @ 0x4A2DEB..0x4A2E5C — the switch runs only when
    // marker+538 != 0; unmatched def types land on the team-0 row]
    world.registry.for_each([&](const Entity &e) {
        if (e.handle.pool() != 3 || e.zone_number == 0) return;
        chain.assigned_slot[assigned_slot_team_for_marker(e.item_id)] = e.zone_number;
    });

    // Zone-entity sweeps: alive, numbered, capture-trigger (ItemDefAttrib 0x20000
    // "ChangeTeam") entities from pool 1 then pool 2 — the registration ORDER is the
    // frontier walk order. [orig: @ 0x4A2E5E (pool 1) / @ 0x4A2EA2 (pool 2); the
    // alive gate is !(entity+36 & 1)]
    for (const int pool : {1, 2}) {
        world.registry.for_each([&](const Entity &e) {
            if (e.handle.pool() != pool) return;
            if (e.zone_number == 0 || !e.is_capture_trigger || !e.alive) return;
            if (!chain_contains(chain, e.handle)) chain.zones.push_back(e.handle);
        });
    }

    zone_chain_rebuild_masks(world, chain);
    assign_ranks(world, chain); // [orig: CNetQuality-misnamed rank pass @ 0x4A2EEC]
}

bool zone_chain_is_capturable(const World &world, const ZoneChain &chain, uint8_t team,
                              const Entity &zone) {
    // [orig: ZoneSlotChain_IsZoneCapturableByTeam @ 0x4A2450]
    // Not in the chain -> always capturable [orig: !ContainsEntity -> return 1].
    if (!chain_contains(chain, zone.handle)) return true;

    const uint32_t mask = team < ZoneChain::kTeamCount ? chain.owned_mask[team] : 0;
    const int32_t assigned =
            team < ZoneChain::kTeamCount ? chain.assigned_slot[team] : 0;
    const int zone_no = zone.zone_number;

    // The team's assigned/base slot: capturable iff not already the team's.
    // [orig: entity_slot == team_assigned_slot -> return entity+354 != teamIndex]
    if (zone_no == assigned) return zone.team != team;
    if (team >= ZoneChain::kTeamCount) return false;
    if (mask == 0) return false;
    // Inside the team's owned mask: capturable iff not already the team's.
    // [orig: (1 << entity_slot) & slot_bitmask -> return entity+354 != teamIndex]
    if ((1u << zone_no) & mask) return zone.team != team;

    // Adjacency (the leapfrog): a neighbouring zone number Z±1 inside the mask,
    // still HELD by the team, opens Z. An enemy-held entity at that neighbouring
    // number closes the direction. [orig: the prev/next walk @ 0x4A24F1..0x4A2604]
    int prev_slot = ((1u << (zone_no - 1)) & mask) ? zone_no - 1 : 0;
    int next_slot = ((1u << (zone_no + 1)) & mask) ? zone_no + 1 : 0;
    if (prev_slot == 0 && next_slot == 0) return false;
    for (const EntityHandle h : chain.zones) {
        const Entity *other = world.registry.get(h);
        if (other == nullptr) continue;
        if (next_slot != 0 && other->zone_number == next_slot && other->team != team)
            next_slot = 0;
        else if (prev_slot != 0 && other->zone_number == prev_slot && other->team != team)
            prev_slot = 0;
        if (prev_slot == 0 && next_slot == 0) return false;
    }
    return true;
}

uint8_t zone_chain_frontier_zone(const World &world, const ZoneChain &chain, uint8_t team) {
    // [orig: ZoneSlotChain_FindFrontierZone @ 0x4A2AC0 — first vector entry
    //  IsZoneCapturableByTeam(team) -> its entity+538]
    for (const EntityHandle h : chain.zones) {
        const Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        if (zone_chain_is_capturable(world, chain, team, *e)) return e->zone_number;
    }
    return 0;
}

uint32_t zone_chain_owned_zone_mask(const World &world, const ZoneChain &chain, uint8_t team) {
    // [orig: ZoneSlotChain_GetOwnedZoneMask @ 0x4A2620 — all_slots_mask & ~mismatch_mask]
    uint32_t all_mask = 0;
    uint32_t mismatch_mask = 0;
    for (const EntityHandle h : chain.zones) {
        const Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        const uint32_t bit = 1u << e->zone_number;
        all_mask |= bit;
        if (e->team != team) mismatch_mask |= bit;
    }
    return all_mask & ~mismatch_mask;
}

void zone_chain_latch_control(World &world, const ZoneChain &chain) {
    // [orig: Server_UpdateCaptureZoneEntities @ 0x519690: per numbered capture entity,
    //  if !IsZoneCapturableByTeam(enemy_of(zone.team)) -> entity+540 = 0x10000
    //  (@ 0x51975B..0x519764); enemy_of = 2 - (team != 1)]
    for (const EntityHandle h : chain.zones) {
        Entity *e = world.registry.get(h);
        if (e == nullptr) continue;
        const uint8_t enemy = (e->team == 1) ? 2 : 1;
        if (!zone_chain_is_capturable(world, chain, enemy, *e)) e->zone_control = 0x10000;
    }
}

uint8_t zone_chain_zone_info_byte(const ZoneChain &chain, const Entity &zone) {
    // [orig: ZoneSlotChain_GetZoneInfo @0x503eeb — the registered entry's zoneNumber +
    //  32 * rank; rank parallels chain.zones (ZoneSlotChain_AssignZoneRanks @0x4A27F0).
    //  An unregistered numbered entity carries rank 0 (bare zone number).]
    uint8_t rank = 0;
    for (size_t i = 0; i < chain.zones.size(); ++i) {
        if (chain.zones[i] != zone.handle) continue;
        rank = i < chain.ranks.size() ? chain.ranks[i] : 0;
        break;
    }
    return static_cast<uint8_t>(zone.zone_number + 32u * rank);
}

} // namespace opennova::world
