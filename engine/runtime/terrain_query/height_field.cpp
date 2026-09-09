#include <runtime/terrain_query/height_field.h>
#include <base/io/bam.h>

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
	        coords_world_to_source<float>(f.layout, world_x, world_z, coords_runtime_options());
	if (!r.valid) return 0.0f;

	const int hm_size = f.dim;
	const CoordsTaps taps = coords_taps_for_sector(f.locks, r.sector_id, hm_size);
	const int hx = taps.x(static_cast<int>(std::floor(r.source_x)));
	const int hz = taps.z(static_cast<int>(std::floor(r.source_z)));
	return f.heightmap[hz * hm_size + hx] / 256.0f;
}

float height_field_height_world_bilinear(const TerrainHeightField &f, float world_x, float world_z) {
	if (!f.valid()) return 0.0f;

	const CoordsResult<float> r =
	        coords_world_to_source<float>(f.layout, world_x, world_z, coords_runtime_options());
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
	// Retail differences raw16 neighbours in height units (1/256), keeps a
	// literal unit up component, then normalizes the three doubles.
	// [orig: Terrain_GenerateNormalMap @0x603210; diff scale @0x7C6950;
	// fld1 third component @0x603248]
	constexpr double kHeightScale = 1.0 / 256.0;
	const double nx = (static_cast<double>(x_minus) -
	                   static_cast<double>(x_plus)) * kHeightScale;
	const double nz = (static_cast<double>(z_minus) -
	                   static_cast<double>(z_plus)) * kHeightScale;
	constexpr double up = 1.0;
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
					f.layout, world_x, world_z, coords_runtime_options());
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


TerrainHeightGradient height_field_gradient_fixed(
		const TerrainHeightField &f, int32_t mission_x, int32_t mission_y) {
	if (!f.valid() || !f.layout.sector_grid) return {};
	// Retail NEG/SAR before sector lookup, including negative fractions and
	// INT32_MIN. A non-wrapping axis clamps to the padded 16-cell grid edges,
	// NOT the authored sector_count/rows. [orig: Terrain_GetHeightGradient
	// @0x606330..0x606385; mask producer PolyTrn_LoadTerrainConfig @0x60E4B1]
	const int32_t atlas_y = io::bam_sub(0, mission_y);
	const int32_t grid_x = io::bam_sub(io::bam_sar(mission_x, 25), f.layout.origin_x);
	const int32_t grid_y = io::bam_sub(io::bam_sar(atlas_y, 25), f.layout.origin_y);
	const auto grid_cell = [](int32_t cell, bool wrap) {
		if (!wrap && (cell & ~15) != 0) return cell < 0 ? 0 : 15;
		return cell & 15;
	};
	const int child = io::bam_sub(f.layout.sector_grid[
			16 * grid_cell(grid_y, f.wrap_z) + grid_cell(grid_x, f.wrap_x)], 1);
	if (child < 0) return {};
	const int quadrant_x = (child & 2) != 0 ? 512 : 0;
	const int quadrant_y = (child & 1) != 0 ? 512 : 0;
	const int x = (io::bam_sar(mission_x, 16) & 511) + quadrant_x;
	const int y = (io::bam_sar(atlas_y, 16) & 511) + quadrant_y;
	const CoordsTaps taps = coords_taps_for_quadrant(
			f.locks, quadrant_x, quadrant_y, f.dim);
	const auto raw_at = [&](int tx, int ty) {
		return static_cast<int32_t>(f.heightmap[
				static_cast<std::size_t>(taps.z(ty)) * f.dim + taps.x(tx)]);
	};
	// Unsigned raw16 neighbours, plus-minus, without /256 or /2. The motor
	// applies its own fixed-point gain. [orig: @0x60648F..0x60650C]
	return {raw_at(x + 1, y) - raw_at(x - 1, y),
	        raw_at(x, y + 1) - raw_at(x, y - 1)};
}

} // namespace opennova::terrain
