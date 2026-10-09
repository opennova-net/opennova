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
	// The file's bytes (the CDEP/DPTH depth section, then the POLY tiles); write() puts them on disk.
	// Throws std::runtime_error for a depth buffer or a tile the writer cannot encode.
	std::vector<uint8_t> write_bytes() const;
	void write(const std::string &path) const;
};

// The widest range a CDEP block holds: its width, that of the range plus one, fits the 4-bit field's 15 [orig:
// Terrain_LoadLodStorage @ 0x603635..0x6037A8, the 4-bit width]. The writer clamps a block that spans more.
inline constexpr int kCdepMaxRange = 32766;

// The stretches of 256 texels along a row (the CDEP section's blocks over the 1024 x 1024 atlas) whose heights span
// more than a block holds: 32,766 raw (just under 128 world units), its width that of the range plus one within the
// field's 15 bits, as the shipped files write it. CptFile::write_bytes clamps each (kCdepMaxRange).
int cpt_steep_blocks(const std::vector<uint16_t> &raw16);

} // namespace opennova
