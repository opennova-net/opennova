#pragma once

// The non-bank map legs' feed (hud::HudMinimapOverlays): the pool-3 zone and
// location entities, the pool-4 player waypoints, the tracked callout, the
// transient-bank persons' display names, the zone waypoint names and the
// session globals the map legs read — gathered from the role's replica state
// and its world, the witnessed selection rules applied here so the embedder
// only marshals the value.
// [orig: HUD_DrawMapOverlay @0x5A5F40 pool-3 walk @0x5a7504, pool-4 walk
//  @0x5a770b; HUD_DrawEntityLabelsAndMarkers @0x5A49E0 loop 1 over the S2C
//  0x4C player-slot table; HUD_SetTrackedEntityTarget @0x59D050;
//  Entity_GetDisplayName @0x59BF70; the KOTH ring Minimap_DrawKothZoneRing @0x5974E0; see
//  docs/interface/hud-re.md]

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
    // The wire handle the local player goes by on a joiner (its S2C 0x4C
    // entries and decoded rows name it so); kInvalid = the world handle.
    uint16_t self_handle = 0xFFFF;
    // The current profile record's inverse OPTIONS_AUTOMEDIC word (+1660), which
    // the own slot's revive leg reads live each frame; 0 = automatic requests.
    // [orig: draw_entity_labels_and_markers @0x5a4b49..0x5a4b56]
    int32_t own_auto_medic_off = 0;
};

// Fills `out` (replaced).
void build_minimap_overlays(const MinimapOverlayInputs &in, hud::HudMinimapOverlays &out);

} // namespace opennova::inmatch
