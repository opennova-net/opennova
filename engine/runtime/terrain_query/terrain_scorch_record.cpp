#include <terrain_query/terrain_scorch_record.h>

#include <limits>

namespace opennova::terrain {
namespace {

bool valid_extents(int32_t half_width, int32_t half_height) noexcept {
	return half_width > 0 && half_height > 0;
}

TerrainScorchResolved make_resolved(uint8_t texture_index,
		int32_t center_x, int32_t center_z,
		int32_t half_width, int32_t half_height) noexcept {
	TerrainScorchResolved result;
	if (!terrain_scorch_texture_index_valid(texture_index) ||
			!valid_extents(half_width, half_height)) {
		return result;
	}
	const int64_t minimum_x = static_cast<int64_t>(center_x) - half_width;
	const int64_t minimum_z = static_cast<int64_t>(center_z) - half_height;
	const int64_t maximum_x = static_cast<int64_t>(center_x) + half_width;
	const int64_t maximum_z = static_cast<int64_t>(center_z) + half_height;
	if (minimum_x < std::numeric_limits<int32_t>::min() ||
			minimum_z < std::numeric_limits<int32_t>::min() ||
			maximum_x > std::numeric_limits<int32_t>::max() ||
			maximum_z > std::numeric_limits<int32_t>::max()) {
		return result;
	}
	result.entry = {texture_index,
			static_cast<int32_t>(minimum_x),
			static_cast<int32_t>(minimum_z),
			static_cast<int32_t>(maximum_x),
			static_cast<int32_t>(maximum_z)};
	result.valid = true;
	return result;
}

} // namespace

bool terrain_scorch_texture_index_valid(uint8_t texture_index) noexcept {
	return texture_index < kTerrainScorchTextureSlots && texture_index != 3;
}

// The standard router: ids 1/2/7 roll one of the three dirt textures with the
// CRT stream, id 8 is the fixed burn texture with the 12x20 unit extent
// [orig: scorch router @ 0x6060d0 (IDB label Render_RestoreDeviceState is
// stale) — case 1 @ 0x606105 (0x4000), cases 2/7 @ 0x606130 (0x40000),
// case 8 @ 0x60614f (0xC0000 x 0x140000)].
TerrainScorchResolved resolve_standard_terrain_scorch(
		int32_t center_x_q16, int32_t center_z_q16,
		int scorch_id, uint16_t crt_roll) noexcept {
	switch (scorch_id) {
		case 1:
			return make_resolved(static_cast<uint8_t>(crt_roll % 3u),
					center_x_q16, center_z_q16,
					kTerrainScorchSmallHalfExtentQ16,
					kTerrainScorchSmallHalfExtentQ16);
		case 2:
		case 7:
			return make_resolved(static_cast<uint8_t>(crt_roll % 3u),
					center_x_q16, center_z_q16,
					kTerrainScorchLargeHalfExtentQ16,
					kTerrainScorchLargeHalfExtentQ16);
		case 8:
			return make_resolved(4, center_x_q16, center_z_q16,
					kTerrainScorchBurnHalfWidthQ16,
					kTerrainScorchBurnHalfHeightQ16);
		default:
			return {};
	}
}

// The sized router: the caller supplies one half extent for both axes
// [orig: sized scorch router @ 0x606180 — ids 1/2/7 @ 0x6061b8, id 8
// @ 0x6061d3].
TerrainScorchResolved resolve_sized_terrain_scorch(
		int32_t center_x_q16, int32_t center_z_q16,
		int scorch_id, int32_t half_extent_q16,
		uint16_t crt_roll) noexcept {
	if (scorch_id == 1 || scorch_id == 2 || scorch_id == 7) {
		return make_resolved(static_cast<uint8_t>(crt_roll % 3u),
				center_x_q16, center_z_q16,
				half_extent_q16, half_extent_q16);
	}
	if (scorch_id == 8) {
		return make_resolved(4, center_x_q16, center_z_q16,
				half_extent_q16, half_extent_q16);
	}
	return {};
}

} // namespace opennova::terrain
