#pragma once

// The clamp-then-narrow the terrain record bindings (TerrainFoliageDef,
// TerrainTileEntry) apply to their byte-ranged authored fields.

#include <algorithm>

namespace godot {

template <typename T>
inline T clamp_int(int value, int min_value, int max_value) {
	return static_cast<T>(std::clamp(value, min_value, max_value));
}

} // namespace godot
