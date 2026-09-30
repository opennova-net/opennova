// The retained minimap marker feed (hud/hud_minimap_feed.h): one layout for
// the producer and the consumer — the round trip is exact, a foreign header
// yields no markers, and a declared count is clamped to what the buffer holds.
#include <cstdint>
#include <cstdio>
#include <vector>

#include <runtime/hud/hud_minimap_feed.h>

using namespace opennova::hud;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

HudMinimapMarker sample(uint16_t handle) {
    HudMinimapMarker m;
    m.bank = static_cast<uint8_t>(HudMinimapBank::kPersistent);
    m.handle = handle;
    m.x = 65536 * 12;
    m.y = -65536 * 7;
    m.z = 3;
    m.heading_bam = 0x40000000;
    m.icon = 3;
    m.color = 0xFF0080FFu;
    m.flags = 0x10;
    m.source = 9;
    m.remaining_ticks = 620;
    m.entity_known = 1;
    m.rotate = 0;
    m.footprint = 1;
    m.half_x_q16 = 0x8000;
    m.half_y_q16 = 0x4000;
    m.floor_px = 6;
    m.medic = 1;
    // v5: the live pool-entity facts.
    m.team = 2;
    m.zone_number = 5;
    m.def_type = 3;
    m.entity_bits = kMarkerEntityHasModel | kMarkerEntityDead;
    m.zone_index = 4;
    m.zone_radius = 70;
    m.entity_x = 65536 * 13;
    m.entity_y = -65536 * 8;
    m.anchor_x = 65536 * 14;
    m.anchor_y = -65536 * 9;
    m.bound_radius_q16 = 65536 * 2;
    // v6: the vehicle-bay logo walk's facts.
    m.entity_z = -65536 * 3 - 17;
    m.bay_groups = 5;
    return m;
}

bool same(const HudMinimapMarker &a, const HudMinimapMarker &b) {
    return a.bank == b.bank && a.handle == b.handle && a.x == b.x && a.y == b.y && a.z == b.z &&
           a.heading_bam == b.heading_bam && a.icon == b.icon && a.color == b.color &&
           a.flags == b.flags && a.source == b.source && a.remaining_ticks == b.remaining_ticks &&
           a.entity_known == b.entity_known && a.rotate == b.rotate &&
           a.footprint == b.footprint && a.half_x_q16 == b.half_x_q16 &&
           a.half_y_q16 == b.half_y_q16 && a.floor_px == b.floor_px && a.medic == b.medic &&
           a.team == b.team && a.zone_number == b.zone_number && a.def_type == b.def_type &&
           a.entity_bits == b.entity_bits && a.zone_index == b.zone_index &&
           a.zone_radius == b.zone_radius && a.entity_x == b.entity_x &&
           a.entity_y == b.entity_y && a.anchor_x == b.anchor_x && a.anchor_y == b.anchor_y &&
           a.bound_radius_q16 == b.bound_radius_q16 && a.entity_z == b.entity_z &&
           a.bay_groups == b.bay_groups;
}

void test_round_trip_is_exact() {
    std::vector<HudMinimapMarker> in = {sample(5), sample(77)};
    in[1].bank = static_cast<uint8_t>(HudMinimapBank::kSpecial);
    in[1].rotate = 1;
    in[1].footprint = 0;
    in[1].medic = 0;
    std::vector<int32_t> feed;
    minimap_feed_encode(in, feed);
    CHECK(feed.size() == static_cast<size_t>(kMinimapFeedHeaderSize + 2 * kMinimapFeedStride));
    CHECK(feed[0] == kMinimapFeedVersion);
    CHECK(feed[1] == kMinimapFeedStride);
    CHECK(feed[2] == 2);
    CHECK(feed[kMinimapFeedHeaderSize + 12] == 2); // bit1 footprint, bit0 rotate clear
    CHECK(feed[kMinimapFeedHeaderSize + 16] == 1); // v4 medic
    std::vector<HudMinimapMarker> out;
    CHECK(minimap_feed_decode(feed.data(), feed.size(), out));
    CHECK(out.size() == 2);
    CHECK(same(out[0], in[0]));
    CHECK(same(out[1], in[1]));
}

void test_empty_feed_round_trips_to_no_markers() {
    std::vector<int32_t> feed;
    minimap_feed_encode({}, feed);
    CHECK(feed.size() == static_cast<size_t>(kMinimapFeedHeaderSize));
    std::vector<HudMinimapMarker> out = {sample(1)};
    CHECK(minimap_feed_decode(feed.data(), feed.size(), out));
    CHECK(out.empty());
}

void test_foreign_headers_yield_no_markers() {
    std::vector<int32_t> feed;
    minimap_feed_encode({sample(1)}, feed);
    std::vector<HudMinimapMarker> out;
    // Too short for a header.
    CHECK(!minimap_feed_decode(feed.data(), 2, out));
    CHECK(out.empty());
    // A foreign version.
    feed[0] = kMinimapFeedVersion + 1;
    out.push_back(sample(9));
    CHECK(!minimap_feed_decode(feed.data(), feed.size(), out));
    CHECK(out.empty());
    feed[0] = kMinimapFeedVersion;
    // A stride below this layout's.
    feed[1] = kMinimapFeedStride - 1;
    CHECK(!minimap_feed_decode(feed.data(), feed.size(), out));
    feed[1] = kMinimapFeedStride;
    // A negative count.
    feed[2] = -1;
    CHECK(!minimap_feed_decode(feed.data(), feed.size(), out));
    CHECK(minimap_feed_decode(nullptr, 0, out) == false);
}

void test_declared_count_is_clamped_to_the_buffer() {
    std::vector<int32_t> feed;
    minimap_feed_encode({sample(1), sample(2), sample(3)}, feed);
    feed[2] = 10; // declares more rows than the buffer holds
    std::vector<HudMinimapMarker> out;
    CHECK(minimap_feed_decode(feed.data(), feed.size(), out));
    CHECK(out.size() == 3);
    CHECK(out[2].handle == 3);
    feed[2] = 1; // declares fewer: only the first row is read
    CHECK(minimap_feed_decode(feed.data(), feed.size(), out));
    CHECK(out.size() == 1);
}

void test_a_wider_stride_skips_the_extra_columns() {
    // A future producer with one extra int per row still decodes today's columns.
    const int32_t stride = kMinimapFeedStride + 1;
    std::vector<int32_t> feed;
    minimap_feed_encode({sample(4), sample(8)}, feed);
    std::vector<int32_t> wide = {kMinimapFeedVersion, stride, 2};
    for (int r = 0; r < 2; ++r) {
        const int32_t *row = feed.data() + kMinimapFeedHeaderSize + r * kMinimapFeedStride;
        wide.insert(wide.end(), row, row + kMinimapFeedStride);
        wide.push_back(12345); // the extra column
    }
    std::vector<HudMinimapMarker> out;
    CHECK(minimap_feed_decode(wide.data(), wide.size(), out));
    CHECK(out.size() == 2);
    CHECK(out[0].handle == 4);
    CHECK(out[1].handle == 8);
    CHECK(same(out[1], sample(8)));
}

} // namespace

int main() {
    test_round_trip_is_exact();
    test_empty_feed_round_trips_to_no_markers();
    test_foreign_headers_yield_no_markers();
    test_declared_count_is_clamped_to_the_buffer();
    test_a_wider_stride_skips_the_extra_columns();
    if (failures == 0) std::printf("hud_minimap_feed_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
