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

// Whether a model's part `section` draws. Retail hides part i when bit (i & 31)
// of the entity's destroyed mask (+0x138) or of g_HiddenSectionMask (@0xB7965C)
// is set; that mask is ~(raw | forced) for a batched building and 0 for every
// other sector entity, so a model with no verdict (`occlusion_mask` -1, the
// port's "no verdict") hides only its destroyed sections [orig:
// BoneCallback_bldg_World @0x4E22CF..0x4E22E2 -- `mov edi,[ebp+138h]`,
// `shl ebx,cl` (the count masked to 5 bits), `or edi,esi` with the hidden mask
// loaded @0x4E22C2, `test ebx,edi`; Terrain_RenderSectorModels
// @0x5C5D72..0x5C5E4F the building's mask, @0x5C7B6B the others' 0].
constexpr bool section_visible(uint32_t destroyed_mask, int64_t occlusion_mask, uint32_t forced_mask,
                               int section) {
    const uint32_t bit = 1u << (static_cast<uint32_t>(section) & 31u);
    if ((destroyed_mask & bit) != 0) return false;
    if (occlusion_mask == -1) return true;
    return ((static_cast<uint32_t>(occlusion_mask) | forced_mask) & bit) != 0;
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
