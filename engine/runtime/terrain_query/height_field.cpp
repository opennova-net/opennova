#include <runtime/terrain_query/height_field.h>

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
	// The WAC descriptor reads the normal table at the entity's grid cell; the
	// table itself is generated by this centered, quadrant-lock-aware kernel.
	// [orig: WacScript_SpawnEffectAtSsnEntity @0x4F23A0 outMillis/
	// off_849934 lookup; Terrain_GenerateNormalMap @0x603210]
	return height_field_normal_from_raw16(
			raw_at(taps.x(cell_x - 1), taps.z(cell_z)),
			raw_at(taps.x(cell_x + 1), taps.z(cell_z)),
			raw_at(taps.x(cell_x), taps.z(cell_z - 1)),
			raw_at(taps.x(cell_x), taps.z(cell_z + 1)));
}

} // namespace opennova::terrain
