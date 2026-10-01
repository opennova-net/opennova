// The player waypoint track: the list of route markers the HUD waypoint label
// walks, with the current selection, proximity auto-advance, event-driven
// completion, and the mission-scripted show/hide gate.
//
// In the original this is client-side player state over pool-3 marker entities:
// the list [orig: g_WaypointList @ 0xB76570, count @ 0xB76568], the current
// entry [orig: g_CurrentWaypoint @ 0xB7656C], the visibility flag
// [orig: g_ShowWaypoints @ 0x27238BC], and per-marker fields on the entity
// (radius +0, name id +672, linked event +528, chain-back +535, done +536).
// Our container rebase copies those marker fields into track entries at build
// time (markers are immutable post-spawn). The MP POI variant of the same
// storage (Entity_BuildMapPoiLists @ 0x42de40) and the spectate-cycle reuse are
// unported (docs/interface/hud-re.md D-HUD-17).

#pragma once

#include <cstdint>
#include <vector>

namespace opennova::world {

class EntityRegistry;
struct Entity;

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
    // Current entry index; -1 = none (the original's null g_CurrentWaypoint).
    int32_t current = -1;
    // The mission-scripted visibility gate [orig: g_ShowWaypoints @ 0x27238BC,
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
    // [orig: Game_InitNewRound @0x422740 ->
    // SpawnPoint_FindNearestEnemyBasePoint @0x4dd290]
    void reset_selection(const EntityRegistry &registry, const Entity &local,
                         uint32_t game_type);
    // A flag event keeps the prior selection if the scan finds no candidate.
    // [orig: SpawnPoint_FindNearestEnemyBasePoint @ 0x4DD290]
    void select_nearest_enemy_base(const EntityRegistry &registry, const Entity &local,
                                  uint32_t game_type);

    // A fired BMS event completes every entry linked to it, chaining backward
    // through chain_back-flagged predecessors, then skips current past done
    // entries. [orig: EventTrigger_MarkLinkedSpawnPoints @ 0x452ce0, called
    // after the event's actions dispatch @ 0x454cbd/@ 0x454d25]
    void on_event_fired(int32_t event_index);

    // Forward / backward cycle with wrap (the original also skips despawned
    // entries — markers never despawn here). [orig: Spectator_CycleTarget
    // @ 0x4dc1d0, the direction's bit 31]
    void cycle_forward();
    void cycle_backward();

    // The NextWaypoint action (catalog row 51, F7; Shift makes it backward).
    // In a session a plain cycle; out of one it needs the show gate and a
    // selection, a forward step only leaves a chain-back or done entry, and a
    // backward step never passes the first entry and reverts unless it lands
    // on a chain-back entry. [orig: Input_HandleActionBinding case 23
    // @0x49b3de..0x49b44e; the Shift / row-flag 0x200 direction bit
    // @0x49d452..0x49d470]
    void manual_cycle(bool backward, bool in_session);

    // Forward-cycle current past done entries, stopping on wrap-around.
    // [orig: SpawnPoint_SkipBlocked @ 0x4de310]
    void skip_done();
};


// The current-waypoint slice of the per-frame HUD info rebuild plus the
// scripted show gate, as one value (its Godot record wraps it by value).
// `current` -1 = no selection yet; `entry` is the current entry (valid while
// current >= 0).
struct WaypointHudView {
    bool show = false;
    int32_t count = 0;
    int32_t current = -1;
    WaypointEntry entry;
    // The current marker ENTITY's live facts the label's name and composition
    // read each frame (hud_game_text.h WaypointNameKey): its item def
    // (entity+32), the def's type id (def+80) and attrib dword (def+84), and
    // the authored zone byte (entity+538) [orig: HUD_GetWaypointName
    // @0x59467D..0x5946AE; HUD_DrawWaypointNameAndDistance @0x5948C2 /
    // @0x59495B..0x59497D].
    bool has_def = false;
    int32_t def_type = 0;
    uint32_t def_attrib = 0;
    uint8_t zone_number = 0;
};

// The fill, from the live track and the marker entities it mirrors (null
// registry: no entity facts). [orig: HUD_BuildEntityInfo @ 0x4b88b7..0x4b8914
// (hudInfo+373 number, +400/404/408 position) + g_ShowWaypoints @ 0x27238BC]
WaypointHudView waypoint_hud_view(const WaypointTrack &track, const EntityRegistry *registry);

} // namespace opennova::world
