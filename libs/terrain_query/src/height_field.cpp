#include "terrain/height_field.h"

#include <cmath>

namespace opennova::terrain {

// Bodies lifted verbatim from the three NovaTerrainData::get_height* methods
// (godot/engine/terrain/nova_terrain_data.cpp) so behavior stays byte-identical;
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

} // namespace opennova::terrain
