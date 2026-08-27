#pragma once

// THE AAS ZONE STATUS PANEL FEED — the live half of hud/hud_lfp_panel.h: the
// spawn-zone list in retail's order joined with each zone's timer entry, the
// viewer's distance and in-cylinder test, and the contest counts — one
// HudLfpZone per registry entry for the panel element.
// [orig: the list walk in HUD_DrawZoneStatusPanel @0x5a2480 (SpawnZoneList_*
//  @0x43b920/@0x43b930); the per-marker reads in HUD_DrawZoneMarker @0x5986f0 —
//  the timer entry CProximityList_FindEntryById @0x598730, the team byte
//  +0x162 @0x59873a, the cylinder @0x5987a6..0x598810, the counts
//  @0x598866/@0x599009..0x59902b, the distance @0x5990ac..0x599108]
// Witness record: docs/interface/hud-re.md "AAS zone status panel".

#include <runtime/world/entity.h>
#include <runtime/world/spawn_select.h>

#include <runtime/hud/hud_frame.h>

#include <functional>
#include <vector>

namespace opennova::world {

class World; // the class-key must match world.h (MSVC mangles struct/class apart)

// The zone-timer entry as the panel reads it (the client runtime owns the
// 13-DWORD image; engine/runtime/world stays net-agnostic, so the embedder
// supplies a lookup). DWORD 1 team, 8 value, 9 control target, 10 limit,
// 11 rate, 12 active, plus the two 0x6F contest bytes.
struct LfpZoneTimer {
    int team = 0;
    int32_t value = 0;
    int32_t control = 0;
    int32_t limit = 0;
    int32_t rate = 0;
    bool active = false;
    uint8_t count_owner = 0;
    uint8_t count_other = 0;
};
// Returns true and fills `out` when the zone has a timer entry.
using LfpZoneTimerLookup = std::function<bool(EntityHandle, LfpZoneTimer &)>;
// The transient minimap slot's flag byte for the zone (+4), whose 0xC0 bits
// gate the marker [orig: the word_28E5620 walk @0x5a2517..0x5a256e]; the
// embedder reads its minimap state. Absent slot -> 0.
using LfpCaptureFlagsLookup = std::function<uint8_t(EntityHandle)>;

// Fill `out` with one row per registry entry, in list order. `local` is the
// viewer (its position feeds the distance/cylinder tests, its team the
// counts' sides); `local_team` the viewer's team byte.
void build_lfp_zones(const World &world, const SpawnZoneRegistry &registry,
                     const Entity &local, int local_team,
                     const LfpZoneTimerLookup &timer,
                     const LfpCaptureFlagsLookup &capture_flags,
                     std::vector<hud::HudLfpZone> &out);

} // namespace opennova::world
