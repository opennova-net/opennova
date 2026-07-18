// The player waypoint track: proximity auto-advance, event-driven completion with
// backward chaining, and the cycle/skip primitives. Pins the witnessed selection
// rules of Player_UpdatePerFrame @0x4de5f7, EventTrigger_MarkLinkedSpawnPoints
// @0x452ce0, Spectator_CycleTarget @0x4dc1d0, SpawnPoint_SkipBlocked @0x4de310.
// See docs/interface/hud-re.md §Waypoint HUD.
#include <cstdio>

#include "world/waypoint_track.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static WaypointEntry wp(int32_t x_units, int32_t y_units, int32_t radius_units) {
    WaypointEntry e;
    e.x = x_units << 16;
    e.y = y_units << 16;
    e.radius = radius_units << 16;
    return e;
}

int main() {
    // --- proximity advance: the NL approx distance (max + min/2) vs radius ---
    {
        WaypointTrack t;
        t.entries = {wp(0, 0, 4), wp(100, 0, 4), wp(200, 0, 4)};

        // Null current latches entry 0 regardless of distance. [orig: @0x4de6de]
        t.tick_advance(500 << 16, 500 << 16);
        CHECK(t.current == 0);

        // Far away: no advance.
        t.tick_advance(50 << 16, 0);
        CHECK(t.current == 0);

        // Inside the radius (dx=3 < 4): advance to entry 1. [orig: @0x4de695]
        t.tick_advance(3 << 16, 0);
        CHECK(t.current == 1);

        // The approx metric: dx=3, dy=3 -> 3 + 3/2 = 4.5 >= radius 4 -> HOLD
        // (the per-axis early accept passes but the combined metric fails).
        t.tick_advance((100 + 3) << 16, 3 << 16);
        CHECK(t.current == 1);

        // dx=2, dy=2 -> 2 + 1 = 3 < 4 -> advance.
        t.tick_advance((100 + 2) << 16, 2 << 16);
        CHECK(t.current == 2);

        // The LAST entry never proximity-advances. [orig: @0x4de6ca]
        t.tick_advance(200 << 16, 0);
        CHECK(t.current == 2);
    }

    // --- an event-linked waypoint blocks proximity advance ---
    {
        WaypointTrack t;
        t.entries = {wp(0, 0, 50), wp(100, 0, 50)};
        t.entries[0].linked_event = 7;
        t.current = 0;
        t.tick_advance(0, 0); // standing on it
        CHECK(t.current == 0); // event-gated: held [orig: @0x4de649]

        // The linked event fires: entry 0 completes and current skips forward.
        t.on_event_fired(7);
        CHECK(t.entries[0].done);
        CHECK(t.current == 1);
    }

    // --- backward chaining through chain_back flags ---
    {
        WaypointTrack t;
        t.entries = {wp(0, 0, 4), wp(1, 0, 4), wp(2, 0, 4), wp(3, 0, 4)};
        t.entries[0].chain_back = true;
        t.entries[1].chain_back = true;
        // entry 2 not flagged: the chain stops there when 3 completes.
        t.entries[3].linked_event = 9;
        t.current = 0;
        t.on_event_fired(9);
        CHECK(t.entries[3].done);
        CHECK(!t.entries[2].done); // predecessor without the flag: chain stops [orig: @0x452d57]
        CHECK(!t.entries[1].done);
        CHECK(t.current == 0); // current (0) is not done: no skip
    }

    // --- chain_back marks contiguous flagged predecessors ---
    {
        WaypointTrack t;
        t.entries = {wp(0, 0, 4), wp(1, 0, 4), wp(2, 0, 4)};
        t.entries[0].chain_back = true;
        t.entries[1].chain_back = true;
        t.entries[2].linked_event = 3;
        t.current = 0;
        t.on_event_fired(3);
        CHECK(t.entries[2].done);
        CHECK(t.entries[1].done); // flagged predecessor completes
        CHECK(t.entries[0].done); // and its flagged predecessor
        // Everything done: skip wraps and stays somewhere valid.
        CHECK(t.current >= 0 && t.current < 3);
    }

    // --- skip_done stops after a full wrap when all entries are done ---
    {
        WaypointTrack t;
        t.entries = {wp(0, 0, 4), wp(1, 0, 4)};
        t.entries[0].done = true;
        t.entries[1].done = true;
        t.current = 0;
        t.skip_done();
        CHECK(t.current == 0); // wrapped back to start [orig: @0x4de338 loop exit]
    }

    // --- single-entry track latches entry 0 ---
    {
        WaypointTrack t;
        t.entries = {wp(10, 10, 4)};
        t.tick_advance(0, 0);
        CHECK(t.current == 0);
        t.tick_advance(10 << 16, 10 << 16); // standing on it: stays (nothing to advance to)
        CHECK(t.current == 0);
    }

    // --- the show gate + clear ---
    {
        WaypointTrack t;
        CHECK(t.show); // [orig: init 1 @0x5a4913]
        t.show = false;
        t.entries = {wp(0, 0, 4)};
        t.clear();
        CHECK(t.show);
        CHECK(t.empty());
        CHECK(t.current_entry() == nullptr);
    }

    if (failures == 0) std::printf("waypoint_track_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
