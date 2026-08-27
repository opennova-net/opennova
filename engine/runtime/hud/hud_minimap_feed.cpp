// The retained minimap marker feed -- see hud_minimap_feed.h.

#include <runtime/hud/hud_minimap_feed.h>

#include <algorithm>

namespace opennova::hud {

void minimap_feed_encode(const std::vector<HudMinimapMarker> &markers,
                         std::vector<int32_t> &out) {
    out.assign(static_cast<std::size_t>(kMinimapFeedHeaderSize) +
                   markers.size() * static_cast<std::size_t>(kMinimapFeedStride),
               0);
    out[0] = kMinimapFeedVersion;
    out[1] = kMinimapFeedStride;
    out[2] = static_cast<int32_t>(markers.size());
    int32_t *dst = out.data() + kMinimapFeedHeaderSize;
    for (const HudMinimapMarker &m : markers) {
        dst[0] = m.bank;
        dst[1] = m.handle;
        dst[2] = m.x;
        dst[3] = m.y;
        dst[4] = m.z;
        dst[5] = m.heading_bam;
        dst[6] = m.icon;
        dst[7] = static_cast<int32_t>(m.color);
        dst[8] = m.flags;
        dst[9] = m.source;
        dst[10] = m.remaining_ticks;
        dst[11] = m.entity_known ? 1 : 0;
        dst[12] = (m.rotate ? 1 : 0) | (m.footprint ? 2 : 0);
        dst[13] = m.half_x_q16;
        dst[14] = m.half_y_q16;
        dst[15] = m.floor_px;
        // v4: the local-team medic bit the marker pass draws the red-cross
        // plate for [orig: draw_entity_labels_and_markers @0x5a49e0 —
        // AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3 under the local-team
        // gate @0x5a4ac6/@0x5a4acf].
        dst[16] = m.medic ? 1 : 0;
        dst += kMinimapFeedStride;
    }
}

bool minimap_feed_decode(const int32_t *data, std::size_t size,
                         std::vector<HudMinimapMarker> &out) {
    out.clear();
    if (data == nullptr || size < static_cast<std::size_t>(kMinimapFeedHeaderSize) ||
        data[0] != kMinimapFeedVersion)
        return false;
    const int32_t stride = data[1];
    const int32_t declared_count = data[2];
    if (stride < kMinimapFeedStride || declared_count < 0) return false;
    const std::size_t available =
        (size - static_cast<std::size_t>(kMinimapFeedHeaderSize)) / static_cast<std::size_t>(stride);
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(declared_count), available);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int32_t *row = data + kMinimapFeedHeaderSize + i * static_cast<std::size_t>(stride);
        HudMinimapMarker marker;
        marker.bank = static_cast<uint8_t>(row[0]);
        marker.handle = static_cast<uint16_t>(row[1]);
        marker.x = row[2];
        marker.y = row[3];
        marker.z = row[4];
        marker.heading_bam = row[5];
        marker.icon = static_cast<uint8_t>(row[6]);
        marker.color = static_cast<uint32_t>(row[7]);
        marker.flags = static_cast<uint8_t>(row[8]);
        marker.source = static_cast<uint8_t>(row[9]);
        marker.remaining_ticks = static_cast<uint16_t>(row[10]);
        marker.entity_known = row[11] != 0 ? 1 : 0;
        marker.rotate = (row[12] & 1) != 0 ? 1 : 0;
        marker.footprint = (row[12] & 2) != 0 ? 1 : 0;
        marker.half_x_q16 = row[13];
        marker.half_y_q16 = row[14];
        marker.floor_px = static_cast<uint8_t>(row[15]);
        marker.medic = row[16] != 0 ? 1 : 0;
        out.push_back(marker);
    }
    return true;
}

} // namespace opennova::hud
