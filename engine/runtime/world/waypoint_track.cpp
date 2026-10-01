#include <runtime/world/waypoint_track.h>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <runtime/world/entity_registry.h>

namespace opennova::world {

void WaypointTrack::reset_selection(const EntityRegistry &registry,
                                    const Entity &local, uint32_t game_type) {
    current = -1;
    select_nearest_enemy_base(registry, local, game_type);
}

void WaypointTrack::select_nearest_enemy_base(const EntityRegistry &registry,
        const Entity &local, uint32_t game_type) {
    if ((game_type != 65540 && game_type != 65544) || entries.size() <= 1) return;
    int32_t nearest = INT32_MAX;
    for (size_t index = 0; index < entries.size(); ++index) {
        const Entity *marker = registry.get(EntityHandle::make(3, entries[index].node));
        if (marker == nullptr || !marker->has_item_def || (marker->flags & 1u) != 0) continue;
        if ((local.team == 2 && marker->item_id != 4091) ||
                (local.team == 1 && marker->item_id != 4093)) continue;
        const int32_t dx = static_cast<int32_t>(static_cast<uint32_t>(to_fixed(local.position.x)) -
                static_cast<uint32_t>(to_fixed(marker->position.x)));
        const int32_t dy = static_cast<int32_t>(static_cast<uint32_t>(to_fixed(local.position.y)) -
                static_cast<uint32_t>(to_fixed(marker->position.y)));
        // The x87 distance caps at flt_7C19E0 (0x7fff0000), then truncates.
        const double length = std::sqrt(double(dx) * dx + double(dy) * dy);
        const int32_t distance = static_cast<int32_t>(std::min(length, 2147418112.0));
        if (distance < nearest) {
            nearest = distance;
            current = static_cast<int32_t>(index);
        }
    }
}

// [orig: Player_UpdatePerFrame @ 0x4de5f7..0x4de72c — the waypoint-gametype leg.
//  The weapon-restriction gate (SpawnPoint_CheckWeaponRestrictions @ 0x4dbe80)
//  is modeled always-pass: SP route markers author no restrictions (D-HUD-17).]
void WaypointTrack::tick_advance(int32_t player_x_fixed, int32_t player_y_fixed) {
    const int32_t count = static_cast<int32_t>(entries.size());
    if (count > 1) {
        const WaypointEntry *cur = current_entry();
        // Authority: an event-linked waypoint never proximity-advances — its
        // completion arrives via on_event_fired. [orig: @ 0x4de649 jge skip]
        if (cur && cur->linked_event > 0) return;
        if (!cur) {
            // Null current latches the first entry, skipping completed ones.
            // [orig: @ 0x4de6de-0x4de6ea]
            current = 0;
            skip_done();
            return;
        }
        // NovaLogic horizontal approx distance: max + min/2 over |dx|, |dy|,
        // preceded by the per-axis early accept. [orig: @ 0x4de687..0x4de6c2]
        const int32_t dx = std::abs(player_x_fixed - cur->x);
        const int32_t dy = std::abs(player_y_fixed - cur->y);
        const uint32_t radius = static_cast<uint32_t>(cur->radius);
        if (static_cast<uint32_t>(dx) > radius && static_cast<uint32_t>(dy) > radius) return;
        const uint32_t approx = (dx <= dy)
                ? static_cast<uint32_t>(dy) + (static_cast<uint32_t>(dx) >> 1)
                : static_cast<uint32_t>(dx) + (static_cast<uint32_t>(dy) >> 1);
        // Never advance past the last entry. [orig: @ 0x4de6ca cmp vs list[count-1]]
        if (approx < radius && current != count - 1)
            cycle_forward();
    } else if (count == 1) {
        current = 0; // [orig: @ 0x4de6f3..0x4de6fa single-entry latch]
    }
}

// [orig: EventTrigger_MarkLinkedSpawnPoints @ 0x452ce0 — linked entries get
//  done=1, then walk BACKWARD while the predecessor's chain flag (+535) is set;
//  ends with SpawnPoint_SkipBlocked. The `> 0` gate is the original's: markers
//  cannot link to event 0.]
void WaypointTrack::on_event_fired(int32_t event_index) {
    if (event_index < 0 || entries.empty()) return;
    for (int32_t i = 0; i < static_cast<int32_t>(entries.size()); ++i) {
        WaypointEntry &e = entries[static_cast<size_t>(i)];
        if (e.linked_event <= 0 || e.linked_event != event_index) continue;
        e.done = true;
        // [orig: @ 0x4de.. — the backward chain reads entry i-1's +535 flag
        //  before marking it, walking down while the flag holds @ 0x452d40]
        for (int32_t back = i - 1; back >= 0; --back) {
            if (!entries[static_cast<size_t>(back)].chain_back) break;
            entries[static_cast<size_t>(back)].done = true;
        }
    }
    skip_done();
}

// [orig: Spectator_CycleTarget @ 0x4dc1d0 forward leg — next index with wrap.
//  The original skips entries whose itemDef cleared (despawned markers); track
//  entries are immutable marker mirrors, so every entry stays cyclable. A
//  current not in the list scans from index 0 (@0x4dc246), so the step lands
//  on index 1.]
void WaypointTrack::cycle_forward() {
    const int32_t count = static_cast<int32_t>(entries.size());
    if (count <= 1) return; // [orig: @0x4dc1d9]
    const int32_t from = current >= 0 && current < count ? current : 0;
    current = (from + 1) % count;
}

// [orig: Spectator_CycleTarget @ 0x4dc1d0 backward leg @0x4dc29a..0x4dc2d6 —
//  the previous index, wrapping to the last entry.]
void WaypointTrack::cycle_backward() {
    const int32_t count = static_cast<int32_t>(entries.size());
    if (count <= 1) return;
    const int32_t from = current >= 0 && current < count ? current : 0;
    current = from > 0 ? from - 1 : count - 1;
}

void WaypointTrack::manual_cycle(bool backward, bool in_session) {
    if (in_session) { // [orig: @0x49b3de..0x49b3f3]
        if (backward) cycle_backward();
        else cycle_forward();
        return;
    }
    // [orig: Game_GetShowWaypoints() && g_CurrentWaypoint @0x49b3f9..0x49b40a]
    const WaypointEntry *cur = current_entry();
    if (!show || cur == nullptr) return;
    if (backward) {
        if (current == 0) return; // [orig: current != g_WaypointList[0]]
        const int32_t was = current;
        cycle_backward();
        const WaypointEntry *landed = current_entry();
        if (landed == nullptr || !landed->chain_back) current = was; // [orig: +535 test]
    } else if (cur->chain_back || cur->done) { // [orig: +535 / +536 tests]
        cycle_forward();
    }
}

// [orig: SpawnPoint_SkipBlocked @ 0x4de310 — cycle forward past done entries
//  until an incomplete one or a full wrap back to the start.]
void WaypointTrack::skip_done() {
    const WaypointEntry *cur = current_entry();
    if (!cur || !cur->done) return;
    const int32_t start = current;
    do {
        cycle_forward();
        cur = current_entry();
    } while (cur && current != start && cur->done);
}

// The current-waypoint slice of the per-frame HUD info rebuild, plus the
// mission-scripted show gate. [orig: HUD_BuildEntityInfo @ 0x4b88b7..0x4b8914
// (hudInfo+373 number, +400/404/408 position) + g_ShowWaypoints @ 0x27238BC]
WaypointHudView waypoint_hud_view(const WaypointTrack &track, const EntityRegistry *registry) {
    WaypointHudView v;
    v.show = track.show;
    v.count = static_cast<int32_t>(track.entries.size());
    const WaypointEntry *cur = track.current_entry();
    v.current = cur ? track.current : -1;
    if (cur != nullptr) {
        v.entry = *cur;
        // The pool-3 marker the entry mirrors [orig: g_CurrentWaypoint is the
        // entity pointer itself; its def +32, def+80 / def+84, entity+538].
        const Entity *marker = registry != nullptr
                ? registry->get(EntityHandle::make(3, cur->node))
                : nullptr;
        if (marker != nullptr) {
            v.has_def = marker->has_item_def;
            v.def_type = marker->has_item_def ? marker->item_id : 0;
            v.def_attrib = marker->has_item_def ? marker->item_attrib : 0u;
            v.zone_number = marker->zone_number;
        }
    }
    return v;
}

} // namespace opennova::world
