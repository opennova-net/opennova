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
// at `bits` bits a sample (8, or 16 big-endian), then `palette`'s PLTE and `alpha`'s tRNS where it has them.
std::vector<uint8_t> encode(const uint8_t *texels, uint32_t width, uint32_t height, size_t row, uint8_t colour,
                            const uint8_t *palette, uint32_t entries, const uint8_t *alpha, uint32_t alpha_entries,
                            uint8_t bits = 8) {
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
	header.push_back(bits);   // bits a sample
	header.push_back(colour); // 6 RGBA, 3 palette, 0 grey
	header.push_back(0);      // deflate
	header.push_back(0);      // filters of method 0
	header.push_back(0);      // not interlaced
	put_chunk(out, "IHDR", header.data(), header.size());
	if (palette) put_chunk(out, "PLTE", palette, size_t(entries) * 3);
	if (alpha && alpha_entries) put_chunk(out, "tRNS", alpha, alpha_entries);
	put_chunk(out, "IDAT", packed.data(), packed.size());
	put_chunk(out, "IEND", nullptr, 0);
	return out;
}

} // namespace

std::vector<uint8_t> encode_png_rgba(const uint8_t *rgba, uint32_t width, uint32_t height) {
	if (!rgba || width == 0 || height == 0) return {};
	return encode(rgba, width, height, size_t(width) * 4, 6, nullptr, 0, nullptr, 0);
}

std::vector<uint8_t> encode_png_indexed(const uint8_t *indices, uint32_t width, uint32_t height, const uint8_t *palette,
                                        uint32_t entries, const uint8_t *alpha, uint32_t alpha_entries) {
	if (!indices || !palette || width == 0 || height == 0 || entries == 0 || entries > 256 || alpha_entries > entries ||
	    (alpha_entries && !alpha))
		return {};
	// Every index within the palette (a reader refuses one past it).
	const size_t count = size_t(width) * size_t(height);
	for (size_t i = 0; i < count; ++i)
		if (indices[i] >= entries) return {};
	return encode(indices, width, height, size_t(width), 3, palette, entries, alpha, alpha_entries);
}

std::vector<uint8_t> encode_png_gray(const uint8_t *levels, uint32_t width, uint32_t height) {
	if (!levels || width == 0 || height == 0) return {};
	return encode(levels, width, height, size_t(width), 0, nullptr, 0, nullptr, 0);
}

std::vector<uint8_t> encode_png_gray16(const uint16_t *samples, uint32_t width, uint32_t height) {
	if (!samples || width == 0 || height == 0) return {};
	// Each sample big-endian, as PNG keeps a 16-bit one.
	std::vector<uint8_t> bytes(size_t(width) * height * 2);
	for (size_t i = 0; i < size_t(width) * height; ++i) {
		bytes[i * 2] = uint8_t(samples[i] >> 8);
		bytes[i * 2 + 1] = uint8_t(samples[i]);
	}
	return encode(bytes.data(), width, height, size_t(width) * 2, 0, nullptr, 0, nullptr, 0, 16);
}

} // namespace opennova::png
