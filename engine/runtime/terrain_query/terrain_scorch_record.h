#pragma once

// The permanent terrain-scorch RECORD and its two retail producers - the
// world -> terrain seam's growth point for scorches (ADR 0020 #5). The world
// side (destruction, wreck landings, explosions) resolves records here; the
// terrain side owns the registry, textures, and page compile behind the seam.
//
// Retail keeps one append-only 4096-record list of {texture index, min x/z,
// max x/z} rows. The standard router selects the texture and extent from the
// scorch id, the sized router takes a caller-authored half extent.
// [orig: Terrain_AddScorchRecord @0x605c90 (the 4096 cap @0x605c9f, the
//  20-byte row @0x605cc7..0x605cef); Terrain_AddScorchForEffectKind @0x6060d0
//  (case 1 @0x606105 = 0x4000, cases 2/7 @0x606130 = 0x40000, case 8
//  @0x60614f = 0xC0000 x 0x140000); Terrain_AddScorchSized @0x606180
//  (ids 1/2/7 @0x6061b8, id 8 @0x6061d3)]

#include <cstddef>
#include <cstdint>

namespace opennova::terrain {

inline constexpr std::size_t kTerrainScorchCapacity = 4096;
inline constexpr std::size_t kTerrainScorchTextureSlots = 5;
inline constexpr int32_t kTerrainScorchSmallHalfExtentQ16 = 0x4000;
inline constexpr int32_t kTerrainScorchLargeHalfExtentQ16 = 0x40000;
inline constexpr int32_t kTerrainScorchBurnHalfWidthQ16 = 0xC0000;
inline constexpr int32_t kTerrainScorchBurnHalfHeightQ16 = 0x140000;

// Slots 0..2 are the three dirt scorches, slot 4 the burn; slot 3 is
// deliberately empty in the retail table (the names live with the terrain
// texture loader behind the seam).
bool terrain_scorch_texture_index_valid(uint8_t texture_index) noexcept;

struct TerrainScorchEntry {
	uint8_t texture_index = 0;
	int32_t minimum_x_q16 = 0;
	int32_t minimum_z_q16 = 0;
	int32_t maximum_x_q16 = 0;
	int32_t maximum_z_q16 = 0;
};

struct TerrainScorchResolved {
	TerrainScorchEntry entry;
	bool valid = false;
};

// The common producer: ids 1/2/7 select trscrch1..3 with CRT rand()%3;
// id 8 selects qburn01 and does not consume a roll. Id 0/unknown is inert.
// The caller supplies the already-consumed 15-bit CRT result so the renderer
// never reaches into an unrelated gameplay PRNG.
TerrainScorchResolved resolve_standard_terrain_scorch(
		int32_t center_x_q16, int32_t center_z_q16,
		int scorch_id, uint16_t crt_roll) noexcept;

// The second retail router accepts one caller-authored half extent. Types
// 1/2/7 select trscrch1..3; type 8 selects qburn01. Both axes use the same
// half extent in this form.
TerrainScorchResolved resolve_sized_terrain_scorch(
		int32_t center_x_q16, int32_t center_z_q16,
		int scorch_id, int32_t half_extent_q16,
		uint16_t crt_roll) noexcept;

} // namespace opennova::terrain
