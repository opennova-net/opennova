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
        // plate for [orig: HUD_DrawEntityLabelsAndMarkers @0x5a49e0 —
        // CharAttr_ClassHasAttribute(playerClass, 8) @0x5a4ab3 under the local-team
        // gate @0x5a4ac6/@0x5a4acf].
        dst[16] = m.medic ? 1 : 0;
        // v5: the live pool-entity facts (hud_minimap.h HudMinimapMarker).
        dst[17] = m.team;
        dst[18] = m.zone_number;
        dst[19] = m.def_type;
        dst[20] = m.entity_bits;
        dst[21] = m.zone_index;
        dst[22] = m.zone_radius;
        dst[23] = m.entity_x;
        dst[24] = m.entity_y;
        dst[25] = m.anchor_x;
        dst[26] = m.anchor_y;
        dst[27] = m.bound_radius_q16;
        // v6: the vehicle-bay logo walk's altitude and spawn families.
        dst[28] = m.entity_z;
        dst[29] = m.bay_groups;
        // v7: the capture-point labels' zone-timer entry.
        dst[30] = (m.timer_known ? 1 : 0) | (m.timer_active ? 2 : 0);
        dst[31] = m.timer_team;
        dst[32] = m.timer_bar_team;
        dst[33] = m.timer_value;
        dst[34] = m.timer_limit;
        dst[35] = m.timer_rate;
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
        marker.team = static_cast<uint8_t>(row[17]);
        marker.zone_number = static_cast<uint8_t>(row[18]);
        marker.def_type = static_cast<uint8_t>(row[19]);
        marker.entity_bits = static_cast<uint8_t>(row[20]);
        marker.zone_index = static_cast<int16_t>(row[21]);
        marker.zone_radius = static_cast<uint16_t>(row[22]);
        marker.entity_x = row[23];
        marker.entity_y = row[24];
        marker.anchor_x = row[25];
        marker.anchor_y = row[26];
        marker.bound_radius_q16 = row[27];
        marker.entity_z = row[28];
        marker.bay_groups = static_cast<uint8_t>(row[29]);
        marker.timer_known = (row[30] & 1) != 0 ? 1 : 0;
        marker.timer_active = (row[30] & 2) != 0 ? 1 : 0;
        marker.timer_team = row[31];
        marker.timer_bar_team = row[32];
        marker.timer_value = row[33];
        marker.timer_limit = row[34];
        marker.timer_rate = row[35];
        out.push_back(marker);
    }
    return true;
}

namespace {

template <size_t N>
int32_t pack_sectors(const std::array<uint8_t, N> &sectors) {
    uint32_t bits = 0;
    for (size_t i = 0; i < N; ++i)
        if (sectors[i] != 0) bits |= 1u << i;
    return static_cast<int32_t>(bits);
}

template <size_t N>
void unpack_sectors(int32_t packed, std::array<uint8_t, N> &sectors) {
    for (size_t i = 0; i < N; ++i)
        sectors[i] = (static_cast<uint32_t>(packed) >> i) & 1u ? 1 : 0;
}

} // namespace

void radar_feed_encode(const HudMinimapRadar &radar, std::vector<int32_t> &out) {
    out.assign(static_cast<std::size_t>(kRadarFeedHeaderSize) +
                   radar.threats.size() * static_cast<std::size_t>(kRadarFeedThreatStride),
               0);
    out[0] = kRadarFeedVersion;
    out[1] = radar.rules_no_tracers ? 1 : 0;
    out[2] = radar.local_x;
    out[3] = radar.local_y;
    out[4] = pack_sectors(radar.red12);
    out[5] = pack_sectors(radar.olive12);
    out[6] = pack_sectors(radar.red24);
    out[7] = pack_sectors(radar.olive24);
    out[8] = static_cast<int32_t>(radar.threats.size());
    int32_t *dst = out.data() + kRadarFeedHeaderSize;
    for (const HudMinimapRadar::Threat &threat : radar.threats) {
        dst[0] = threat.present;
        dst[1] = threat.x;
        dst[2] = threat.y;
        dst += kRadarFeedThreatStride;
    }
}

bool radar_feed_decode(const int32_t *data, std::size_t size, HudMinimapRadar &out) {
    out = HudMinimapRadar{};
    if (data == nullptr || size < static_cast<std::size_t>(kRadarFeedHeaderSize) ||
        data[0] != kRadarFeedVersion || data[8] < 0)
        return false;
    out.rules_no_tracers = (data[1] & 1) != 0;
    out.local_x = data[2];
    out.local_y = data[3];
    unpack_sectors(data[4], out.red12);
    unpack_sectors(data[5], out.olive12);
    unpack_sectors(data[6], out.red24);
    unpack_sectors(data[7], out.olive24);
    const std::size_t available = (size - static_cast<std::size_t>(kRadarFeedHeaderSize)) /
                                  static_cast<std::size_t>(kRadarFeedThreatStride);
    const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(data[8]), available);
    out.threats.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int32_t *row = data + kRadarFeedHeaderSize + i * kRadarFeedThreatStride;
        out.threats[i].present = row[0] != 0 ? 1 : 0;
        out.threats[i].x = row[1];
        out.threats[i].y = row[2];
    }
    return true;
}

} // namespace opennova::hud
