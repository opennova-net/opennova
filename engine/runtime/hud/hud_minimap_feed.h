#pragma once

// The retained minimap marker feed: the packed int32 form the marker rows
// cross a scripting seam in ({version, stride, count} then `stride` ints per
// HudMinimapMarker). One layout, owned here; a producer encodes, a consumer
// decodes, and neither restates the columns.
//
// v3 appended the client-resolved draw policy {policy_flags (bit0 rotate,
// bit1 footprint), half_x_q16, half_y_q16, floor_px}; v4 appends {medic} —
// the local-team charattr Medic bit the map marker pass draws the red-cross
// plate for [orig: HUD_DrawEntityLabelsAndMarkers @0x5a49e0 —
// CharAttr_ClassHasAttribute(playerClass, 8) @0x5a4ab3 under the local-team gate
// @0x5a4ac6/@0x5a4acf, see docs/interface/hud-re.md]; v5 appends the live
// pool-entity facts {team, zone_number, def_type, entity_bits, zone_index,
// zone_radius, entity_x, entity_y, anchor_x, anchor_y, bound_radius_q16} the
// drawer and the later map legs read behind the slot handle
// (HudMinimapMarker carries the witnesses); v6 appends {entity_z,
// bay_groups} for the vehicle-bay logo walk (hud_bay_logos.h), the bay bit
// riding entity_bits; v7 appends the zone-timer entry keyed by the slot's
// entity {timer_flags (bit0 known, bit1 active), team, bar_team, value, limit,
// rate} for the capture-point labels (hud_capture_labels.h).

#include <cstddef>
#include <cstdint>
#include <vector>

#include <runtime/hud/hud_minimap.h>

namespace opennova::hud {

inline constexpr int32_t kMinimapFeedVersion = 7;
inline constexpr int32_t kMinimapFeedHeaderSize = 3;
inline constexpr int32_t kMinimapFeedStride = 36;

// {version, stride, count} + count rows.
void minimap_feed_encode(const std::vector<HudMinimapMarker> &markers,
                         std::vector<int32_t> &out);

// Parses a feed into markers. A short header, a foreign version, a stride
// below this layout's or a negative count yields false with `out` cleared —
// the consumer draws no markers. A declared count past the available rows is
// clamped to what the buffer holds (a stride above the layout's skips the
// extra ints per row).
bool minimap_feed_decode(const int32_t *data, std::size_t size,
                         std::vector<HudMinimapMarker> &out);

// The radar-contact snapshot's packed form (HudMinimapRadar): {version,
// flags (bit0 NoTracers), local_x, local_y, red12, olive12, red24, olive24
// (one bit per sector), threat_count} then threat_count x {present, x, y}.
inline constexpr int32_t kRadarFeedVersion = 1;
inline constexpr int32_t kRadarFeedHeaderSize = 9;
inline constexpr int32_t kRadarFeedThreatStride = 3;

void radar_feed_encode(const HudMinimapRadar &radar, std::vector<int32_t> &out);
// A short or foreign header yields false with `out` cleared (the legs draw
// nothing lit); a declared count past the buffer clamps to what it holds.
bool radar_feed_decode(const int32_t *data, std::size_t size, HudMinimapRadar &out);

} // namespace opennova::hud
