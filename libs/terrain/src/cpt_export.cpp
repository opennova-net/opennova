#include "cpt_export.h"

#include "terrain/mesh_data.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace opennova {

namespace {

int count_depth_levels(int tile_size, int min_tile_size) {
	int levels = 0;
	int current_size = tile_size;
	while (current_size >= min_tile_size) {
		++levels;
		current_size >>= 1;
	}
	return levels;
}

std::vector<uint16_t> load_depth_buffer(const std::string &path) {
	std::vector<uint16_t> depth_buffer(1024u * 1024u);
	std::ifstream file(path, std::ios::binary);
	if (file.is_open()) {
		file.read(reinterpret_cast<char *>(depth_buffer.data()),
		          static_cast<std::streamsize>(depth_buffer.size() * sizeof(uint16_t)));
	}
	return depth_buffer;
}

void set_header_text(char *buffer, size_t size, const std::string &value) {
	std::strncpy(buffer, value.c_str(), size - 1);
	buffer[size - 1] = '\0';
}

CptTile build_tile_from_mesh(const MeshData &mesh, int current_size, bool is_single_tile) {
	CptTile tile;
	tile.tile_x = mesh.tile_x;
	tile.tile_y = mesh.tile_y;
	tile.vertex_count = static_cast<uint16_t>(mesh.vertex_data.size());
	tile.tile_size = static_cast<uint16_t>(current_size);
	tile.is_single = is_single_tile;
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

} // namespace

void export_terrain_cpt(const std::string &output_prefix,
                        const std::string &tile_ext,
                        const std::string &cpt_path,
                        int tile_size,
                        int min_tile_size,
                        const std::string &terrain_name,
                        const std::string &creator,
                        DepthFormat depth_format,
                        const TilePackProgressCallback &progress_callback) {
	CptFile cpt;
	cpt.depth_format = depth_format;
	set_header_text(cpt.header.terrain_name, sizeof(cpt.header.terrain_name), terrain_name);
	set_header_text(cpt.header.creator, sizeof(cpt.header.creator), creator);
	set_header_text(cpt.header.version, sizeof(cpt.header.version), "taylor");
	cpt.depth_buffer = load_depth_buffer(output_prefix + "Output.dep");

	const int depth_count = count_depth_levels(tile_size, min_tile_size);
	int total_tiles = 0;
	{
		int size = tile_size;
		int multiplier = 1;
		while (size >= min_tile_size) {
			total_tiles += multiplier;
			size >>= 1;
			multiplier *= 4;
		}
	}

	int current_size = tile_size;
	int depth_level = 0;
	int processed_tiles = 0;

	while (current_size >= min_tile_size) {
		for (int y = 0; y < tile_size; y += current_size) {
			for (int x = 0; x < tile_size; x += current_size) {
				MeshData mesh;
				bool is_single_tile = false;
				bool loaded = false;
				char filename[512];

				int file_depth = depth_level;
				if (depth_level == 0) {
					file_depth = depth_count - 1;
				}

				std::snprintf(filename, sizeof(filename), "%sS%d.%s",
				              output_prefix.c_str(), file_depth, tile_ext.c_str());
				try {
					mesh = MeshData::read(filename);
					is_single_tile = true;
					loaded = true;
				} catch (...) {
				}

				if (!loaded) {
					std::snprintf(filename, sizeof(filename), "%sS%d_%02x_%02x.%s",
					              output_prefix.c_str(), file_depth,
					              x >> 4, y >> 4, tile_ext.c_str());
					try {
						mesh = MeshData::read(filename);
						loaded = true;
					} catch (...) {
					}
				}

				++processed_tiles;
				if (progress_callback) {
					char message[256];
					std::snprintf(message, sizeof(message), "Packing %s tiles depth=%d %02x/%02x",
					              tile_ext.c_str(), file_depth, x >> 4, y >> 4);
					progress_callback(processed_tiles, total_tiles, message);
				}

				if (!loaded) {
					continue;
				}

				cpt.tiles.push_back(build_tile_from_mesh(mesh, current_size, is_single_tile));
				if (is_single_tile) {
					goto next_level;
				}
			}
		}

next_level:
		current_size >>= 1;
		++depth_level;
	}

	const uint32_t header_patch =
		(static_cast<uint32_t>(depth_level) << 16u) | static_cast<uint16_t>(tile_size);
	std::memcpy(cpt.header.reserved, &header_patch, sizeof(header_patch));
	cpt.write(cpt_path);
}

} // namespace opennova
