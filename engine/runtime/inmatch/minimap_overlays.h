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
#include <runtime/world/occlusion.h>
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

// Entity_GetDisplayName: the name the map labels and the weapon inset draw for
// an entity, false (null) when it has none: under the rules word's
// FriendlyTag-0 bit (0x400) or off pool 0; a non-player of another team than
// the HUD's; a non-player of the HUD's team names its authored name or '^' +
// the fallback name at its pool-encoded id; a player the first S2C 0x4C table
// slot driving it, " <ch>" tag "<co>" appended for a non-empty tag, all within
// 64 bytes (no such slot is null). `cs` is the role's replica (null: no
// session). [orig: Entity_GetDisplayName @0x59BF70]
bool entity_display_name(const world::Entity &e, uint8_t hud_team, uint32_t rules_word,
                         const replication::ClientState *cs, std::string &out);

// Fills `out` (replaced).
void build_minimap_overlays(const MinimapOverlayInputs &in, hud::HudMinimapOverlays &out);

// The footprint polygons of every visible footprint-class entity, in registry order: an entity
// the overlay classifier admits whose draw policy is the footprint (a building whose model
// carries the occlusion block), its OOBJ ground-slice mesh placed by its pose in its fill colour
// (world::minimap_footprint_from_occlusion, minimap_footprint_place, minimap_footprint_fill_argb;
// each occlusion model's mesh built once). `out` is replaced.
// [orig: Minimap_DrawBlip's Building leg @0x597a84..0x597b2d -> Render_CollisionWireframe
//  @0x596800]
void build_minimap_footprints(const world::World &world, const world::OcclusionWorld &occlusion,
		std::vector<hud::HudMinimapFootprint> &out);

} // namespace opennova::inmatch
