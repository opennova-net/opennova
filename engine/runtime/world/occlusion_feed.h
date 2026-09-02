#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace opennova::world {

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
