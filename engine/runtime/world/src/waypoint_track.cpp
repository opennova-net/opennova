#include "world/waypoint_track.h"

#include <cstdlib>

namespace opennova::world {

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
//  entries are immutable marker mirrors, so every entry stays cyclable.]
void WaypointTrack::cycle_forward() {
    const int32_t count = static_cast<int32_t>(entries.size());
    if (count <= 1) return;
    if (current < 0 || current >= count) {
        current = 0;
        return;
    }
    current = (current + 1) % count;
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

} // namespace opennova::world
