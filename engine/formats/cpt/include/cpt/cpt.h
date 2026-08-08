#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova {

enum class DepthFormat { DPTH, CDEP };

struct CptTileLOD {
	std::vector<uint16_t> indices;
	uint16_t max_index = 0;
	bool is_strip = false;
};

struct CptTile {
	using LODLevel = CptTileLOD;

	uint16_t tile_x = 0;
	uint16_t tile_y = 0;
	uint16_t vertex_count = 0;
	uint16_t tile_size = 0;
	bool is_single = false;
	std::vector<uint16_t> vertex_indices;
	LODLevel lods[8];
};

struct CptFile {
	using TileData = CptTile;

	static constexpr uint32_t MAGIC = 0x31545043; // "CPT1"

	struct Header {
		uint32_t magic = MAGIC;
		uint8_t reserved[12] = {};
		char terrain_name[64] = {};
		char creator[64] = {};
		char version[16] = {};
	};

	Header header{};
	DepthFormat depth_format = DepthFormat::DPTH;
	std::vector<uint16_t> depth_buffer;
	std::vector<TileData> tiles;

	static CptFile read(const std::string &path);
	void write(const std::string &path) const;
};

} // namespace opennova
