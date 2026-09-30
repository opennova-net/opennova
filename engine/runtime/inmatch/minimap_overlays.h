#pragma once

// The non-bank map legs' feed (hud::HudMinimapOverlays): the pool-3 zone and
// location entities, the pool-4 player waypoints, the tracked callout, the
// transient-bank persons' display names, the zone waypoint names and the
// session globals the map legs read — gathered from the role's replica state
// and its world, the witnessed selection rules applied here so the embedder
// only marshals the value.
// [orig: HUD_DrawMapOverlay @0x5A5F40 pool-3 walk @0x5a7504, pool-4 walk
//  @0x5a770b; HUD_SetTrackedEntityTarget @0x59D050; Entity_GetDisplayName
//  @0x59BF70; see docs/interface/hud-re.md]

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/replication/client_state.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

struct MinimapOverlayInputs {
    // The role's replica state; null = no session.
    const replication::ClientState *client = nullptr;
    world::World *world = nullptr;
    int32_t game_type = 0;
    // The session rules word; bit 0x400 nulls every display name.
    uint32_t rules_word = 0;
    // The authority's own location table when the replica carries none.
    const std::vector<std::string> *authority_location_names = nullptr;
    hud::GameTextLookup gametext;
};

// Fills `out` (replaced).
void build_minimap_overlays(const MinimapOverlayInputs &in, hud::HudMinimapOverlays &out);

} // namespace opennova::inmatch
