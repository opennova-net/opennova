#include <cpt/cpt.h>
#include <cpt/cpt_io.h>

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

// A 164-byte CPT prefix: valid magic, zeroed header, `section` at offset 160.
std::vector<uint8_t> cpt_prefix(const char *section) {
	std::vector<uint8_t> b(164, 0);
	const uint32_t magic = opennova::CptFile::MAGIC;
	for (int i = 0; i < 4; ++i) {
		b[static_cast<size_t>(i)] = static_cast<uint8_t>(magic >> (8 * i));
	}
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
		tile.push(7, 8u);    // lod_count (the parser requires 8)
		tile.push(1, 0u);    // is_single
		tile.push(8, 48u);   // chunk_size (the parser requires 48)

		std::vector<uint8_t> buf = cpt_prefix("CDEP");
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
	if (!expect(got.tile_x == tile.tile_x && got.tile_y == tile.tile_y,
	            "tile coordinates should round-trip")) return false;
	if (!expect(got.vertex_count == tile.vertex_count, "vertex_count should round-trip")) return false;
	if (!expect(got.tile_size == tile.tile_size, "tile_size should round-trip")) return false;
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

} // namespace

int main() {
	const std::filesystem::path out_path = std::filesystem::temp_directory_path() / "opennova_cpt_roundtrip_test.cpt";

	if (!check_malformed_guards()) return 1;
	if (!check_poly_roundtrip(out_path)) return 1;

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
