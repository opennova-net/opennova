// The player waypoint track: the list of route markers the HUD waypoint label
// walks, with the current selection, proximity auto-advance, event-driven
// completion, and the mission-scripted show/hide gate.
//
// In the original this is client-side player state over pool-3 marker entities:
// the list [orig: g_waypointList @ 0xB76570, count @ 0xB76568], the current
// entry [orig: g_currentWaypoint @ 0xB7656C], the visibility flag
// [orig: g_showWaypoints @ 0x27238BC], and per-marker fields on the entity
// (radius +0, name id +672, linked event +528, chain-back +535, done +536).
// Our container rebase copies those marker fields into track entries at build
// time (markers are immutable post-spawn). The MP POI variant of the same
// storage (Entity_BuildMapPoiLists @ 0x42de40) and the spectate-cycle reuse are
// unported (docs/interface/hud-re.md D-HUD-17).

#ifndef OPENNOVA_WORLD_WAYPOINT_TRACK_H
#define OPENNOVA_WORLD_WAYPOINT_TRACK_H

#include <cstdint>
#include <vector>

namespace opennova::world {

// One route marker, mirroring the pool-3 marker entity fields the waypoint
// system reads. [orig: Entity_SpawnFromBMSRecord @ 0x40f0aa seeds them from the
// BMS record: radius = wp_distance<<16 (default 0x8000 = 0.5 u), name id =
// ttool_index, linked event = wp_adv_trigger, chain-back = attributes bit 22]
struct WaypointEntry {
    int32_t node = -1;         // NavNodeTable::nodes index (the pool-3 mirror)
    int32_t x = 0;             // marker position, 16.16 fixed [orig: entity+4]
    int32_t y = 0;             // [orig: entity+8]
    int32_t z = 0;             // [orig: entity+12]
    int32_t radius = 0x8000;   // arrival radius, 16.16 [orig: entity+0]
    int32_t name_id = 0;       // WPNames STRWPNAME%03i index [orig: entity+672]
    int32_t linked_event = 0;  // completing event index; <= 0 = none [orig: entity+528]
    bool chain_back = false;   // completion chains to the previous entry [orig: entity+535]
    bool done = false;         // completed [orig: entity+536]
};

class WaypointTrack {
public:
    std::vector<WaypointEntry> entries;
    // Current entry index; -1 = none (the original's null g_currentWaypoint).
    int32_t current = -1;
    // The mission-scripted visibility gate [orig: g_showWaypoints @ 0x27238BC,
    // init 1 @ 0x5a4913; BMS ShowWaypoints -> Game_SetShowWaypoints @ 0x58fb50].
    bool show = true;

    void clear() {
        entries.clear();
        current = -1;
        show = true;
    }

    bool empty() const { return entries.empty(); }
    const WaypointEntry *current_entry() const {
        if (current < 0 || current >= static_cast<int32_t>(entries.size())) return nullptr;
        return &entries[static_cast<size_t>(current)];
    }

    // The per-frame current-selection pass, authority form (the original's
    // non-authority leg forces linked_event = -1 and is the client-view
    // follow-up, D-HUD-16). Player position in 16.16 fixed mission space.
    // [orig: Player_UpdatePerFrame @ 0x4de5f7..0x4de72c]
    void tick_advance(int32_t player_x_fixed, int32_t player_y_fixed);

    // A fired BMS event completes every entry linked to it, chaining backward
    // through chain_back-flagged predecessors, then skips current past done
    // entries. [orig: EventTrigger_MarkLinkedSpawnPoints @ 0x452ce0, called
    // after the event's actions dispatch @ 0x454cbd/@ 0x454d25]
    void on_event_fired(int32_t event_index);

    // Forward cycle with wrap (the original also skips despawned entries —
    // markers never despawn here). [orig: Spectator_CycleTarget @ 0x4dc1d0]
    void cycle_forward();

    // Forward-cycle current past done entries, stopping on wrap-around.
    // [orig: SpawnPoint_SkipBlocked @ 0x4de310]
    void skip_done();
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_WAYPOINT_TRACK_H
