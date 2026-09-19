#pragma once
#include <cstdint>
#include <vector>
namespace opennova::world {
struct ReverbRegion { int32_t min[3] = {}, max[3] = {}; int32_t value = 0; };
// Optional preset table overrides: Audio_LoadReverbDefs @0x766D80 and
// parser @0x7BF5E4. The original mixer output witness finds no live sample
// consumer for the selected coefficients; selection remains gameplay state.
struct ReverbState {
    int32_t mission = 0, selected = 0;
    std::vector<ReverbRegion> regions;
    // Strict bounds, last matching case-4 region wins. Zero is an authored
    // region override but an absent building override.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B5F9E..0x4B614F, setter @0x4B633F]
    void update(const int32_t position[3], int32_t building) {
        selected = building != 0 ? building : mission;
        for (const auto &r : regions) {
            bool inside = true;
            for (int i = 0; i < 3; ++i) inside = inside && position[i] > r.min[i] && position[i] < r.max[i];
            if (inside) selected = r.value;
        }
    }
};
} // namespace opennova::world
