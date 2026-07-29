#include "cpt/cpt_io.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <io/le.h>
#include <io/log.h>

namespace opennova {

namespace {

// The depth image is a fixed 1024x1024 grid: DPTH stores it raw, and the CDEP
// writer splits exactly that buffer into 4096 blocks (write_depth_section).
static constexpr size_t kDepthPixels = 1024u * 1024u;

static constexpr uint32_t DPTH_MAGIC = 0x48545044; // "DPTH"
static constexpr uint32_t CDEP_MAGIC = 0x50454443; // "CDEP"
static constexpr uint32_t POLY_MAGIC = 0x594C4F50; // "POLY"

class BitReader {
public:
	BitReader(const uint8_t *data, size_t size) : data_(data), size_(size) {}

	uint32_t read_bits(int num_bits) {
		if (num_bits <= 0) {
			return 0;
		}
		if (byte_pos_ + 4 <= size_) {
			uint32_t value = 0;
			std::memcpy(&value, data_ + byte_pos_, sizeof(uint32_t));
			const uint32_t mask = (num_bits < 32) ? ((1u << num_bits) - 1u) : 0xFFFFFFFFu;
			const uint32_t result = (value >> bit_pos_) & mask;
			const uint32_t total = bit_pos_ + static_cast<uint32_t>(num_bits);
			byte_pos_ += total >> 3;
			bit_pos_ = static_cast<int>(total & 7u);
			return result;
		}

		uint32_t result = 0;
		int shift = 0;
		int remaining = num_bits;
		while (remaining > 0 && byte_pos_ < size_) {
			const int available = 8 - bit_pos_;
			const int take = std::min(remaining, available);
			const uint32_t mask = (1u << take) - 1u;
			result |= ((data_[byte_pos_] >> bit_pos_) & mask) << shift;
			shift += take;
			remaining -= take;
			byte_pos_ += static_cast<size_t>((bit_pos_ + take) / 8);
			bit_pos_ = (bit_pos_ + take) % 8;
		}
		return result;
	}

	void advance_byte() {
		if (bit_pos_) {
			bit_pos_ = 0;
			++byte_pos_;
		}
	}

	void align_dword() {
		if (bit_pos_) {
			bit_pos_ = 0;
			++byte_pos_;
		}
		if (byte_pos_ & 3u) {
			byte_pos_ = (byte_pos_ + 3u) & ~static_cast<size_t>(3);
		}
	}

	bool at_end() const { return byte_pos_ >= size_; }

	// Bits still readable from the cursor. Lets a decoder reject a declared
	// count the section cannot possibly encode BEFORE it allocates for it.
	uint64_t remaining_bits() const {
		if (byte_pos_ >= size_) {
			return 0;
		}
		return (static_cast<uint64_t>(size_ - byte_pos_) * 8u) -
		       static_cast<uint64_t>(bit_pos_);
	}

private:
	const uint8_t *data_;
	size_t size_;
	size_t byte_pos_ = 0;
	int bit_pos_ = 0;
};

class BitWriter {
public:
	explicit BitWriter(size_t initial_capacity = 0x4000) : buffer_(initial_capacity, 0) {}

	void set_bit_width(int num_bits) {
		bit_width_ = static_cast<uint32_t>(num_bits);
		bitmask_ = (num_bits >= 32) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);
	}

	void set_position(uint32_t byte_offset, uint32_t bit_offset) {
		if (bit_offset <= 8) {
			byte_pos_ = byte_offset;
			bit_pos_ = bit_offset;
		} else {
			byte_pos_ = byte_offset + (bit_offset >> 3);
			bit_pos_ = bit_offset & 7u;
		}
	}

	void advance_byte() {
		if (bit_pos_) {
			bit_pos_ = 0;
			++byte_pos_;
		}
	}

	void align_dword() {
		if (bit_pos_) {
			bit_pos_ = 0;
			++byte_pos_;
		}
		if (byte_pos_ & 3u) {
			byte_pos_ = (byte_pos_ + 3u) & ~3u;
		}
	}

	void write_bytes(const void *data, size_t size) {
		ensure_capacity(byte_pos_ + static_cast<uint32_t>(size) + 32u);
		if (bit_pos_) {
			bit_pos_ = 0;
			++byte_pos_;
		}
		std::memcpy(buffer_.data() + byte_pos_, data, size);
		byte_pos_ += static_cast<uint32_t>(size);
		if (byte_pos_ > high_water_) {
			high_water_ = byte_pos_;
		}
	}

	void write_bits(uint32_t value) {
		ensure_capacity(byte_pos_ + 32u);
		// Byte-wise little-endian read-modify-write. This was a dword store through a
		// reinterpret_cast at an arbitrary byte offset: strict-aliasing and alignment
		// UB that merely happened to work on x86, and that UBSan flags. io/le.h is
		// also the layout contract — the stream packs LSB-first within a
		// little-endian dword — so being explicit makes the codec correct rather
		// than accidentally correct. Identical bytes on a little-endian host.
		uint8_t *dst = buffer_.data() + byte_pos_;
		const uint32_t cur = opennova::io::read_u32_le(dst);
		const uint32_t mask = bitmask_ << bit_pos_;
		opennova::io::write_u32_le(dst, ((value & bitmask_) << bit_pos_) | (cur & ~mask));

		const uint32_t total_bits = bit_pos_ + bit_width_;
		byte_pos_ += total_bits >> 3;
		bit_pos_ = total_bits & 7u;
		if (byte_pos_ + 1u > high_water_) {
			high_water_ = byte_pos_ + 1u;
		}
	}

	void write_field(int num_bits, uint32_t value) {
		set_bit_width(num_bits);
		write_bits(value);
	}

	void write_to_file(const std::string &path) const {
		std::ofstream out(path, std::ios::binary);
		if (!out.is_open()) {
			throw std::runtime_error("Failed to create file: " + path);
		}
		out.write(reinterpret_cast<const char *>(buffer_.data()), static_cast<std::streamsize>(high_water_ + 4u));
	}

private:
	void ensure_capacity(uint32_t needed) {
		if (buffer_.empty()) {
			buffer_.resize(0x4000, 0);
		}
		while (needed >= buffer_.size()) {
			buffer_.resize(buffer_.size() + 0x4000, 0);
		}
	}

	std::vector<uint8_t> buffer_;
	uint32_t bit_width_ = 0;
	uint32_t byte_pos_ = 0;
	uint32_t bit_pos_ = 0;
	uint32_t bitmask_ = 0;
	uint32_t high_water_ = 0;
};

int count_bit_width(int value) {
	int result = 0;
	while (value) {
		value >>= 1;
		++result;
	}
	return result;
}

void write_depth_section(BitWriter &bits,
                         DepthFormat depth_format,
                         const std::vector<uint16_t> &depth_buffer) {
	if (depth_format == DepthFormat::CDEP && !depth_buffer.empty()) {
		if (depth_buffer.size() % 4096u != 0u) {
			throw std::runtime_error("CDEP depth buffer must divide evenly into 4096 blocks");
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

			int range = static_cast<int>(max_value) - static_cast<int>(min_value);
			const bool needs_clamp = range > 32767;
			if (needs_clamp) {
				range = 32767;
				++clamped_blocks;
			}

			const int width = count_bit_width(range);
			if (width > 15) {
				throw std::runtime_error("CDEP block range exceeds 15-bit delta limit");
			}

			bits.write_field(4, static_cast<uint32_t>(width));
			bits.write_field(16, min_value);
			bits.set_bit_width(width);
			for (int i = 0; i < block_width; ++i) {
				int delta = static_cast<int>(depth_buffer[block_start + static_cast<size_t>(i)]) -
				            static_cast<int>(min_value);
				if (needs_clamp && delta > 32767) {
					delta = 32767;
				}
				bits.write_bits(static_cast<uint32_t>(delta));
			}
		}

		if (clamped_blocks > 0) {
			opennova::io::logf(opennova::io::LogLevel::kWarn,
		"CDEP: clamped %d block(s) to 15-bit delta limit", clamped_blocks);
		}
		return;
	}

	bits.write_bytes("DPTH", 4);
	if (!depth_buffer.empty()) {
		bits.write_bytes(depth_buffer.data(), depth_buffer.size() * sizeof(uint16_t));
	}
}

void write_poly_section(BitWriter &bits, const std::vector<CptTile> &tiles) {
	bits.align_dword();
	bits.write_bytes("POLY", 4);

	for (const CptTile &tile : tiles) {
		const int bit_width = count_bit_width(static_cast<int>(tile.tile_size));
		if (bit_width <= 0) {
			throw std::runtime_error("CPT tile_size must be positive");
		}
		if (tile.vertex_indices.size() < static_cast<size_t>(tile.vertex_count) * 2u) {
			throw std::runtime_error("CPT tile vertex index payload is truncated");
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
}

} // namespace

bool load_cpt(const uint8_t *data, size_t size, CptFile &out, std::string &error) {
	out = CptFile{};

	if (size < 164) {
		error = "CPT data too small";
		return false;
	}

	uint32_t magic = 0;
	std::memcpy(&magic, data, sizeof(uint32_t));
	if (magic != CptFile::MAGIC) {
		error = "Bad CPT magic";
		return false;
	}

	std::memcpy(&out.header, data, sizeof(CptFile::Header));
	out.header.magic = magic;

	uint32_t section_magic = 0;
	std::memcpy(&section_magic, data + 160, sizeof(uint32_t));

	if (section_magic == DPTH_MAGIC) {
		out.depth_format = DepthFormat::DPTH;
		const size_t depth_offset = 164;
		const size_t depth_bytes = kDepthPixels * sizeof(uint16_t);
		if (size < depth_offset + depth_bytes) {
			error = "DPTH section truncated";
			return false;
		}
		out.depth_buffer.resize(kDepthPixels);
		std::memcpy(out.depth_buffer.data(), data + depth_offset, depth_bytes);
	} else if (section_magic == CDEP_MAGIC) {
		out.depth_format = DepthFormat::CDEP;
		BitReader bits(data + 164, size - 164);
		const uint32_t num_blocks = bits.read_bits(16);
		const uint32_t block_width = bits.read_bits(16);
		// Both counts come straight off the bitstream, and the product used to be
		// computed in 32 bits and handed to resize(): a corrupt 4-byte header
		// (0xffff blocks of 0xffff pixels) demanded ~8 GB before a single delta
		// was read. Two bounds, both taken from the format rather than guessed:
		//   * the depth image is 1024x1024 — DPTH above resizes to exactly that,
		//     and the writer emits 4096 blocks of depth_buffer.size()/4096;
		//   * each block costs at least its 4-bit width + 16-bit base, so the
		//     block count is bounded by the bits actually present. Pixels are NOT
		//     bounded that way: bits_per_delta may legitimately be 0, encoding a
		//     whole block in 20 bits, so a per-pixel bit budget would reject
		//     valid files.
		const uint64_t declared_pixels =
				static_cast<uint64_t>(num_blocks) * static_cast<uint64_t>(block_width);
		if (declared_pixels > kDepthPixels) {
			error = "CDEP declares more depth pixels than the 1024x1024 image holds";
			return false;
		}
		if (static_cast<uint64_t>(num_blocks) * 20u > bits.remaining_bits()) {
			error = "CDEP declares more blocks than the section's bits can encode";
			return false;
		}
		out.depth_buffer.resize(static_cast<size_t>(declared_pixels));
		uint32_t pixel_index = 0;

		for (uint32_t block = 0; block < num_blocks; ++block) {
			const uint32_t bits_per_delta = bits.read_bits(4);
			const uint32_t base_value = bits.read_bits(16);
			for (uint32_t px = 0; px < block_width; ++px) {
				const uint32_t delta = bits.read_bits(static_cast<int>(bits_per_delta));
				out.depth_buffer[static_cast<size_t>(pixel_index++)] = static_cast<uint16_t>(base_value + delta);
			}
		}
	} else {
		error = "Unknown depth section magic";
		return false;
	}

	size_t poly_offset = 0;
	for (size_t off = 164; off + 4 <= size; off += 4) {
		uint32_t section = 0;
		std::memcpy(&section, data + off, sizeof(uint32_t));
		if (section == POLY_MAGIC) {
			poly_offset = off + 4;
			break;
		}
	}

	if (poly_offset > 0 && poly_offset < size) {
		BitReader bits(data + poly_offset, size - poly_offset);
		while (true) {
			bits.align_dword();
			if (bits.at_end()) {
				break;
			}

			const uint32_t tile_x = bits.read_bits(10);
			const uint32_t tile_y = bits.read_bits(10);
			const uint32_t vertex_count = bits.read_bits(16);
			const uint32_t bit_width = bits.read_bits(4);
			const uint32_t lod_count = bits.read_bits(7);
			const uint32_t is_single = bits.read_bits(1);
			const uint32_t chunk_size = bits.read_bits(8);

			// bit_width == 0 joins the structural guards: the writer derives it
			// from tile_size and refuses a non-positive width, so zero never
			// appears in a real tile — and reaching tile_size below would shift
			// by (0 - 1), which is undefined.
			if (vertex_count == 0 || bit_width == 0 || lod_count != 8 || chunk_size != 48) {
				break;
			}

			CptTile tile;
			tile.tile_x = static_cast<uint16_t>(tile_x);
			tile.tile_y = static_cast<uint16_t>(tile_y);
			tile.vertex_count = static_cast<uint16_t>(vertex_count);
			tile.tile_size = static_cast<uint16_t>(1u << (bit_width - 1));
			tile.is_single = is_single != 0;
			// Each vertex costs 2 * bit_width bits. A declared count the section
			// cannot encode means the file is truncated — say so instead of
			// fabricating that many zero vertices out of absent data.
			if (static_cast<uint64_t>(vertex_count) * 2u * bit_width > bits.remaining_bits()) {
				error = "POLY tile declares more vertices than the section can encode";
				return false;
			}
			tile.vertex_indices.resize(static_cast<size_t>(vertex_count) * 2u);

			for (uint32_t i = 0; i < vertex_count; ++i) {
				tile.vertex_indices[static_cast<size_t>(i * 2 + 0)] = static_cast<uint16_t>(bits.read_bits(static_cast<int>(bit_width)));
				tile.vertex_indices[static_cast<size_t>(i * 2 + 1)] = static_cast<uint16_t>(bits.read_bits(static_cast<int>(bit_width)));
			}

			bits.align_dword();
			for (int lod = 0; lod < 8; ++lod) {
				bits.advance_byte();

				const uint32_t total_indices = bits.read_bits(16);
				tile.lods[lod].max_index = static_cast<uint16_t>(bits.read_bits(15));
				tile.lods[lod].is_strip = bits.read_bits(1) != 0;
				tile.lods[lod].indices.resize(total_indices);

				int remaining = static_cast<int>(total_indices);
				int offset = 0;
				while (remaining > 0) {
					const int chunk = std::min(remaining, 48);
					const uint32_t chunk_width = bits.read_bits(4);
					const uint32_t min_value = bits.read_bits(12);
					for (int i = 0; i < chunk; ++i) {
						const uint32_t delta = bits.read_bits(static_cast<int>(chunk_width));
						tile.lods[lod].indices[static_cast<size_t>(offset + i)] = static_cast<uint16_t>(min_value + delta);
					}
					offset += chunk;
					remaining -= 48;
				}
			}

			out.tiles.push_back(std::move(tile));
			if (is_single != 0) {
				break;
			}
		}
	}

	return true;
}

CptFile CptFile::read(const std::string &path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
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

void CptFile::write(const std::string &path) const {
	BitWriter bits;

	Header raw_header = header;
	raw_header.magic = MAGIC;

	bits.write_bytes(&raw_header, sizeof(Header));
	bits.align_dword();
	write_depth_section(bits, depth_format, depth_buffer);
	write_poly_section(bits, tiles);
	bits.write_to_file(path);
}

} // namespace opennova
