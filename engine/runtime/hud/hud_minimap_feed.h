#pragma once

// The retained minimap marker feed: the packed int32 form the marker rows
// cross a scripting seam in ({version, stride, count} then `stride` ints per
// HudMinimapMarker). One layout, owned here; a producer encodes, a consumer
// decodes, and neither restates the columns.
//
// v3 appended the client-resolved draw policy {policy_flags (bit0 rotate,
// bit1 footprint), half_x_q16, half_y_q16, floor_px}; v4 appends {medic} —
// the local-team charattr Medic bit the map marker pass draws the red-cross
// plate for (retail: draw_entity_labels_and_markers @0x5a49e0 —
// AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3 under the local-team gate
// @0x5a4ac6/@0x5a4acf, see docs/interface/hud-re.md).

#include <cstddef>
#include <cstdint>
#include <vector>

#include <runtime/hud/hud_minimap.h>

namespace opennova::hud {

inline constexpr int32_t kMinimapFeedVersion = 4;
inline constexpr int32_t kMinimapFeedHeaderSize = 3;
inline constexpr int32_t kMinimapFeedStride = 17;

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

} // namespace opennova::hud
