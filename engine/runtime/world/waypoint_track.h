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
// time (markers are immutable post-spawn). Outside the waypoint gametypes the
// same storage holds the multiplayer point-of-interest list instead
// (build_map_poi_list), whose entries are pool-1/2/3 objectives that can move
// and die: the walks read their live entities through a WaypointCycleContext.

#pragma once

#include <cstdint>
#include <vector>

#include <runtime/world/entity.h>

namespace opennova::world {

class EntityRegistry;

// What the list walks read of the world: the entries' entities (one whose
// item def is gone is skipped, and a list of nothing but such entries empties
// itself) and the session facts the KOTH leg tests. A null registry treats
// every entry as live, as the immutable marker mirrors of a fixture are.
// [orig: Spectator_CycleTarget @0x4dc1d0 -- the def tests @0x4dc1fb /
//  @0x4dc267 / @0x4dc2a8, the session/game-type leg @0x4dc2de..0x4dc324]
struct WaypointCycleContext {
    const EntityRegistry *registry = nullptr;
    uint32_t game_type = 0;
    bool in_session = false;
};

// The per-frame selection pass's further inputs: the local player's
// position (16.16 fixed), the SubGoalWon mask the goal gate reads, and the
// local entity the nearest-enemy-base pick measures from.
// [orig: Player_UpdatePerFrame @0x4de5f7..0x4de72c]
struct WaypointFrameInputs : WaypointCycleContext {
    int32_t player_x = 0;
    int32_t player_y = 0;
    uint32_t subgoals_won = 0; // dword_AC86F4 (World::script.subgoals.won)
    const Entity *local = nullptr;
};

// One route marker, mirroring the pool-3 marker entity fields the waypoint
// system reads. [orig: Entity_SpawnFromBMSRecord @ 0x40f0aa seeds them from the
// BMS record: radius = wp_distance<<16 (default 0x8000 = 0.5 u), name id =
// ttool_index, linked event = wp_adv_trigger, chain-back = attributes bit 22]
struct WaypointEntry {
    int32_t node = -1;         // the entity's pool slot (NavNodeTable::nodes for a marker)
    // The entry entity's pool: 3 for a route marker, 1..3 for a POI entry.
    uint8_t pool = 3;
    int32_t x = 0;             // marker position, 16.16 fixed [orig: entity+4]
    int32_t y = 0;             // [orig: entity+8]
    int32_t z = 0;             // [orig: entity+12]
    int32_t radius = 0x8000;   // arrival radius, 16.16 [orig: entity+0]
    int32_t name_id = 0;       // WPNames STRWPNAME%03i index [orig: entity+672]
    int32_t linked_event = 0;  // completing event index; <= 0 = none [orig: entity+528]
    bool chain_back = false;   // completion chains to the previous entry [orig: entity+535]
    bool done = false;         // completed [orig: entity+536]
    // The four waypoint-goal bytes (the BMS record's wpgoal0..3): a nonzero
    // byte i asks for SubGoalWon slot i+1 before the waypoint advances.
    // [orig: entity+0x20C = record +0x64 @0x40f090 (type 6005); read by
    //  SpawnPoint_CheckWeaponRestrictions @0x4dbeb4..0x4dbeca]
    uint8_t goals[4] = {0, 0, 0, 0};

    EntityHandle handle() const { return EntityHandle::make(pool, node); }
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
    // follow-up, D-HUD-16). A waypoint gametype with more than one entry runs
    // the goal gate and the proximity advance; one entry latches it; any other
    // list with no selection picks the nearest enemy base (game types 0x10004
    // / 0x10008) or the first live entry.
    // [orig: Player_UpdatePerFrame @ 0x4de5f7..0x4de72c]
    void tick_advance(const WaypointFrameInputs &in);

    // The multiplayer point-of-interest list, in place of a route outside the
    // waypoint gametypes: pool-2 targets (def attrib 0x8000), pool-1 flags
    // (4091 / 4093 / 4095) and targets, pool-1 flag bays (4098 / 4100 / 4102
    // / 4103), pool-3 zones (6026..6028, 6092, 6093), then pool-2 armories
    // (attrib 0x80000), each walk in slot order. The selection is kept.
    // [orig: Entity_BuildMapPoiLists @0x42de40, called by the 0x0F apply
    //  @0x42e4fd and the local player's 0x50 @0x431b2d]
    void build_map_poi_list(const EntityRegistry &registry);
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
    void on_event_fired(int32_t event_index, const WaypointCycleContext &ctx = {});

    // Forward / backward cycle with wrap past entries whose entity lost its
    // def; in session in the KOTH game types a dead target entry is stepped
    // over once more. [orig: Spectator_CycleTarget @ 0x4dc1d0, the
    // direction's bit 31]
    void cycle_forward(const WaypointCycleContext &ctx = {});
    void cycle_backward(const WaypointCycleContext &ctx = {});

    // The NextWaypoint action (catalog row 51, F7; Shift makes it backward).
    // In a session a plain cycle; out of one it needs the show gate and a
    // selection, a forward step only leaves a chain-back or done entry, and a
    // backward step never passes the first entry and reverts unless it lands
    // on a chain-back entry. [orig: Input_HandleActionBinding case 23
    // @0x49b3de..0x49b44e; the Shift / row-flag 0x200 direction bit
    // @0x49d452..0x49d470]
    void manual_cycle(bool backward, const WaypointCycleContext &ctx);

    // Forward-cycle current past done entries, stopping on wrap-around.
    // [orig: SpawnPoint_SkipBlocked @ 0x4de310]
    void skip_done(const WaypointCycleContext &ctx = {});

private:
    void cycle(bool backward, const WaypointCycleContext &ctx, int depth);
    // The KOTH re-step's guard: the entry the last cycle landed on
    // [orig: dword_B79434 @0x4dc31e / @0x4dc333].
    EntityHandle cycle_guard_;
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

// The fill, from the live track and the entities it names (null registry: no
// entity facts); the position is the entity's live one, so a carried flag's
// label follows it. [orig: HUD_BuildEntityInfo @ 0x4b88b7..0x4b8914
// (hudInfo+373 number, +400/404/408 position) + g_ShowWaypoints @ 0x27238BC]
WaypointHudView waypoint_hud_view(const WaypointTrack &track, const EntityRegistry *registry);

} // namespace opennova::world
