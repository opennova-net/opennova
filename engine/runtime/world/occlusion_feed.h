#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace opennova::world {

// One building's occlusion verdict as the feed transports it: the RAW 32-bit
// section mask (bit N = COBJ section / render part N; bit 0 = exterior; the
// def's forced-visible bits travel beside it, OcclusionWorld::
// forced_section_mask) in the low word, the batch/frustum visible flag at
// bit 32. No engine witness: the packing is the feed's own transport so a
// presenter reads one integer per building.
constexpr int kBuildingVisibleBit = 32;

constexpr int64_t pack_building_visibility(uint32_t section_mask, bool visible) {
    return static_cast<int64_t>(section_mask) |
           (visible ? (int64_t(1) << kBuildingVisibleBit) : int64_t(0));
}

constexpr uint32_t building_visibility_mask(int64_t packed) {
    return static_cast<uint32_t>(packed & 0xFFFFFFFFll);
}

constexpr bool building_visibility_visible(int64_t packed) {
    return ((packed >> kBuildingVisibleBit) & 1) != 0;
}

// The delta between the current culled-id set and the one a presenter already
// applied: `added` = now - applied, `removed` = applied - now (both sorted),
// after which the current set becomes the applied baseline. No engine witness:
// the feed exists so a presenter re-asserts only what changed.
inline void culled_changes_since(const std::vector<int32_t> &now,
                                 std::vector<int32_t> &applied,
                                 std::vector<int32_t> &added,
                                 std::vector<int32_t> &removed) {
    std::vector<int32_t> current = now;
    std::sort(current.begin(), current.end());
    added.clear();
    removed.clear();
    std::set_difference(current.begin(), current.end(), applied.begin(), applied.end(),
                        std::back_inserter(added));
    std::set_difference(applied.begin(), applied.end(), current.begin(), current.end(),
                        std::back_inserter(removed));
    applied = std::move(current);
}

} // namespace opennova::world
