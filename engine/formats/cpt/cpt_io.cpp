#include <formats/cpt/cpt_io.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <base/io/bit_stream.h>
#include <base/io/log.h>
#include <base/io/os_path.h>

// [orig: Terrain_LoadLodStorage @0x603550 — the CDEP/DPTH/POLY/NMAP chunk walk with the CBitStream
//  variable-width tile-index codec; DPTH magic @0x6037b2, CDEP magic @0x60361f]

namespace opennova {

namespace {

// The depth image is a fixed 1024x1024 grid: DPTH stores it raw, and the CDEP
// writer splits exactly that buffer into 4096 blocks (write_depth_section).
static constexpr size_t kDepthPixels = 1024u * 1024u;

static constexpr uint32_t DPTH_MAGIC = 0x48545044; // "DPTH"
static constexpr uint32_t CDEP_MAGIC = 0x50454443; // "CDEP"
static constexpr uint32_t POLY_MAGIC = 0x594C4F50; // "POLY"
static constexpr uint32_t NMAP_MAGIC = 0x50414D4E; // "NMAP"


// The CDEP/POLY bit codec is the shared io::BitReader / io::BitWriter
// (engine/base/io/bit_stream.h).
using io::BitReader;
using io::BitWriter;

int count_bit_width(int value) {
	int result = 0;
	while (value) {
		value >>= 1;
		++result;
	}
	return result;
}

// False, with `error`, for a depth buffer the section cannot carry.
bool write_depth_section(BitWriter &bits,
                         DepthFormat depth_format,
                         const std::vector<uint16_t> &depth_buffer,
                         std::string &error) {
	if (depth_format == DepthFormat::CDEP && !depth_buffer.empty()) {
		if (depth_buffer.size() % 4096u != 0u) {
			error = "CDEP depth buffer must divide evenly into 4096 blocks";
			return false;
		}

		bits.write_bytes("CDEP", 4);
		const int num_blocks = 4096;
		const int block_width = static_cast<int>(depth_buffer.size() / 4096u);
		bits.write_field(16, num_blocks);
		bits.write_field(16, block_width);

		int clamped_blocks = 0;
		for (int block = 0; block < num_blocks; ++block) {
			const size_t block_start = static_cast<size_t>(block * block_width);

			uint16_t min_value = depth_buffer[block_start];
			uint16_t max_value = depth_buffer[block_start];
			for (int i = 1; i < block_width; ++i) {
				const uint16_t value = depth_buffer[block_start + static_cast<size_t>(i)];
				min_value = std::min(min_value, value);
				max_value = std::max(max_value, value);
			}

			// A block's width is that of its range plus one, as the shipped files carry it: a flat
			// block is written 1 bit wide, never 0, and a range of 2^n - 1 takes n + 1 bits (the retail
			// .cpt corpus re-encodes byte-identical under this rule; the encoder that made it is not in
			// Jointops.exe, whose reader takes any width up to 15 [orig: Terrain_LoadLodStorage @
			// 0x603635..0x6037A8, the 4-bit width]). So a block holds a range of at most 32766.
			int range = static_cast<int>(max_value) - static_cast<int>(min_value);
			const bool needs_clamp = range > kCdepMaxRange;
			if (needs_clamp) {
				range = kCdepMaxRange;
				++clamped_blocks;
			}

			const int width = count_bit_width(range + 1);
			if (width > 15) {
				error = "CDEP block range exceeds 15-bit delta limit";
				return false;
			}

			bits.write_field(4, static_cast<uint32_t>(width));
			bits.write_field(16, min_value);
			bits.set_bit_width(width);
			for (int i = 0; i < block_width; ++i) {
				int delta = static_cast<int>(depth_buffer[block_start + static_cast<size_t>(i)]) -
				            static_cast<int>(min_value);
				if (needs_clamp && delta > kCdepMaxRange) {
					delta = kCdepMaxRange;
				}
				bits.write_bits(static_cast<uint32_t>(delta));
			}
		}

		if (clamped_blocks > 0) {
			opennova::io::logf(opennova::io::LogLevel::kWarn,
		"CDEP: clamped %d block(s) to 15-bit delta limit", clamped_blocks);
		}
		return true;
	}

	bits.write_bytes("DPTH", 4);
	if (!depth_buffer.empty()) {
		bits.write_bytes(depth_buffer.data(), depth_buffer.size() * sizeof(uint16_t));
	}
	return true;
}

// The NMAP section, for a file whose model holds a normal map: its id on the next dword, then the map's bytes as
// stored, which the game's walk reads there [orig: Terrain_LoadLodStorage @ 0x6037BD..0x6037F6]. False, with
// `error`, for a map of another size.
bool write_normal_map_section(BitWriter &bits, const std::vector<uint8_t> &normal_map, std::string &error) {
	if (normal_map.empty()) return true;
	if (normal_map.size() != kCptNormalMapBytes) {
		error = "NMAP normal map must be 1024 x 1024 A8R8G8B8 (4 MiB)";
		return false;
	}
	bits.align_dword();
	bits.write_bytes("NMAP", 4);
	bits.write_bytes(normal_map.data(), normal_map.size());
	return true;
}

// False, with `error`, for a tile the section cannot carry.
bool write_poly_section(BitWriter &bits, const std::vector<CptTile> &tiles, std::string &error) {
	bits.align_dword();
	bits.write_bytes("POLY", 4);

	for (const CptTile &tile : tiles) {
		const int bit_width = count_bit_width(static_cast<int>(tile.tile_size));
		if (bit_width <= 0) {
			error = "CPT tile_size must be positive";
			return false;
		}
		if (tile.vertex_indices.size() < static_cast<size_t>(tile.vertex_count) * 2u) {
			error = "CPT tile vertex index payload is truncated";
			return false;
		}

		bits.align_dword();
		bits.write_field(10, tile.tile_x);
		bits.write_field(10, tile.tile_y);
		bits.write_field(16, tile.vertex_count);
		bits.write_field(4, static_cast<uint32_t>(bit_width));
		bits.write_field(7, 8);
		bits.write_field(1, tile.is_single ? 1u : 0u);
		bits.write_field(8, 48);

		bits.set_bit_width(bit_width);
		for (uint32_t i = 0; i < static_cast<uint32_t>(tile.vertex_count); ++i) {
			bits.write_bits(tile.vertex_indices[static_cast<size_t>(i * 2u + 0u)]);
			bits.write_bits(tile.vertex_indices[static_cast<size_t>(i * 2u + 1u)]);
		}

		bits.align_dword();
		for (const CptTileLOD &lod : tile.lods) {
			bits.advance_byte();
			const uint32_t total_indices = static_cast<uint32_t>(lod.indices.size());
			bits.write_field(16, total_indices);
			bits.write_field(15, lod.max_index);
			bits.write_field(1, lod.is_strip ? 1u : 0u);

			size_t offset = 0;
			while (offset < lod.indices.size()) {
				const size_t chunk_size = std::min<size_t>(48, lod.indices.size() - offset);
				int min_value = 0x40000000;
				int max_value = 0;
				for (size_t i = 0; i < chunk_size; ++i) {
					const int value = static_cast<int>(lod.indices[offset + i]);
					min_value = std::min(min_value, value);
					max_value = std::max(max_value, value);
				}
				if (min_value > 4095) {
					min_value = 4095;
				}

				const int chunk_width = count_bit_width(max_value - min_value + 1);
				bits.write_field(4, static_cast<uint32_t>(chunk_width));
				bits.write_field(12, static_cast<uint32_t>(min_value));
				bits.set_bit_width(chunk_width);
				for (size_t i = 0; i < chunk_size; ++i) {
					bits.write_bits(static_cast<uint32_t>(lod.indices[offset + i] - min_value));
				}

				offset += chunk_size;
			}
		}
	}
	return true;
}

} // namespace

namespace {

// A CDEP section after its id: its block count and width, then each block's 4-bit width, 16-bit least height and
// its heights as the least plus a delta of the width, from the atlas's first word [orig: Terrain_LoadLodStorage
// @ 0x603635..0x6037A8]. A later CDEP writes over an earlier one from its start. False, with `error`, for a count the
// 1024 x 1024 atlas or the bits left cannot hold (the game writes on past its atlas).
bool read_cdep(BitReader &bits, CptFile &out, std::string &error) {
	const uint32_t num_blocks = bits.read_bits(16);
	const uint32_t block_width = bits.read_bits(16);
	// Both counts come straight off the bitstream: the depth image is 1024 x 1024, and each block costs at least
	// its 4-bit width and 16-bit least height (a width of 0 is a whole block in those 20 bits).
	const uint64_t declared_pixels = static_cast<uint64_t>(num_blocks) * static_cast<uint64_t>(block_width);
	if (declared_pixels > kDepthPixels) {
		error = "CDEP declares more depth pixels than the 1024x1024 image holds";
		return false;
	}
	if (static_cast<uint64_t>(num_blocks) * 20u > bits.remaining_bits()) {
		error = "CDEP declares more blocks than the section's bits can encode";
		return false;
	}
	out.depth_format = DepthFormat::CDEP;
	if (out.depth_buffer.size() < declared_pixels) out.depth_buffer.resize(static_cast<size_t>(declared_pixels));
	size_t pixel_index = 0;
	for (uint32_t block = 0; block < num_blocks; ++block) {
		const uint32_t bits_per_delta = bits.read_bits(4);
		const uint32_t base_value = bits.read_bits(16);
		for (uint32_t px = 0; px < block_width; ++px) {
			const uint32_t delta = bits.read_bits(static_cast<int>(bits_per_delta));
			out.depth_buffer[pixel_index++] = static_cast<uint16_t>(base_value + delta);
		}
	}
	return true;
}

// One LOD list as the draw reads it: from the next whole byte its 16-bit count, 15-bit largest index and strip bit,
// then chunks of `chunk_size` indices (the last the rest), each a 4-bit width and a 12-bit least index, an index the
// least plus a delta of the width [orig: BitStream_ReadTileLODIndices @ 0x602F40; the walk's pass over it
// Terrain_LoadLodStorage @ 0x603A87..0x603BB7]. `out` null walks it only. False, with `error`, for a list whose
// chunks the bits left cannot hold, or a chunk size of 0 under a list of indices, which the game's walk never ends.
bool read_lod_list(BitReader &bits, uint32_t chunk_size, CptTileLOD *out, const std::string &at, std::string &error) {
	bits.advance_byte();
	const uint32_t total = bits.read_bits(16);
	const uint32_t max_index = bits.read_bits(15);
	const bool is_strip = bits.read_bits(1) != 0;
	std::vector<uint16_t> indices;
	if (total > 0) {
		if (chunk_size == 0) {
			error = at + " has a LOD list of " + std::to_string(total) +
			        " indices in chunks of 0, which the game's walk never ends";
			return false;
		}
		if ((static_cast<uint64_t>(total) + chunk_size - 1) / chunk_size * 16u > bits.remaining_bits()) {
			error = at + "'s LOD list declares more indices than the section can encode";
			return false;
		}
		indices.resize(total);
		for (uint32_t done = 0; done < total;) {
			const uint32_t count = std::min(total - done, chunk_size);
			const uint32_t width = bits.read_bits(4);
			const uint32_t least = bits.read_bits(12);
			for (uint32_t i = 0; i < count; ++i)
				indices[done + i] = static_cast<uint16_t>(least + bits.read_bits(static_cast<int>(width)));
			done += count;
		}
	}
	if (out) {
		out->indices = std::move(indices);
		out->max_index = static_cast<uint16_t>(max_index);
		out->is_strip = is_strip;
	}
	return true;
}

// The tiles after the POLY id, level by level: the header's level count at most the five the game configures, a
// level a grid of the root's side over its stride, the stride halved a level; in a level the rows (tile_y) outer and
// the places along them (tile_x) inner, each tile placed by its order, never by the place it stores; an is_single
// tile ends its level [orig: Terrain_LoadLodStorage @ 0x60380E..0x60381C, @ 0x603845..0x603C9A - the stored place
// passed over @ 0x6038C2..0x6038DD, is_single @ 0x603BF0, the stride @ 0x603C77; Terrain_LoadTileBitStreamData
// @ 0x602E25, the tile's index]. A tile's vertices are read at the width it stores, its lists in chunks of the size it
// stores (the draw's @ 0x602EF8); of its lists the walk passes the count it stores and the draw reads eight
// [orig: LODTileCache_UpdateSlots @ 0x602572], so a list past the eighth is never drawn and, under eight, the draw's
// later lists are the bits that follow. False, with `error`, for a section ending before a tile its levels hold.
bool read_poly_levels(BitReader &bits, CptFile &out, std::string &error) {
	const int levels = std::min(cpt_header_levels(out.header), kCptLevelsMax);
	int stride = kCptGridSide;
	for (int level = 0; level < levels; ++level, stride >>= 1) {
		bool level_done = false;
		for (int y = 0; y < kCptGridSide && !level_done; y += stride)
			for (int x = 0; x < kCptGridSide && !level_done; x += stride) {
				const std::string at = "level " + std::to_string(level) + "'s tile at (" + std::to_string(x) + ", " +
				                       std::to_string(y) + ")";
				bits.align_dword();
				if (bits.at_end()) {
					error = "the POLY section ends before " + at;
					return false;
				}
				bits.read_bits(10); // the place the tile stores, which the walk passes over
				bits.read_bits(10);
				const uint32_t vertex_count = bits.read_bits(16);
				const uint32_t bit_width = bits.read_bits(4);
				const uint32_t lod_count = bits.read_bits(7);
				const uint32_t is_single = bits.read_bits(1);
				const uint32_t chunk_size = bits.read_bits(8);

				CptTile tile;
				tile.tile_x = static_cast<uint16_t>(x);
				tile.tile_y = static_cast<uint16_t>(y);
				tile.tile_size = static_cast<uint16_t>(stride);
				tile.vertex_count = static_cast<uint16_t>(vertex_count);
				tile.is_single = is_single != 0;
				// Each vertex costs 2 * bit_width bits: a count the section cannot encode is a truncated file.
				if (static_cast<uint64_t>(vertex_count) * 2u * bit_width > bits.remaining_bits()) {
					error = "POLY tile declares more vertices than the section can encode";
					return false;
				}
				tile.vertex_indices.resize(static_cast<size_t>(vertex_count) * 2u);
				for (uint16_t &word : tile.vertex_indices)
					word = static_cast<uint16_t>(bits.read_bits(static_cast<int>(bit_width)));

				bits.align_dword();
				for (uint32_t lod = 0; lod < lod_count; ++lod)
					if (!read_lod_list(bits, chunk_size, lod < 8 ? &tile.lods[lod] : nullptr, at, error)) return false;
				if (lod_count < 8) {
					BitReader draw = bits;
					for (uint32_t lod = lod_count; lod < 8; ++lod)
						if (!read_lod_list(draw, chunk_size, &tile.lods[lod], at, error)) return false;
				}
				out.tiles.push_back(std::move(tile));
				if (is_single != 0) level_done = true;
			}
	}
	return true;
}

} // namespace

bool load_cpt(const uint8_t *data, size_t size, CptFile &out, std::string &error) {
	out = CptFile{};

	// The header the walk starts past (its magic is never tested) and a chunk id.
	if (size < 164) {
		error = "CPT data too small";
		return false;
	}
	std::memcpy(&out.header, data, sizeof(CptFile::Header));

	// The chunk walk from byte 160, each id on a dword boundary: CDEP decodes the atlas, NMAP's 4 MiB is the normal
	// map the game then takes in place of generating one, POLY ends the walk, any other id is passed as its dword
	// [orig: Terrain_LoadLodStorage @ 0x6035FA, @ 0x603605..0x603804; NMAP @ 0x6037C2..0x6037F6, the generation
	// skipped @ 0x603CA0..0x603CA8]. DPTH is the raw atlas the original editor reads [orig:
	// JOTACmed.exe MissionDocument_LoadTerrain @ 0x44c64b] and TrnGen writes; the game returns at it with no terrain
	// [orig: Terrain_LoadLodStorage @ 0x6037B7].
	BitReader bits(data + 160, size - 160);
	bool poly = false;
	while (!poly) {
		bits.align_dword();
		uint32_t section = 0;
		if (!bits.read_bytes(&section, sizeof(section))) break; // no POLY: the game reads on past the file
		if (section == CDEP_MAGIC) {
			if (!read_cdep(bits, out, error)) return false;
		} else if (section == DPTH_MAGIC) {
			out.depth_format = DepthFormat::DPTH;
			out.depth_buffer.resize(kDepthPixels);
			if (!bits.read_bytes(out.depth_buffer.data(), kDepthPixels * sizeof(uint16_t))) {
				error = "DPTH section truncated";
				return false;
			}
		} else if (section == NMAP_MAGIC) {
			// A second NMAP reads over the first, as into the game's one buffer. A file ending inside the map
			// holds what it has, the rest zero (the game reads on past the file).
			out.normal_map.assign(kCptNormalMapBytes, 0);
			const size_t held = static_cast<size_t>(
					std::min<uint64_t>(bits.remaining_bits() / 8u, kCptNormalMapBytes));
			bits.read_bytes(out.normal_map.data(), held);
			bits.skip_bytes(kCptNormalMapBytes - held);
		} else if (section == POLY_MAGIC) {
			poly = true;
		}
	}
	return !poly || read_poly_levels(bits, out, error);
}

CptFile CptFile::read(const std::string &path) {
	std::ifstream file(io::os_path(path), std::ios::binary | std::ios::ate); // UTF-8, long-path safe
	if (!file.is_open()) {
		throw std::runtime_error("Failed to open CPT: " + path);
	}

	const auto size = file.tellg();
	file.seekg(0, std::ios::beg);
	if (size < 0) {
		throw std::runtime_error("Failed to stat CPT: " + path);
	}

	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), size)) {
		throw std::runtime_error("Failed to read CPT: " + path);
	}

	CptFile cpt;
	std::string error;
	if (!load_cpt(bytes.data(), bytes.size(), cpt, error)) {
		throw std::runtime_error(error.empty() ? "Failed to parse CPT: " + path : error);
	}
	return cpt;
}

bool save_cpt(const CptFile &cpt, std::vector<uint8_t> &out, std::string &error) {
	BitWriter bits;

	CptFile::Header raw_header = cpt.header;
	raw_header.magic = CptFile::MAGIC;

	bits.write_bytes(&raw_header, sizeof(CptFile::Header));
	bits.align_dword();
	if (!write_depth_section(bits, cpt.depth_format, cpt.depth_buffer, error)) return false;
	if (!write_normal_map_section(bits, cpt.normal_map, error)) return false;
	if (!write_poly_section(bits, cpt.tiles, error)) return false;
	// The file carries four zero bytes past the stream's high-water mark.
	out = bits.bytes(4);
	return true;
}

std::vector<uint8_t> CptFile::write_bytes() const {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!save_cpt(*this, bytes, error)) throw std::runtime_error(error);
	return bytes;
}

void CptFile::write(const std::string &path) const {
	const std::vector<uint8_t> bytes = write_bytes();
	std::ofstream out(io::os_path(path), std::ios::binary); // UTF-8, long-path safe
	if (!out.is_open()) {
		throw std::runtime_error("Failed to create file: " + path);
	}
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void cpt_clamp_steep_blocks(uint16_t *raw16, size_t count) {
	for (size_t start = 0; start + 256 <= count; start += 256) {
		uint16_t *block = raw16 + start;
		const int low = *std::min_element(block, block + 256);
		for (size_t i = 0; i < 256; ++i)
			if (int(block[i]) - low > kCdepMaxRange) block[i] = static_cast<uint16_t>(low + kCdepMaxRange);
	}
}

int cpt_steep_blocks(const std::vector<uint16_t> &raw16) {
	int steep = 0;
	for (size_t start = 0; start + 256 <= raw16.size(); start += 256) {
		const auto [low, high] = std::minmax_element(raw16.begin() + static_cast<std::ptrdiff_t>(start),
		                                             raw16.begin() + static_cast<std::ptrdiff_t>(start + 256));
		if (int(*high) - int(*low) > kCdepMaxRange) ++steep; // a width of range + 1 holds 32,766 in 15 bits
	}
	return steep;
}

} // namespace opennova
