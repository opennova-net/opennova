#include <runtime/world/entity_index.h>
#include "common/test_expect.h"

using namespace opennova::world;

int main() {
    EntityIndex index;
    TEST_EXPECT(index.generation() == 0 && index.entries().empty());
    const std::vector<EntityIndexEntry> entries{
        {11, 101, 1, 0x1000000, 5, {0, 0, 0}},
        {22, 102, 2, 0, 5, {10, 10, 10}},
        {33, 103, 1, 7, -1, {5, 5, 500}},
        {44, 0, -1, 7, 0, {11, 5, 5}},
        {55, -8, 2, -1, 0, {5, -1, 5}},
    };
    opennova::mission::AreaTriggerRecord zone{};
    zone.min_x = zone.min_y = zone.min_z = 10; // Deliberately reversed.
    zone.max_x = zone.max_y = zone.max_z = 0;
    zone.active = false; // Presentation lookup does not require active zones.
    index.build(entries, {zone});
    TEST_EXPECT(index.generation() == 1 && index.entries().size() == 5);
    TEST_EXPECT(index.by_bms_id(101) == 11 && index.by_bms_id(-8) == 55);
    TEST_EXPECT(index.by_bms_id(0) == 0 && index.by_bms_id(999) == 0);
    TEST_EXPECT((index.candidates(101, 2, 0) == std::array<uint64_t, 2>{11, 22}));
    TEST_EXPECT((index.candidates(999, 1, 0x1000000) == std::array<uint64_t, 2>{0, 11}));
    TEST_EXPECT((index.candidates(0, 2, 0) == std::array<uint64_t, 2>{0, 22}));
    TEST_EXPECT((index.candidates(0, -1, 7) == std::array<uint64_t, 2>{0, 0}));
    TEST_EXPECT((index.candidates(0, 2, -1) == std::array<uint64_t, 2>{0, 0}));
    TEST_EXPECT(index.group(5) == std::vector<uint64_t>({11, 22}));
    TEST_EXPECT(index.group(0) == std::vector<uint64_t>({44, 55}));
    TEST_EXPECT(index.group(-1).empty() && index.group(999).empty());
    TEST_EXPECT(index.zone(0) == std::vector<uint64_t>({11, 22, 33}));
    TEST_EXPECT(index.zone(-1).empty() && index.zone(1).empty());
    zone.constrain_z = true;
    index.build(entries, {zone});
    TEST_EXPECT(index.generation() == 2 && index.zone(0) == std::vector<uint64_t>({11, 22}));

    // Single-identity lookups keep the last registration; group and full-set
    // traversal retain registration order, including duplicate identities.
    auto repeated = entries;
    repeated.push_back({66, 101, 2, 0, 5, {5, 5, 5}});
    index.build(repeated, {});
    TEST_EXPECT(index.by_bms_id(101) == 66);
    TEST_EXPECT((index.candidates(101, 2, 0) == std::array<uint64_t, 2>{66, 66}));
    TEST_EXPECT(index.group(5) == std::vector<uint64_t>({11, 22, 66}));
    TEST_EXPECT(index.entries().front().token == 11 && index.entries().back().token == 66);
    TEST_EXPECT(index.zone(0).empty());
    const auto generation = index.generation();
    index.clear();
    TEST_EXPECT(index.generation() == generation + 1);
    TEST_EXPECT(index.by_bms_id(101) == 0 && index.group(5).empty() && index.entries().empty());
    index.build({{77, 201, 4, 0, 9, {}}}, {});
    TEST_EXPECT(index.generation() == generation + 2 && index.by_bms_id(201) == 77);
    TEST_EXPECT((index.candidates(0, 2, 0) == std::array<uint64_t, 2>{0, 0}));
    return 0;
}
