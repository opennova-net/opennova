#include <formats/cpt/cpt.h>
#include <formats/cpt/cpt_io.h>

#include "common/test_paths.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}


// LSB-first bit packer matching the CDEP/POLY layout, so a malformed section can
// be built the way the format actually encodes one.
class BitPacker {
public:
	void push(int num_bits, uint32_t value) {
		for (int i = 0; i < num_bits; ++i) {
			if (bit_ == 0) {
				bytes_.push_back(0);
			}
			if ((value >> i) & 1u) {
				bytes_.back() = static_cast<uint8_t>(bytes_.back() | (1u << bit_));
			}
			bit_ = (bit_ + 1) & 7;
		}
	}
	// Zero-fill to `n` bytes and restart on a byte boundary.
	void pad_to(size_t n) {
		while (bytes_.size() < n) {
			bytes_.push_back(0);
		}
		bit_ = 0;
	}
	const std::vector<uint8_t> &bytes() const { return bytes_; }

private:
	std::vector<uint8_t> bytes_;
	int bit_ = 0;
};

// A 164-byte CPT prefix: valid magic, zeroed header but its level count at +6, `section` at offset 160.
std::vector<uint8_t> cpt_prefix(const char *section, uint8_t levels = 0) {
	std::vector<uint8_t> b(164, 0);
	const uint32_t magic = opennova::CptFile::MAGIC;
	for (int i = 0; i < 4; ++i) {
		b[static_cast<size_t>(i)] = static_cast<uint8_t>(magic >> (8 * i));
	}
	b[6] = levels;
	std::memcpy(b.data() + 160, section, 4);
	return b;
}

void append(std::vector<uint8_t> &dst, const std::vector<uint8_t> &src) {
	dst.insert(dst.end(), src.begin(), src.end());
}

// A .cpt is untrusted input — missions ship them and the editor opens whatever
// it is pointed at — so every count read off the bitstream has to be checked
// against the bytes that are actually there before it is believed.
bool check_malformed_guards() {
	std::string error;
	opennova::CptFile out;

	// 1. CDEP declaring 0xffff blocks of 0xffff pixels. The product was computed
	//    in 32 bits and handed straight to resize(): ~8 GB demanded from a
	//    172-byte file, before a single delta was read.
	{
		BitPacker bits;
		bits.push(16, 0xFFFFu);
		bits.push(16, 0xFFFFu);
		std::vector<uint8_t> buf = cpt_prefix("CDEP");
		append(buf, bits.bytes());
		error.clear();
		if (!expect(!opennova::load_cpt(buf.data(), buf.size(), out, error),
		            "CDEP declaring 0xffff*0xffff pixels must be rejected")) return false;
		if (!expect(!error.empty(), "the oversized CDEP must report a reason")) return false;
		std::printf("  rejected oversized CDEP: %s\n", error.c_str());
	}

	// 2. CDEP whose pixel count is legal (4096 x 256 = the real image) but whose
	//    section holds nowhere near the bits those blocks would need.
	{
		BitPacker bits;
		bits.push(16, 4096u);
		bits.push(16, 256u);
		bits.pad_to(16);
		std::vector<uint8_t> buf = cpt_prefix("CDEP");
		append(buf, bits.bytes());
		error.clear();
		if (!expect(!opennova::load_cpt(buf.data(), buf.size(), out, error),
		            "a truncated CDEP must be rejected even at a legal pixel count")) return false;
		std::printf("  rejected truncated CDEP: %s\n", error.c_str());
	}

	// 3. A truncated POLY tile claiming 0xffff vertices. Nothing bounded the
	//    count against the section, so the parser fabricated 65535 zero vertices
	//    out of bytes that were not there.
	{
		BitPacker depth;
		depth.push(16, 1u); // num_blocks
		depth.push(16, 1u); // block_width
		depth.push(4, 0u);  // bits_per_delta — 0 is legal and costs nothing
		depth.push(16, 0u); // base value
		depth.pad_to(12);   // keep POLY on the 4-byte grid the section scan walks

		BitPacker tile;
		tile.push(10, 0u);   // tile_x
		tile.push(10, 0u);   // tile_y
		tile.push(16, 0xFFFFu); // vertex_count
		tile.push(4, 8u);    // bit_width
		tile.push(7, 8u);    // lod_count
		tile.push(1, 0u);    // is_single
		tile.push(8, 48u);   // chunk_size

		std::vector<uint8_t> buf = cpt_prefix("CDEP", 1); // one level: the root's tile
		append(buf, depth.bytes());
		const char poly[4] = {'P', 'O', 'L', 'Y'};
		buf.insert(buf.end(), poly, poly + 4);
		append(buf, tile.bytes());
		error.clear();
		if (!expect(!opennova::load_cpt(buf.data(), buf.size(), out, error),
		            "a POLY tile declaring more vertices than the section holds must be rejected"))
			return false;
		std::printf("  rejected truncated POLY: %s\n", error.c_str());
	}

	return true;
}

// POLY round-trip. The depth round-trips below go through write_bytes; the tile
// section is what drives BitWriter::write_bits across every bit offset, so this
// is the check that pins the byte-safe little-endian store as equivalent.
bool check_poly_roundtrip(const std::filesystem::path &out_path) {
	opennova::CptFile saved;
	// A real .cpt always carries the full depth image; the reader requires it
	// before it reaches the tile section.
	saved.depth_format = opennova::DepthFormat::DPTH;
	saved.depth_buffer.assign(1024u * 1024u, 0);
	saved.header.reserved[2] = 1; // one level, the root's tile

	opennova::CptTile tile;
	tile.tile_x = 12;
	tile.tile_y = 34;
	tile.vertex_count = 7;
	tile.tile_size = 256; // a power of two: bit_width 9 decodes back to 1 << 8
	tile.is_single = true;
	for (uint16_t i = 0; i < tile.vertex_count * 2u; ++i) {
		tile.vertex_indices.push_back(static_cast<uint16_t>(i * 13u + 1u));
	}
	for (int lod = 0; lod < 8; ++lod) {
		auto &l = tile.lods[static_cast<size_t>(lod)];
		l.is_strip = (lod % 2) == 0;
		for (int i = 0; i < 50 + lod; ++i) { // > 48 so the chunker runs twice
			l.indices.push_back(static_cast<uint16_t>((i * 7 + lod) % 4096));
		}
		l.max_index = 4095;
	}
	saved.tiles.push_back(tile);

	try {
		saved.write(out_path.string());
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: CptFile::write with tiles threw: %s\n", e.what());
		return false;
	}

	opennova::CptFile loaded;
	try {
		loaded = opennova::CptFile::read(out_path.string());
	} catch (const std::exception &e) {
		std::filesystem::remove(out_path);
		std::fprintf(stderr, "FAIL: CptFile::read with tiles threw: %s\n", e.what());
		return false;
	}
	std::filesystem::remove(out_path);

	if (!expect(loaded.tiles.size() == 1, "one tile should round-trip")) return false;
	const opennova::CptTile &got = loaded.tiles[0];
	// The walk places the root's tile at (0, 0) the root's side, whatever place it stores [orig:
	// Terrain_LoadLodStorage @ 0x6038C2..0x6038DD]; its vertices read at the width it stores (9 bits).
	if (!expect(got.tile_x == 0 && got.tile_y == 0, "the root's tile stands at (0, 0)")) return false;
	if (!expect(got.vertex_count == tile.vertex_count, "vertex_count should round-trip")) return false;
	if (!expect(got.tile_size == 1024, "the root's tile is the root's side")) return false;
	if (!expect(got.is_single == tile.is_single, "is_single should round-trip")) return false;
	if (!expect(got.vertex_indices.size() >= tile.vertex_indices.size(),
	            "vertex index payload should round-trip")) return false;
	for (size_t i = 0; i < tile.vertex_indices.size(); ++i) {
		if (!expect(got.vertex_indices[i] == tile.vertex_indices[i],
		            "each vertex index should round-trip")) return false;
	}
	for (size_t lod = 0; lod < 8; ++lod) {
		const auto &a = tile.lods[lod];
		const auto &b = got.lods[lod];
		if (!expect(b.indices.size() == a.indices.size(), "LOD index count should round-trip"))
			return false;
		if (!expect(b.is_strip == a.is_strip, "LOD strip flag should round-trip")) return false;
		for (size_t i = 0; i < a.indices.size(); ++i) {
			if (!expect(b.indices[i] == a.indices[i], "each LOD index should round-trip"))
				return false;
		}
	}
	return true;
}

opennova::CptTile walk_tile(uint16_t vertices, uint16_t size, bool single = false) {
	opennova::CptTile tile;
	tile.tile_x = 7; // a place no walk puts a tile: the walk places it by its order
	tile.tile_y = 9;
	tile.tile_size = size;
	tile.vertex_count = vertices;
	tile.is_single = single;
	tile.vertex_indices.assign(static_cast<size_t>(vertices) * 2u, 1);
	return tile;
}

opennova::CptFile walk_file(int levels) {
	opennova::CptFile cpt;
	cpt.depth_format = opennova::DepthFormat::DPTH;
	cpt.depth_buffer.assign(1024u * 1024u, 3);
	cpt.header.reserved[2] = static_cast<uint8_t>(levels);
	return cpt;
}

// One LOD list bit by bit, as the draw reads it: from the next whole byte its count, largest index and strip bit, then
// chunks of `chunk` indices, each a 4-bit width (4 here) and a 12-bit least index.
void push_list(BitPacker &bits, const std::vector<uint16_t> &indices, uint32_t chunk) {
	bits.pad_to(bits.bytes().size());
	bits.push(16, static_cast<uint32_t>(indices.size()));
	bits.push(15, indices.empty() ? 0u : *std::max_element(indices.begin(), indices.end()));
	bits.push(1, 0u);
	if (chunk == 0) return; // no chunk the reader reaches: it refuses the size
	for (size_t at = 0; at < indices.size(); at += chunk) {
		const size_t count = std::min<size_t>(chunk, indices.size() - at);
		const uint16_t least = *std::min_element(indices.begin() + long(at), indices.begin() + long(at + count));
		bits.push(4, 4u);
		bits.push(12, least);
		for (size_t i = 0; i < count; ++i) bits.push(4, static_cast<uint32_t>(indices[at + i] - least));
	}
}

// A one-level file of one tile built bit by bit: no vertices, `lod_count` lists the first holding 0..24, chunks of
// `chunk`, and `after` (a list the tile does not count) past its lists.
std::vector<uint8_t> one_tile_file(uint32_t lod_count, uint32_t chunk, const std::vector<uint16_t> &after) {
	BitPacker depth;
	depth.push(16, 0u); // a CDEP of no blocks
	depth.push(16, 0u);
	std::vector<uint8_t> buf = cpt_prefix("CDEP", 1);
	append(buf, depth.bytes());
	const char poly[4] = {'P', 'O', 'L', 'Y'};
	buf.insert(buf.end(), poly, poly + 4);
	BitPacker tile;
	tile.push(10, 0u);
	tile.push(10, 0u);
	tile.push(16, 0u); // vertex_count
	tile.push(4, 1u);  // bit_width
	tile.push(7, lod_count);
	tile.push(1, 0u);  // is_single
	tile.push(8, chunk);
	tile.pad_to(8); // the lists from the next dword
	std::vector<uint16_t> first;
	for (uint16_t i = 0; i < 25; ++i) first.push_back(i);
	push_list(tile, first, chunk);
	for (uint32_t lod = 1; lod < lod_count; ++lod) push_list(tile, {}, chunk);
	if (!after.empty()) push_list(tile, after, chunk);
	append(buf, tile.bytes());
	return buf;
}

// The game's walk [orig: Terrain_LoadLodStorage @ 0x603550]: no magic test; the chunk ids on dword boundaries, an
// unknown one passed as its dword and NMAP's 4 MiB normal map passed over whole (@ 0x603605..0x603804); the header's
// level count at +6, at most five (@ 0x60380E..0x60381C); each level a grid walked rows outer, its tiles placed by
// their order, an is_single tile ending its level (@ 0x603BF0), a tile of no vertices kept; the chunk size each tile
// stores, and of its lists the eight the draw reads [orig: LODTileCache_UpdateSlots @ 0x602572].
bool check_game_walk() {
	std::string error;

	// Two levels, the second's four tiles placed by their order; no magic, a JUNK id and an NMAP of POLY ids before the
	// POLY section.
	opennova::CptFile two = walk_file(2);
	two.tiles.push_back(walk_tile(1, 1024));
	for (uint16_t i = 0; i < 4; ++i) two.tiles.push_back(walk_tile(static_cast<uint16_t>(2 + i), 512));
	std::vector<uint8_t> bytes;
	if (!expect(opennova::save_cpt(two, bytes, error), "the two-level file writes")) return false;
	std::memcpy(bytes.data(), "XXXX", 4);
	const size_t poly = 164 + 1024u * 1024u * 2u;
	if (!expect(std::memcmp(bytes.data() + poly, "POLY", 4) == 0, "the writer's POLY follows its DPTH")) return false;
	std::vector<uint8_t> extra = {'J', 'U', 'N', 'K', 'N', 'M', 'A', 'P'};
	for (size_t i = 0; i < 0x400000u / 4u; ++i) extra.insert(extra.end(), {'P', 'O', 'L', 'Y'});
	bytes.insert(bytes.begin() + long(poly), extra.begin(), extra.end());
	opennova::CptFile read;
	if (!expect(opennova::load_cpt(bytes.data(), bytes.size(), read, error) && read.tiles.size() == 5,
	            "a file of another magic, an unknown id and an NMAP reads its five tiles"))
		return false;
	const uint16_t places[4][2] = {{0, 0}, {512, 0}, {0, 512}, {512, 512}};
	bool placed = read.tiles[0].tile_x == 0 && read.tiles[0].tile_y == 0 && read.tiles[0].tile_size == 1024;
	for (int i = 0; i < 4; ++i) {
		const opennova::CptTile &t = read.tiles[size_t(1 + i)];
		placed = placed && t.tile_x == places[i][0] && t.tile_y == places[i][1] && t.tile_size == 512 &&
		         t.vertex_count == 2 + i;
	}
	if (!expect(placed, "each tile stands where its order puts it, rows outer")) return false;
	bytes[6] = 0;
	if (!expect(opennova::load_cpt(bytes.data(), bytes.size(), read, error) && read.tiles.empty(),
	            "a level count of 0 reads no tile"))
		return false;
	bytes[6] = 3;
	if (!expect(!opennova::load_cpt(bytes.data(), bytes.size(), read, error) && error.find("level 2") != std::string::npos,
	            "a third level the section does not hold is refused"))
		return false;

	// An is_single tile ends its level, not the read.
	opennova::CptFile single = walk_file(3);
	single.tiles.push_back(walk_tile(1, 1024));
	single.tiles.push_back(walk_tile(1, 512, true));
	for (int i = 0; i < 15; ++i) single.tiles.push_back(walk_tile(1, 256));
	single.tiles.push_back(walk_tile(5, 256));
	if (!expect(opennova::save_cpt(single, bytes, error) && opennova::load_cpt(bytes.data(), bytes.size(), read, error) &&
	                    read.tiles.size() == 18 && read.tiles[1].is_single && read.tiles[2].tile_x == 0 &&
	                    read.tiles[2].tile_y == 0 && read.tiles[2].tile_size == 256 && read.tiles[17].tile_x == 768 &&
	                    read.tiles[17].tile_y == 768 && read.tiles[17].vertex_count == 5,
	            "an is_single tile ends its level and the next level reads"))
		return false;

	// Five levels at most, whatever the header says; a tile of no vertices is a tile.
	opennova::CptFile full = walk_file(6);
	for (int size = 1024; size >= 64; size /= 2)
		for (int n = 0; n < (1024 / size) * (1024 / size); ++n)
			full.tiles.push_back(walk_tile(0, static_cast<uint16_t>(size)));
	full.tiles.push_back(walk_tile(3, 64)); // past the five levels
	if (!expect(opennova::save_cpt(full, bytes, error) && opennova::load_cpt(bytes.data(), bytes.size(), read, error) &&
	                    read.tiles.size() == 341 && read.tiles.back().tile_x == 960 && read.tiles.back().tile_y == 960 &&
	                    read.tiles.back().tile_size == 64 && read.tiles.back().vertex_count == 0,
	            "a level count of 6 reads the five levels' 341 tiles, each of no vertices"))
		return false;

	// The chunk size the tile stores; nine lists, the draw's eight kept.
	bytes = one_tile_file(9, 10, {});
	if (!expect(opennova::load_cpt(bytes.data(), bytes.size(), read, error) && read.tiles.size() == 1 &&
	                    read.tiles[0].lods[0].indices.size() == 25 && read.tiles[0].lods[0].indices[24] == 24 &&
	                    read.tiles[0].lods[0].indices[10] == 10 && read.tiles[0].lods[7].indices.empty(),
	            "chunks of 10 read 0..24; a ninth list walked"))
		return false;
	// Seven lists: the draw's eighth is the list that follows.
	bytes = one_tile_file(7, 10, {5, 6, 7});
	if (!expect(opennova::load_cpt(bytes.data(), bytes.size(), read, error) && read.tiles.size() == 1 &&
	                    read.tiles[0].lods[7].indices == std::vector<uint16_t>({5, 6, 7}),
	            "a tile of seven lists draws the following list as its eighth"))
		return false;
	// A chunk size of 0 under a list of indices never ends the game's walk.
	bytes = one_tile_file(8, 0, {});
	if (!expect(!opennova::load_cpt(bytes.data(), bytes.size(), read, error) && error.find("chunks of 0") != std::string::npos,
	            "a chunk size of 0 under indices is refused"))
		return false;
	return true;
}

} // namespace

// The CDEP blocks the writer clamps (cpt_steep_blocks): a 256-texel stretch whose heights span more than
// kCdepMaxRange [orig: Terrain_LoadLodStorage @ 0x603635..0x6037A8, the 4-bit width].
bool check_steep_blocks() {
	std::vector<uint16_t> steep(1024, 0);
	steep[300] = 40000;
	if (!expect(opennova::cpt_steep_blocks(steep) == 1, "one stretch spans more than a block holds")) return false;
	steep[300] = static_cast<uint16_t>(opennova::kCdepMaxRange);
	if (!expect(opennova::cpt_steep_blocks(steep) == 0, "a span of kCdepMaxRange fits")) return false;
	steep[300] = static_cast<uint16_t>(opennova::kCdepMaxRange + 1);
	steep[700] = 65535;
	if (!expect(opennova::cpt_steep_blocks(steep) == 2, "each stretch counts once")) return false;
	if (!expect(opennova::cpt_steep_blocks(std::vector<uint16_t>(255, 0)) == 0, "a short tail is no block")) return false;
	return true;
}

// cpt_clamp_steep_blocks is what a CDEP write reads back: a steep block clamped over its lowest, the others as
// they stand (the heights an editor's device and ground take where the written .cpt clamps).
bool check_clamp_steep_blocks() {
	opennova::CptFile cpt;
	cpt.depth_format = opennova::DepthFormat::CDEP;
	cpt.depth_buffer.assign(1024u * 1024u, 1000);
	cpt.depth_buffer[300] = 40000;  // a block past the range (its lowest 1000)
	cpt.depth_buffer[301] = 33766;  // within it (1000 + 32,766)
	cpt.depth_buffer[700] = 30000;  // a block within the range
	cpt.depth_buffer[5000] = 65535; // a later row's
	cpt.depth_buffer[5001] = 0;
	std::vector<uint8_t> bytes;
	std::string error;
	opennova::CptFile read;
	if (!expect(opennova::save_cpt(cpt, bytes, error) && opennova::load_cpt(bytes.data(), bytes.size(), read, error),
	            "the steep atlas writes and reads"))
		return false;
	std::vector<uint16_t> clamped = cpt.depth_buffer;
	opennova::cpt_clamp_steep_blocks(clamped.data(), clamped.size());
	if (!expect(clamped == read.depth_buffer, "the clamp is what the write reads back")) return false;
	if (!expect(clamped[300] == 1000 + opennova::kCdepMaxRange && clamped[301] == 33766 && clamped[700] == 30000 &&
	                    clamped[5000] == opennova::kCdepMaxRange && clamped[5001] == 0,
	            "a steep block clamped over its lowest, the rest as they stand"))
		return false;
	return true;
}

// save_cpt is write_bytes without the throw: the same bytes for a file it encodes, and false with the
// writer's words, `out` untouched, for one it cannot (a CDEP buffer not 4096 whole blocks, a tile of no size).
bool check_save_cpt() {
	opennova::CptFile cpt;
	cpt.depth_format = opennova::DepthFormat::CDEP;
	cpt.depth_buffer.assign(1024u * 1024u, 0);
	for (size_t i = 0; i < cpt.depth_buffer.size(); ++i) cpt.depth_buffer[i] = static_cast<uint16_t>((i * 37u) % 5000u);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!expect(opennova::save_cpt(cpt, bytes, error) && !bytes.empty() && bytes == cpt.write_bytes(),
	            "save_cpt writes write_bytes' bytes"))
		return false;

	const std::vector<uint8_t> kept = bytes;
	cpt.depth_buffer.resize(4095);
	if (!expect(!opennova::save_cpt(cpt, bytes, error) && error.find("4096") != std::string::npos && bytes == kept,
	            "a CDEP buffer of no whole blocks is refused in words, the output untouched"))
		return false;
	cpt.depth_format = opennova::DepthFormat::DPTH;
	cpt.tiles.emplace_back(); // tile_size 0
	error.clear();
	if (!expect(!opennova::save_cpt(cpt, bytes, error) && error.find("tile_size") != std::string::npos,
	            "a tile of no size is refused in words"))
		return false;
	return true;
}

int main() {
	const std::filesystem::path out_path = std::filesystem::temp_directory_path() / test_paths_unique("opennova_cpt_roundtrip_test", ".cpt");

	if (!check_malformed_guards()) return 1;
	if (!check_steep_blocks()) return 1;
	if (!check_clamp_steep_blocks()) return 1;
	if (!check_save_cpt()) return 1;
	if (!check_poly_roundtrip(out_path)) return 1;
	if (!check_game_walk()) return 1;

	opennova::CptFile saved;
	std::strncpy(saved.header.terrain_name, "RoundtripTerrain", sizeof(saved.header.terrain_name) - 1);
	std::strncpy(saved.header.creator, "tester", sizeof(saved.header.creator) - 1);
	std::strncpy(saved.header.version, "unit", sizeof(saved.header.version) - 1);
	saved.depth_format = opennova::DepthFormat::DPTH;
	saved.depth_buffer.resize(1024u * 1024u);
	for (size_t i = 0; i < saved.depth_buffer.size(); ++i) {
		saved.depth_buffer[i] = static_cast<uint16_t>(i % 1024u);
	}

	try {
		saved.write(out_path.string());
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: CptFile::write threw: %s\n", e.what());
		return 1;
	}

	opennova::CptFile loaded;
	try {
		loaded = opennova::CptFile::read(out_path.string());
	} catch (const std::exception &e) {
		std::filesystem::remove(out_path);
		std::fprintf(stderr, "FAIL: CptFile::read threw: %s\n", e.what());
		return 1;
	}

	std::filesystem::remove(out_path);

	if (!expect(loaded.depth_format == opennova::DepthFormat::DPTH, "depth_format should round-trip")) return 1;
	if (!expect(loaded.depth_buffer.size() == saved.depth_buffer.size(), "depth buffer size should round-trip")) return 1;
	if (!expect(std::strcmp(loaded.header.terrain_name, saved.header.terrain_name) == 0, "terrain_name should round-trip")) return 1;
	if (!expect(std::strcmp(loaded.header.creator, saved.header.creator) == 0, "creator should round-trip")) return 1;
	if (!expect(loaded.depth_buffer[0] == saved.depth_buffer[0] &&
	                loaded.depth_buffer[511] == saved.depth_buffer[511] &&
	                loaded.depth_buffer[900000] == saved.depth_buffer[900000],
	            "depth samples should round-trip")) return 1;

	std::printf("OK: cpt round-trip preserved DPTH depth + POLY tiles; malformed declarations rejected\n");
	return 0;
}
