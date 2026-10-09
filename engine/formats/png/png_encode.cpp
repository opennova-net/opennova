#include <formats/png/png_encode.h>

#include <cstddef>
#include <cstring>

#include <miniz.h> // third_party/miniz: the zlib deflate and the CRC

namespace opennova::png {

namespace {

void put_u32_be(std::vector<uint8_t> &out, uint32_t value) {
	out.push_back(uint8_t(value >> 24));
	out.push_back(uint8_t(value >> 16));
	out.push_back(uint8_t(value >> 8));
	out.push_back(uint8_t(value));
}

// A chunk: its length, its type, its data, the CRC of the type and the data.
void put_chunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t size) {
	put_u32_be(out, uint32_t(size));
	const size_t start = out.size();
	out.insert(out.end(), type, type + 4);
	if (size) out.insert(out.end(), data, data + size);
	put_u32_be(out, uint32_t(mz_crc32(MZ_CRC32_INIT, out.data() + start, out.size() - start)));
}

} // namespace

std::vector<uint8_t> encode_png_rgba(const uint8_t *rgba, uint32_t width, uint32_t height) {
	if (!rgba || width == 0 || height == 0) return {};
	const size_t row = size_t(width) * 4;
	// Each row behind its filter byte (0: none).
	std::vector<uint8_t> raw;
	raw.reserve((row + 1) * height);
	for (uint32_t y = 0; y < height; ++y) {
		raw.push_back(0);
		raw.insert(raw.end(), rgba + y * row, rgba + (y + 1) * row);
	}
	mz_ulong packed_size = mz_compressBound(mz_ulong(raw.size()));
	std::vector<uint8_t> packed(packed_size);
	if (mz_compress2(packed.data(), &packed_size, raw.data(), mz_ulong(raw.size()), MZ_DEFAULT_COMPRESSION) != MZ_OK)
		return {};
	packed.resize(packed_size);
	std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	std::vector<uint8_t> header;
	put_u32_be(header, width);
	put_u32_be(header, height);
	header.push_back(8); // bits a sample
	header.push_back(6); // RGBA
	header.push_back(0); // deflate
	header.push_back(0); // filters of method 0
	header.push_back(0); // not interlaced
	put_chunk(out, "IHDR", header.data(), header.size());
	put_chunk(out, "IDAT", packed.data(), packed.size());
	put_chunk(out, "IEND", nullptr, 0);
	return out;
}

} // namespace opennova::png
