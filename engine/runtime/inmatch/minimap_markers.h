#pragma once

// The retained minimap marker rows a client draws: the three retained banks
// of the client minimap state walked in bank order (transient, persistent,
// special), each row's draw policy resolved against the LOCAL entity (host:
// the live registry; joiner: the materialized twin — retail reads the pool
// slot's def at draw time the same way, witness at
// world::minimap_blip_draw_policy) and its local-team medic bit, plus the
// locally deployed player's own row restored into the regular persistent
// bank when no decoded regular row already covers its wire handle.
// [orig: Render_MinimapSlotBlip @0x5BE240 -> Minimap_DrawBlip, the regular
//  TSDicon submit @0x597F73; the medic gate HUD_DrawEntityLabelsAndMarkers
//  @0x5a49e0 — AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3 under the
//  local-team gate @0x5a4ac6/@0x5a4acf; see docs/interface/hud-re.md]

#include <cstdint>
#include <vector>

#include <runtime/replication/client_state.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

struct MinimapMarkerInputs {
    // The retained banks; null = no session (only the local row can appear).
    const replication::ClientMinimapState *map = nullptr;
    // The world the policies resolve against; null = unresolved fallbacks.
    world::World *world = nullptr;
    // The handle the wire stream knows the local player by (kInvalid = none).
    uint16_t local_marker_handle = world::EntityHandle::kInvalid;
    // The local player's presented heading for the restored row.
    int32_t local_heading_bam = 0;
};

// Builds the rows in draw order. `out` is replaced.
void build_minimap_markers(const MinimapMarkerInputs &in,
                           std::vector<hud::HudMinimapMarker> &out);

} // namespace opennova::inmatch
