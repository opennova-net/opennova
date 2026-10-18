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
    // v7: the capture-point labels' zone-timer entry.
    m.timer_known = 1;
    m.timer_active = 1;
    m.timer_team = 2;
    m.timer_bar_team = -1;
    m.timer_value = 31 * 65536;
    m.timer_limit = 62 * 65536;
    m.timer_rate = -1057;
    // v8: the change-team class's cell and the zone's taker.
    m.change_team = 1;
    m.cell = 27;
    m.capture_team = 2;
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
           a.bay_groups == b.bay_groups && a.timer_known == b.timer_known &&
           a.timer_active == b.timer_active && a.timer_team == b.timer_team &&
           a.timer_bar_team == b.timer_bar_team && a.timer_value == b.timer_value &&
           a.timer_limit == b.timer_limit && a.timer_rate == b.timer_rate &&
           a.change_team == b.change_team && a.cell == b.cell && a.capture_team == b.capture_team;
}

void test_round_trip_is_exact() {
    std::vector<HudMinimapMarker> in = {sample(5), sample(77)};
    in[1].bank = static_cast<uint8_t>(HudMinimapBank::kSpecial);
    in[1].rotate = 1;
    in[1].footprint = 0;
    in[1].medic = 0;
    in[1].timer_active = 0; // an entry with only the window leg landed
    std::vector<int32_t> feed;
    minimap_feed_encode(in, feed);
    CHECK(feed.size() == static_cast<size_t>(kMinimapFeedHeaderSize + 2 * kMinimapFeedStride));
    CHECK(feed[0] == kMinimapFeedVersion);
    CHECK(feed[1] == kMinimapFeedStride);
    CHECK(feed[2] == 2);
    CHECK(feed[kMinimapFeedHeaderSize + 12] == 6); // bit1 footprint, bit2 change team, bit0 rotate clear
    CHECK(feed[kMinimapFeedHeaderSize + 16] == 1); // v4 medic
    CHECK(kMinimapFeedVersion == 8 && kMinimapFeedStride == 38);
    CHECK(feed[kMinimapFeedHeaderSize + 36] == 27 && feed[kMinimapFeedHeaderSize + 37] == 2); // v8
    CHECK(feed[kMinimapFeedHeaderSize + 30] == 3); // v7 timer flags: known | active
    CHECK(feed[kMinimapFeedHeaderSize + kMinimapFeedStride + 30] == 1);
    CHECK(feed[kMinimapFeedHeaderSize + 35] == -1057); // the signed rate rides as is
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

// The footprint feed's layout is the engine's (C-492): an exact round trip, each
// row finalized; a foreign header draws none; a short row ends the walk.
void test_footprint_feed_round_trip() {
    std::vector<HudMinimapFootprint> in(2);
    in[0].handle = 0x2042;
    in[0].fill_argb = 0xFF60A060u;
    in[0].fill_xy_q16 = {20 << 16, -(4 << 16), 24 << 16, -(4 << 16), 22 << 16, 4 << 16};
    in[0].edge_xy_q16 = {20 << 16, -(4 << 16), 24 << 16, -(4 << 16)};
    in[1].handle = 7;
    in[1].fill_argb = 0xFFA0A0A0u;
    std::vector<int32_t> feed;
    minimap_footprint_feed_encode(in, feed);
    CHECK(feed.size() == 2 + (3 + 6 + 1 + 4) + (3 + 0 + 1 + 0));
    CHECK(feed[0] == kMinimapFootprintFeedVersion && feed[1] == 2);
    std::vector<HudMinimapFootprint> out;
    CHECK(minimap_footprint_feed_decode(feed.data(), feed.size(), out));
    CHECK(out.size() == 2 && out[0].handle == 0x2042 && out[0].fill_argb == 0xFF60A060u &&
          out[0].fill_xy_q16 == in[0].fill_xy_q16 && out[0].edge_xy_q16 == in[0].edge_xy_q16 &&
          out[1].handle == 7 && out[1].fill_xy_q16.empty());
    HudMinimapFootprint finalized = in[0];
    hud_minimap_finalize_footprint(finalized);
    CHECK(!out.empty() && out[0].bound_x_q16 == finalized.bound_x_q16 &&
          out[0].bound_radius_q16 == finalized.bound_radius_q16 && finalized.bound_radius_q16 > 0);
    std::vector<int32_t> foreign = feed;
    foreign[0] = kMinimapFootprintFeedVersion + 1;
    CHECK(!minimap_footprint_feed_decode(foreign.data(), foreign.size(), out) && out.empty());
    CHECK(minimap_footprint_feed_decode(feed.data(), 2 + 3 + 6, out) && out.empty());
}

int main() {
    test_round_trip_is_exact();
    test_empty_feed_round_trips_to_no_markers();
    test_foreign_headers_yield_no_markers();
    test_declared_count_is_clamped_to_the_buffer();
    test_a_wider_stride_skips_the_extra_columns();
    test_footprint_feed_round_trip();
    if (failures == 0) std::printf("hud_minimap_feed_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
