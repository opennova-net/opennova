#include <til/til.h>

namespace opennova {

bool til_blocks_foliage(const TilFile &file,
                         float world_x,
                         float world_z,
                         float radius) {
	// Retail linearly scans g_TerrainTileArray. Each 12-byte entry owns an
	// inclusive 16x16 world AABB; the candidate is an axis-aligned square.
	// [orig: Foliage_PathBlockedByPlacedTile @ 0x606490]
	for (const TilOverlayEntry &entry : file.entries) {
		const float min_x = til_world_x_from_fixed(entry.x_fixed);
		const float min_z = til_world_z_from_fixed(entry.z_fixed);
		const float max_x = min_x + static_cast<float>(TIL_CELL_WORLD_UNITS);
		const float max_z = min_z + static_cast<float>(TIL_CELL_WORLD_UNITS);
		if (min_x <= world_x + radius && min_z <= world_z + radius &&
		    max_x >= world_x - radius && max_z >= world_z - radius) {
			return true;
		}
	}
	return false;
}

} // namespace opennova
