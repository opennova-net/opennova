// D-SND-15: the placed-tile surface-override resolvers. Witness maps on the
// declarations (surface_tiles.h); the format access (.til placements, the
// .TSD table parser) stays behind this translation unit.
#include <runtime/terrain_query/surface_tiles.h>

#include <formats/til/til_io.h>
#include <formats/til/til_tsd.h>

namespace opennova::terrain {

void resolve_tileset_surface_table(const SurfaceTileFileSource &files,
		const std::string &tilestrip, uint8_t out_table[256]) {
	TilSurfaceTable table; // zeroed = retail's memset default [orig: @ 0x60c5c9]
	if (files.valid() && !tilestrip.empty()) {
		std::string name = tilestrip;
		const std::size_t dot = name.find_last_of('.');
		if (dot != std::string::npos) name.resize(dot);
		if (!name.empty()) {
			name += ".tsd"; // the retail extension pairing [orig: @ 0x610a1c]
			std::vector<uint8_t> text;
			if (files.has_file(name) && files.read_file(name, text) &&
			    !text.empty())
				til_tsd_parse(reinterpret_cast<const char *>(text.data()),
				              text.size(), table);
		}
	}
	for (int i = 0; i < 256; ++i) out_table[i] = table.map[i];
}

std::vector<SurfaceTileEntry> surface_tiles_from_til_bytes(
		const std::vector<uint8_t> &til_bytes) {
	std::vector<SurfaceTileEntry> tiles;
	if (til_bytes.empty()) return tiles;
	TilFile til;
	std::string til_error;
	if (!load_til(til_bytes.data(), til_bytes.size(), til, til_error))
		return tiles;
	tiles.reserve(til.entries.size());
	for (const TilOverlayEntry &e : til.entries)
		tiles.push_back({e.x_fixed, e.z_fixed, e.tile_index});
	return tiles;
}

} // namespace opennova::terrain
