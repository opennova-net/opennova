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

// The image's rows of `row` bytes each behind filter 0, deflated, between the signature and IEND: IHDR of `colour`
// at 8 bits a sample, then `palette`'s PLTE where it has one.
std::vector<uint8_t> encode(const uint8_t *texels, uint32_t width, uint32_t height, size_t row, uint8_t colour,
                            const uint8_t *palette, uint32_t entries) {
	std::vector<uint8_t> raw;
	raw.reserve((row + 1) * height);
	for (uint32_t y = 0; y < height; ++y) {
		raw.push_back(0);
		raw.insert(raw.end(), texels + y * row, texels + (y + 1) * row);
	}
	mz_ulong packed_size = mz_compressBound(mz_ulong(raw.size()));
	std::vector<uint8_t> packed(packed_size);
	if (mz_compress2(packed.data(), &packed_size, raw.data(), mz_ulong(raw.size()), MZ_DEFAULT_COMPRESSION) != MZ_OK)
		return {};
	packed.resize(packed_size);
	std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::vector<uint8_t> header;
	put_u32_be(header, width);
	put_u32_be(header, height);
	header.push_back(8);      // bits a sample
	header.push_back(colour); // 6 RGBA, 3 palette, 0 grey
	header.push_back(0);      // deflate
	header.push_back(0);      // filters of method 0
	header.push_back(0);      // not interlaced
	put_chunk(out, "IHDR", header.data(), header.size());
	if (palette) put_chunk(out, "PLTE", palette, size_t(entries) * 3);
	put_chunk(out, "IDAT", packed.data(), packed.size());
	put_chunk(out, "IEND", nullptr, 0);
	return out;
}

} // namespace

std::vector<uint8_t> encode_png_rgba(const uint8_t *rgba, uint32_t width, uint32_t height) {
	if (!rgba || width == 0 || height == 0) return {};
	return encode(rgba, width, height, size_t(width) * 4, 6, nullptr, 0);
}

std::vector<uint8_t> encode_png_indexed(const uint8_t *indices, uint32_t width, uint32_t height, const uint8_t *palette,
                                        uint32_t entries) {
	if (!indices || !palette || width == 0 || height == 0 || entries == 0 || entries > 256) return {};
	return encode(indices, width, height, size_t(width), 3, palette, entries);
}

std::vector<uint8_t> encode_png_grey(const uint8_t *levels, uint32_t width, uint32_t height) {
	if (!levels || width == 0 || height == 0) return {};
	return encode(levels, width, height, size_t(width), 0, nullptr, 0);
}

} // namespace opennova::png
