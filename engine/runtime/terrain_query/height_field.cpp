#include <runtime/terrain_query/height_field.h>
#include <base/io/bam.h>
#include <runtime/terrain_query/atlas_routing.h>

#include <cstddef>
#include <cmath>

namespace opennova::terrain {

// Bodies lifted verbatim from the three TerrainData::get_height* methods
// (godot/src/terrain/terrain_data.cpp) so behavior stays byte-identical;
// the only change is reading f.heightmap/f.dim/f.layout instead of the class's
// cpt.depth_buffer/trn. The original hardcoded a 1024 atlas side in the two
// world-remap variants; we read f.dim (== 1024 for real terrain), a faithful
// generalization that also lets tests use a smaller square atlas.

float height_field_height_bilinear(const TerrainHeightField &f, float world_x, float world_z) {
	if (!f.valid()) return 0.0f;

	// [orig: gobj_trn_sample_height_bilinear 0x100314A1] direct bilinear, (dim-1) wrap.
	const int hm_size = f.dim;
	const int mask = hm_size - 1;
	const int ix = static_cast<int>(std::floor(world_x));
	const int iz = static_cast<int>(std::floor(world_z));
	const float fx = world_x - static_cast<float>(ix);
	const float fz = world_z - static_cast<float>(iz);
	const int x0 = ix & mask, x1 = (ix + 1) & mask;
	const int z0 = iz & mask, z1 = (iz + 1) & mask;
	const float h00 = static_cast<float>(f.heightmap[z0 * hm_size + x0]);
	const float h10 = static_cast<float>(f.heightmap[z0 * hm_size + x1]);
	const float h01 = static_cast<float>(f.heightmap[z1 * hm_size + x0]);
	const float h11 = static_cast<float>(f.heightmap[z1 * hm_size + x1]);
	const float top = h00 + (h10 - h00) * fx;
	const float bot = h01 + (h11 - h01) * fx;
	return (top + (bot - top) * fz) / 256.0f;
}

float height_field_height_world(const TerrainHeightField &f, float world_x, float world_z) {
	if (!f.valid()) return 0.0f;

	const CoordsResult<float> r =
	        coords_world_to_source<float>(f.layout, world_x, world_z, coords_runtime_options(f.wrap_x, f.wrap_z));
	if (!r.valid) return 0.0f;

	const int hm_size = f.dim;
	const CoordsTaps taps = coords_taps_for_sector(f.locks, r.sector_id, hm_size);
	const int hx = taps.x(static_cast<int>(std::floor(r.source_x)));
	const int hz = taps.z(static_cast<int>(std::floor(r.source_z)));
	return f.heightmap[hz * hm_size + hx] / 256.0f;
}

float height_field_height_mission(const TerrainHeightField &f, float mission_x, float mission_y) {
	return height_field_height_world(f, mission_x, -mission_y);
}

float height_field_height_mission_bilinear(const TerrainHeightField &f, float mission_x,
		float mission_y) {
	return height_field_height_world_bilinear(f, mission_x, -mission_y);
}

float height_field_height_world_bilinear(const TerrainHeightField &f, float world_x, float world_z) {
	if (!f.valid()) return 0.0f;

	const CoordsResult<float> r =
	        coords_world_to_source<float>(f.layout, world_x, world_z, coords_runtime_options(f.wrap_x, f.wrap_z));
	if (!r.valid) return 0.0f;

	const int hm_size = f.dim;
	const int ix = static_cast<int>(std::floor(r.source_x));
	const int iz = static_cast<int>(std::floor(r.source_z));
	const float fx = r.source_x - static_cast<float>(ix);
	const float fz = r.source_z - static_cast<float>(iz);
	// The +1 taps are the ones that reach the quadrant's far edge: on a locked axis
	// they wrap back to its own row/column 0 instead of crossing into the neighbour.
	const CoordsTaps taps = coords_taps_for_sector(f.locks, r.sector_id, hm_size);
	const int x0 = taps.x(ix);
	const int x1 = taps.x(ix + 1);
	const int z0 = taps.z(iz);
	const int z1 = taps.z(iz + 1);
	const float h00 = static_cast<float>(f.heightmap[z0 * hm_size + x0]);
	const float h10 = static_cast<float>(f.heightmap[z0 * hm_size + x1]);
	const float h01 = static_cast<float>(f.heightmap[z1 * hm_size + x0]);
	const float h11 = static_cast<float>(f.heightmap[z1 * hm_size + x1]);
	const float top = h00 + (h10 - h00) * fx;
	const float bot = h01 + (h11 - h01) * fx;
	return (top + (bot - top) * fz) / 256.0f;
}

TerrainSurfaceNormal height_field_normal_from_raw16(
		uint16_t x_minus, uint16_t x_plus,
		uint16_t z_minus, uint16_t z_plus) {
	// Retail differences raw16 neighbours in height units (1/256) as two
	// one-sided vectors, each with a unit up component, and adds them before
	// normalizing: the sum is the centred difference on both axes over an up
	// of 2 (D-TERRAIN-19). The first vector is spilled to the stack, its up the
	// 1.0 stored from the fld1; the second keeps the fld1 itself on the x87
	// stack, and the two ups add in `fadd st, st(5)`.
	// [orig: Terrain_GenerateNormalMap @0x603210; diff scale @0x7C6950;
	// fld1 @0x603248 stored @0x60324C; first vector @0x603381..0x6033CC;
	// second @0x6033EE..0x603419; sum @0x60341B..0x603425; normalize
	// @0x603427..0x60344E]
	constexpr double kHeightScale = 1.0 / 256.0;
	const double nx = (static_cast<double>(x_minus) -
	                   static_cast<double>(x_plus)) * kHeightScale;
	const double nz = (static_cast<double>(z_minus) -
	                   static_cast<double>(z_plus)) * kHeightScale;
	constexpr double up = 2.0;
	const double inverse_length =
			1.0 / std::sqrt(nx * nx + nz * nz + up * up);
	return TerrainSurfaceNormal{
			nx * inverse_length, nz * inverse_length, up * inverse_length};
}

TerrainSurfaceNormal height_field_surface_normal_world(
		const TerrainHeightField &f, float world_x, float world_z) {
	if (!f.valid()) return {};
	const CoordsResult<float> r =
			coords_world_to_source<float>(
					f.layout, world_x, world_z, coords_runtime_options(f.wrap_x, f.wrap_z));
	if (!r.valid) return {};

	const int cell_x = static_cast<int>(std::floor(r.source_x));
	const int cell_z = static_cast<int>(std::floor(r.source_z));
	const CoordsTaps taps =
			coords_taps_for_sector(f.locks, r.sector_id, f.dim);
	const auto raw_at = [&](int x, int z) {
		return f.heightmap[static_cast<std::size_t>(z) *
		                   static_cast<std::size_t>(f.dim) +
		                   static_cast<std::size_t>(x)];
	};
	// The table is generated by this centered, quadrant-lock-aware kernel.
	// [orig: Terrain_GenerateNormalMap @0x603210; scale @0x7C6950]
	return height_field_normal_from_raw16(
			raw_at(taps.x(cell_x - 1), taps.z(cell_z)),
			raw_at(taps.x(cell_x + 1), taps.z(cell_z)),
			raw_at(taps.x(cell_x), taps.z(cell_z - 1)),
			raw_at(taps.x(cell_x), taps.z(cell_z + 1)));
}


int32_t height_field_height_fixed(const TerrainHeightField &f, int32_t mission_x, int32_t mission_y) {
	if (!f.valid()) return 0;
	const TerrainAtlasTexel texel = terrain_atlas_texel_at(
			TerrainAtlasRouting{f.layout.sector_grid, f.layout.origin_x, f.layout.origin_y, f.wrap_x, f.wrap_z},
			mission_x, mission_y);
	// A test atlas narrower than retail's 1024 has no texel past its side.
	if (!texel.mapped || texel.x >= f.dim || texel.z >= f.dim) return 0;
	return static_cast<int32_t>(f.heightmap[static_cast<std::size_t>(texel.z) * f.dim + texel.x]) << 8;
}

int32_t height_field_height_bilinear_fixed(const TerrainHeightField &f, int32_t mission_x,
		int32_t mission_y) {
	if (!f.valid()) return 0;
	const TerrainAtlasTexel texel = terrain_atlas_texel_at(
			TerrainAtlasRouting{f.layout.sector_grid, f.layout.origin_x, f.layout.origin_y, f.wrap_x, f.wrap_z},
			mission_x, mission_y);
	if (!texel.mapped) return 0;
	const CoordsTaps taps = coords_taps_for_quadrant(f.locks, (texel.slot & 2) != 0 ? 512 : 0,
			(texel.slot & 1) != 0 ? 512 : 0, f.dim);
	const auto raw = [&](int tx, int tz) -> int64_t {
		const int x = taps.x(tx), z = taps.z(tz);
		// A test atlas narrower than retail's 1024 has no texel past its side.
		if (x >= f.dim || z >= f.dim) return 0;
		return static_cast<int64_t>(f.heightmap[static_cast<std::size_t>(z) * f.dim + x]) << 8;
	};
	const uint64_t fx = static_cast<uint16_t>(mission_x);
	const uint64_t fy = static_cast<uint16_t>(io::bam_sub(0, mission_y));
	const auto weight = [](uint64_t a, uint64_t b) {
		return static_cast<int64_t>((a * b + 0x8000u) >> 16);
	};
	const auto term = [](int64_t w, int64_t h) {
		return static_cast<int32_t>(static_cast<uint64_t>(w * h + 0x8000) >> 16);
	};
	const int x = texel.x, z = texel.z;
	return term(weight(0x10000 - fx, 0x10000 - fy), raw(x, z)) +
	       term(weight(fx, 0x10000 - fy), raw(x + 1, z)) +
	       term(weight(0x10000 - fx, fy), raw(x, z + 1)) +
	       term(weight(fx, fy), raw(x + 1, z + 1));
}

TerrainHeightGradient height_field_gradient_fixed(
		const TerrainHeightField &f, int32_t mission_x, int32_t mission_y) {
	if (!f.valid() || !f.layout.sector_grid) return {};
	// The routing every lookup shares (atlas_routing.h): retail's NEG/SAR before
	// the sector lookup, including negative fractions and INT32_MIN; a
	// non-wrapping axis clamps to the padded 16-cell grid edges, NOT the
	// authored sector_count/rows. [orig: Terrain_GetHeightGradient
	// @0x606330..0x606385; mask producer PolyTrn_LoadTerrainConfig @0x60E4B1]
	const TerrainAtlasTexel texel = terrain_atlas_texel_at(
			TerrainAtlasRouting{f.layout.sector_grid, f.layout.origin_x, f.layout.origin_y, f.wrap_x, f.wrap_z},
			mission_x, mission_y);
	if (!texel.mapped) return {};
	const int quadrant_x = (texel.slot & 2) != 0 ? 512 : 0;
	const int quadrant_y = (texel.slot & 1) != 0 ? 512 : 0;
	const int x = texel.x;
	const int y = texel.z;
	const CoordsTaps taps = coords_taps_for_quadrant(
			f.locks, quadrant_x, quadrant_y, f.dim);
	const auto raw_at = [&](int tx, int ty) {
		return static_cast<int32_t>(f.heightmap[
				static_cast<std::size_t>(taps.z(ty)) * f.dim + taps.x(tx)]);
	};
	// Unsigned raw16 neighbours, plus-minus, without /256 or /2. The motor
	// applies its own fixed-point gain. [orig: Terrain_GetHeightGradient @ 0x606429..0x606488]
	return {raw_at(x + 1, y) - raw_at(x - 1, y),
	        raw_at(x, y + 1) - raw_at(x, y - 1)};
}

namespace {

// The mip build's 64 x 64 level entry's max byte for tile (tile_x, tile_z): the max over the
// tile's eight by eight finest cells of each cell's byte, (max raw16 + 127) >> 7 kept as a
// byte, over the texel rows and columns 2r..2r+2 of the 1024 atlas (wrapping). A test atlas
// narrower than 1024 reads 0 past its side.
// [orig: Terrain_BuildMipchain @0x6056D0 - the rows (row - 2, row - 1, row) & 0x3FF
//  @0x605740..0x60575D, `add eax, 7Fh; sar eax, 7` and the byte store @0x60589B..0x6058A7; the
//  coarser levels keep their children's max byte]
int32_t mip64_max_byte(const TerrainHeightField &f, int tile_x, int tile_z) {
	uint8_t level_max = 0;
	for (int r = tile_z * 8; r < tile_z * 8 + 8; ++r) {
		for (int c = tile_x * 8; c < tile_x * 8 + 8; ++c) {
			int max_h = -1;
			for (int dz = 0; dz < 3; ++dz) {
				const int z = (2 * r + dz) & 0x3FF;
				for (int dx = 0; dx < 3; ++dx) {
					const int x = (2 * c + dx) & 0x3FF;
					const int h = x < f.dim && z < f.dim
							? f.heightmap[static_cast<std::size_t>(z) * f.dim + x] : 0;
					if (h > max_h) max_h = h;
				}
			}
			const uint8_t cell = static_cast<uint8_t>((max_h + 127) >> 7);
			if (cell > level_max) level_max = cell;
		}
	}
	return level_max;
}

} // namespace

int32_t height_field_mip_max_bilinear_fixed(const TerrainHeightField &f, int32_t mission_x,
		int32_t mission_y) {
	if (!f.valid()) return 0;
	// [orig: Foliage_SampleDensityBilinear @0x606200 - x - 0x100000 @0x60620D, -(y + 0x100000)
	//  @0x606219, the shared grid routing @0x606220..0x606260, the empty cell's 0 @0x606264]
	const int32_t centred_x = io::bam_sub(mission_x, 0x100000);
	const int32_t centred_y = io::bam_add(mission_y, 0x100000);
	const TerrainAtlasTexel texel = terrain_atlas_texel_at(
			TerrainAtlasRouting{f.layout.sector_grid, f.layout.origin_x, f.layout.origin_y, f.wrap_x,
					f.wrap_z},
			centred_x, centred_y);
	if (!texel.mapped) return 0;
	const int32_t centred_z = io::bam_sub(0, centred_y);
	// The weights, the tiles (the quadrant's +32 is the texel's +512 over 16) and their +1
	// neighbours on the 64-tile side [orig: @0x606275 / @0x606280, @0x6062AF..0x6062C8].
	const int32_t frac_x = io::bam_sar(centred_x, 12) & 0xFF;
	const int32_t frac_z = io::bam_sar(centred_z, 12) & 0xFF;
	const int tile_x = texel.x >> 4;
	const int tile_z = texel.z >> 4;
	const int next_x = (tile_x + 1) & 0x3F;
	const int next_z = (tile_z + 1) & 0x3F;
	// The four taps' byte+1 (the max) of the +0x14 level, weighted, summed, halved
	// [orig: @0x606266; g_TerrainMipchainStorage +0x14 @0x6062CB].
	return (frac_z * (256 - frac_x) * mip64_max_byte(f, tile_x, next_z) +
	        (256 - frac_z) * (256 - frac_x) * mip64_max_byte(f, tile_x, tile_z) +
	        frac_x * frac_z * mip64_max_byte(f, next_x, next_z) +
	        frac_x * (256 - frac_z) * mip64_max_byte(f, next_x, tile_z)) >> 1;
}

} // namespace opennova::terrain
