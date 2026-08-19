#pragma once

// Shared byte identity for terrain lighting. Retail's environment getter
// exposes (g0,g1,g2), while the terrain DOT3 texture basis consumes
// (g2,g0,g1). Static projections use the same quantized epoch for cache
// invalidation, but retain the raw getter tuple for actual projection.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace opennova::terrain {

using TerrainTileLightEpoch = std::array<uint8_t, 3>;
// The straight-up light direction under retail's (d+1)*127.5 truncating byte
// pack: (0,0,1) -> {127,127,255}. [orig: the DOT3 diffuse packing @
// 0x60E231..0x60E331 in PolyTrn_RenderTile]
inline constexpr TerrainTileLightEpoch kDefaultTerrainTileLightEpoch{
		127, 127, 255};

inline TerrainTileLightEpoch terrain_tile_light_epoch_from_environment_tuple(
		float g0, float g1, float g2) noexcept {
	const auto quantize_signed = [](float value) noexcept {
		if (!std::isfinite(value)) return static_cast<uint8_t>(127);
		return static_cast<uint8_t>(std::clamp(
				static_cast<int>((std::clamp(value, -1.0f, 1.0f) + 1.0f) *
						127.5f),
				0, 255));
	};
	return {quantize_signed(g2), quantize_signed(g0),
			quantize_signed(g1)};
}

} // namespace opennova::terrain
