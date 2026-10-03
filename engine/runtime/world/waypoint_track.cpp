#include <runtime/world/waypoint_track.h>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <runtime/world/entity_registry.h>

namespace opennova::world {

namespace {

// An entry's entity still holds its item def: the walks' `+32` test. Every
// retail spawn links a def (row 0 for a type items.def lacks) and the destroy
// clears it, so the registry's own presence is that test here (the port's
// has_item_def is false for a type with no items.def row). Without a registry
// every entry counts as live.
bool entry_live(const WaypointEntry &entry, const EntityRegistry *registry) {
    return registry == nullptr || registry->get(entry.handle()) != nullptr;
}

// The waypoint gametypes the route list and the goal gate belong to
// [orig: `and edx, 0FFFDFFFFh; cmp edx, 10020h` @0x4de60c..0x4de614].
bool waypoint_game_type(uint32_t game_type) {
    return (game_type & 0xFFFDFFFFu) == 0x10020u;
}

// The goal gate over the current entry's four goal bytes: every nonzero byte i
// needs SubGoalWon bit i+1. `met` reports that at least one byte was set and
// passed (the immediate advance); a null entry passes.
// [orig: SpawnPoint_CheckWeaponRestrictions @0x4dbe80 (a misnomer) -- the
//  mask EventSystem_GetEntityCounts @0x452e10 = dword_AC86F4, the walk
//  @0x4dbeb4..0x4dbed8]
bool goals_pass(const WaypointEntry *entry, uint32_t won, bool &met) {
    met = false;
    if (entry == nullptr) return true; // [orig: @0x4dbea4..0x4dbeab]
    for (int i = 0; i < 4; ++i) {
        if (entry->goals[i] == 0) continue;
        if ((won & (1u << (i + 1))) == 0) {
            met = false; // [orig: @0x4dbee6]
            return false;
        }
        met = true; // [orig: @0x4dbecc]
    }
    return true;
}

} // namespace

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
        const Entity *marker = registry.get(entries[index].handle());
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

// [orig: Player_UpdatePerFrame @ 0x4de5f7..0x4de72c]
void WaypointTrack::tick_advance(const WaypointFrameInputs &in) {
    const int32_t count = static_cast<int32_t>(entries.size());
    if (count > 1 && waypoint_game_type(in.game_type)) {
        const WaypointEntry *cur = current_entry();
        // Authority: an event-linked waypoint never proximity-advances — its
        // completion arrives via on_event_fired. [orig: @ 0x4de649 jge skip]
        if (cur && cur->linked_event > 0) return;
        bool goals_met = false;
        if (!goals_pass(cur, in.subgoals_won, goals_met)) return; // [orig: @0x4de664]
        if (!cur) {
            // Null current latches the first entry, skipping completed ones.
            // [orig: @ 0x4de6de-0x4de6ea]
            current = 0;
            skip_done(in);
            return;
        }
        // Every goal the waypoint asks for is won: advance at once, wherever
        // the player stands [orig: `cmp [esp+outEntry], ebx` @0x4de68e ->
        // Spectator_CycleTarget(1) @0x4de695].
        if (goals_met) {
            cycle_forward(in);
            return;
        }
        // NovaLogic horizontal approx distance: max + min/2 over |dx|, |dy|,
        // preceded by the per-axis early accept, from the marker entity's live
        // X/Y [orig: `sub eax, [esi+4]` / `sub eax, [esi+8]` @0x4de676..0x4de682;
        // the abs and compare @ 0x4de687..0x4de6c2].
        int32_t marker_x = cur->x;
        int32_t marker_y = cur->y;
        if (const Entity *marker = in.registry != nullptr ? in.registry->get(cur->handle()) : nullptr) {
            marker_x = to_fixed(marker->position.x);
            marker_y = to_fixed(marker->position.y);
        }
        const int32_t dx = std::abs(in.player_x - marker_x);
        const int32_t dy = std::abs(in.player_y - marker_y);
        const uint32_t radius = static_cast<uint32_t>(cur->radius);
        if (static_cast<uint32_t>(dx) > radius && static_cast<uint32_t>(dy) > radius) return;
        const uint32_t approx = (dx <= dy)
                ? static_cast<uint32_t>(dy) + (static_cast<uint32_t>(dx) >> 1)
                : static_cast<uint32_t>(dx) + (static_cast<uint32_t>(dy) >> 1);
        // Never advance past the last entry. [orig: @ 0x4de6ca cmp vs list[count-1]]
        if (approx < radius && current != count - 1)
            cycle_forward(in);
        return;
    }
    if (count == 1) {
        current = 0; // [orig: @ 0x4de6f3..0x4de6fa single-entry latch]
        return;
    }
    // Any other list with nothing selected: the CTF pair picks the nearest
    // enemy base, the rest the first live entry [orig: @0x4de701..0x4de72c].
    if (count == 0 || current_entry() != nullptr) return;
    if (in.game_type == 0x10004u || in.game_type == 0x10008u) {
        if (in.registry != nullptr && in.local != nullptr)
            select_nearest_enemy_base(*in.registry, *in.local, in.game_type);
        return;
    }
    current = 0;
    skip_done(in);
}

void WaypointTrack::build_map_poi_list(const EntityRegistry &registry) {
    // [orig: Entity_BuildMapPoiLists @0x42de40]
    std::vector<WaypointEntry> list;
    const auto add = [&list](const Entity &e) {
        WaypointEntry entry;
        entry.node = e.handle.slot();
        entry.pool = static_cast<uint8_t>(e.handle.pool());
        entry.x = to_fixed(e.position.x);
        entry.y = to_fixed(e.position.y);
        entry.z = to_fixed(e.position.z);
        entry.radius = to_fixed(e.bound_radius);
        list.push_back(entry);
    };
    // Pool 2: the targets (def attrib 0x8000) [orig: @0x42de58..0x42de71].
    registry.for_each_in_pool(2, [&](const Entity &e) {
        if (e.has_item_def && (e.item_attrib & 0x8000u) != 0) add(e);
    });
    // Pool 1: the flags, then the same row again when its def is a target,
    // so a target flag lists twice [orig: @0x42de91..0x42dede].
    registry.for_each_in_pool(1, [&](const Entity &e) {
        if (!e.has_item_def) return;
        if (e.item_id == 4093 || e.item_id == 4091 || e.item_id == 4095) add(e);
        if ((e.item_attrib & 0x8000u) != 0) add(e);
    });
    // Pool 1 again: the flag bays. The local team's own bay also lands in a
    // 32-entry team list whose only readers (HUD_GetNearestWaypointAngleAndColor
    // @0x5bb1b3 and its siblings) have no caller in the image, so the port
    // keeps no copy [orig: @0x42df00..0x42e021].
    registry.for_each_in_pool(1, [&](const Entity &e) {
        if (!e.has_item_def) return;
        if (e.item_id == 4100 || e.item_id == 4098 || e.item_id == 4103 || e.item_id == 4102)
            add(e);
    });
    // Pool 3: the zones [orig: @0x42e040..0x42e07a].
    registry.for_each_in_pool(3, [&](const Entity &e) {
        if (!e.has_item_def) return;
        const int32_t t = e.item_id;
        if (t == 6026 || t == 6027 || t == 6028 || t == 6092 || t == 6093) add(e);
    });
    // Pool 2 again: the armories (def attrib 0x80000) [orig: @0x42e0a0..0x42e0bd].
    registry.for_each_in_pool(2, [&](const Entity &e) {
        if (e.has_item_def && (e.item_attrib & 0x80000u) != 0) add(e);
    });
    // The count store; g_CurrentWaypoint is left as it was [orig: @0x42e0d4].
    const WaypointEntry *was = current_entry();
    const EntityHandle selected = was != nullptr ? was->handle() : EntityHandle{};
    entries = std::move(list);
    current = -1;
    if (selected.valid()) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].handle() == selected) {
                current = static_cast<int32_t>(i);
                break;
            }
        }
    }
}

// [orig: EventTrigger_MarkLinkedSpawnPoints @ 0x452ce0 — linked entries get
//  done=1, then walk BACKWARD while the predecessor's chain flag (+535) is set;
//  ends with SpawnPoint_SkipBlocked. The `> 0` gate is the original's: markers
//  cannot link to event 0.]
void WaypointTrack::on_event_fired(int32_t event_index, const WaypointCycleContext &ctx) {
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
    skip_done(ctx);
}

// [orig: Spectator_CycleTarget @ 0x4dc1d0]
void WaypointTrack::cycle(bool backward, const WaypointCycleContext &ctx, int depth) {
    const int32_t count = static_cast<int32_t>(entries.size());
    if (count <= 1) return; // [orig: @0x4dc1d9]
    // A list with no live entry empties itself [orig: @0x4dc1df..0x4dc213].
    bool any_live = false;
    for (const WaypointEntry &entry : entries)
        if (entry_live(entry, ctx.registry)) {
            any_live = true;
            break;
        }
    if (!any_live) {
        entries.clear();
        current = -1;
        return;
    }
    // A current not in the list scans from index 0 (@0x4dc246), so a forward
    // step from no selection lands on index 1.
    const int32_t from = current >= 0 && current < count ? current : 0;
    int32_t target = current;
    bool found = false;
    if (backward) {
        // [orig: @0x4dc29a..0x4dc2d6] down from the previous index, then
        // down from the last to the current one inclusive.
        for (int32_t i = from - 1; i >= 0 && !found; --i)
            if (entry_live(entries[static_cast<size_t>(i)], ctx.registry)) target = i, found = true;
        for (int32_t i = count - 1; i >= from && !found; --i)
            if (entry_live(entries[static_cast<size_t>(i)], ctx.registry)) target = i, found = true;
    } else {
        // [orig: @0x4dc251..0x4dc296] up from the next index, then up from
        // 0 to the current one inclusive.
        for (int32_t i = from + 1; i < count && !found; ++i)
            if (entry_live(entries[static_cast<size_t>(i)], ctx.registry)) target = i, found = true;
        for (int32_t i = 0; i <= from && !found; ++i)
            if (entry_live(entries[static_cast<size_t>(i)], ctx.registry)) target = i, found = true;
    }
    if (found) current = target; // [orig: @0x4dc2d8]
    const EntityHandle landed = current >= 0 && current < count
            ? entries[static_cast<size_t>(current)].handle() : EntityHandle{};
    // In session in the KOTH pair a dead target entry steps once more, the
    // guard holding the entry it stepped from [orig: @0x4dc2de..0x4dc324 --
    // game types 0x90002 / 0x10002, def attrib 0x8000, the signed Health word
    // +286 <= 0, dword_B79434]. Retail recurses without bound; two dead
    // targets alone would alternate forever, so the port stops after one
    // pass over the list.
    if (ctx.in_session && ctx.registry != nullptr &&
            (ctx.game_type == 589826u || ctx.game_type == 65538u) && landed.valid() &&
            depth < count) {
        const Entity *e = ctx.registry->get(landed);
        if (e != nullptr && e->has_item_def && (e->item_attrib & 0x8000u) != 0 &&
                static_cast<int16_t>(e->health) <= 0 && cycle_guard_ != landed) {
            cycle_guard_ = landed;
            cycle(backward, ctx, depth + 1);
            return;
        }
    }
    cycle_guard_ = landed; // [orig: @0x4dc333]
}

void WaypointTrack::cycle_forward(const WaypointCycleContext &ctx) {
    cycle(false, ctx, 0);
}

void WaypointTrack::cycle_backward(const WaypointCycleContext &ctx) {
    cycle(true, ctx, 0);
}

void WaypointTrack::manual_cycle(bool backward, const WaypointCycleContext &ctx) {
    if (ctx.in_session) { // [orig: @0x49b3de..0x49b3f3]
        if (backward) cycle_backward(ctx);
        else cycle_forward(ctx);
        return;
    }
    // [orig: Game_GetShowWaypoints() && g_CurrentWaypoint @0x49b3f9..0x49b40a]
    const WaypointEntry *cur = current_entry();
    if (!show || cur == nullptr) return;
    if (backward) {
        if (current == 0) return; // [orig: current != g_WaypointList[0]]
        const int32_t was = current;
        cycle_backward(ctx);
        const WaypointEntry *landed = current_entry();
        if (landed == nullptr || !landed->chain_back) current = was; // [orig: +535 test]
    } else if (cur->chain_back || cur->done) { // [orig: +535 / +536 tests]
        cycle_forward(ctx);
    }
}

// [orig: SpawnPoint_SkipBlocked @ 0x4de310 — cycle forward past done entries
//  until an incomplete one or a full wrap back to the start.]
void WaypointTrack::skip_done(const WaypointCycleContext &ctx) {
    const WaypointEntry *cur = current_entry();
    if (!cur || !cur->done) return;
    const int32_t start = current;
    do {
        cycle_forward(ctx);
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
        // The entity the entry names [orig: g_CurrentWaypoint is the entity
        // pointer itself; its def +32, def+80 / def+84, entity+538, and the
        // live Position @0x4b88ee..0x4b890c].
        const Entity *marker = registry != nullptr ? registry->get(cur->handle()) : nullptr;
        if (marker != nullptr) {
            v.has_def = marker->has_item_def;
            v.def_type = marker->has_item_def ? marker->item_id : 0;
            v.def_attrib = marker->has_item_def ? marker->item_attrib : 0u;
            v.zone_number = marker->zone_number;
            v.entry.x = to_fixed(marker->position.x);
            v.entry.y = to_fixed(marker->position.y);
            v.entry.z = to_fixed(marker->position.z);
        }
    }
    return v;
}

} // namespace opennova::world
