#include "bake_tiles.h"

#include "tristrip.h"

namespace opennova::trngen {

// [orig: TrnGen.exe build_terrain_thread @ 0x4013A0, its CPT export]
CptTile build_tile_from_mesh(const MeshData &mesh, int size, bool is_single) {
	CptTile tile;
	tile.tile_x = mesh.tile_x;
	tile.tile_y = mesh.tile_y;
	tile.vertex_count = static_cast<uint16_t>(mesh.vertex_data.size());
	tile.tile_size = static_cast<uint16_t>(size);
	tile.is_single = is_single;
	tile.vertex_indices.resize(mesh.vertex_data.size() * 2u);

	for (size_t i = 0; i < mesh.vertex_data.size(); ++i) {
		const uint32_t packed = mesh.vertex_data[i];
		tile.vertex_indices[i * 2u + 0u] = static_cast<uint16_t>(packed & 0xFFFFu);
		tile.vertex_indices[i * 2u + 1u] = static_cast<uint16_t>(packed >> 16);
	}

	for (int lod = 0; lod < 8; ++lod) {
		const auto &entry = mesh.entries[lod * 2];
		auto &target = tile.lods[lod];
		target.indices = entry.index_data;
		target.max_index = entry.max_index;
		target.is_strip = entry.is_strip();
	}

	return tile;
}

// [orig: TrnGen.exe build_terrain_thread @ 0x4013A0, the multi-resolution tile loop; sub_404480 @ 0x404480]
bool cpt_tile_from_tml(const std::vector<uint8_t> &tml, int size, CptTile &out, std::string &error) {
	MeshData mesh;
	if (!MeshData::read_bytes(tml, mesh)) {
		error = "a tile mesh does not read back";
		return false;
	}
	if (!remap_vertex_ordering(mesh, error)) return false;
	MeshData tms;
	if (!MeshData::read_bytes(mesh.write_bytes(), tms)) {
		error = "a strip tile mesh does not read back";
		return false;
	}
	out = build_tile_from_mesh(tms, size, false);
	return true;
}

} // namespace opennova::trngen
