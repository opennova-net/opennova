#pragma once
// A PNG writer good enough to make test input for the editor's importers: stored
// (uncompressed) deflate blocks inside a zlib stream, the standard CRC, one filter byte
// per scanline as given. Shared by the import tests and the command-line tests.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace editor_test {

inline uint32_t crc32_of(const uint8_t *data, size_t size, uint32_t crc = 0) {
	static uint32_t table[256];
	static bool ready = false;
	if (!ready) {
		for (uint32_t n = 0; n < 256; ++n) {
			uint32_t c = n;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[n] = c;
		}
		ready = true;
	}
	crc ^= 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFu;
}

inline void put_be32(std::vector<uint8_t> &out, uint32_t value) {
	out.push_back(uint8_t(value >> 24));
	out.push_back(uint8_t(value >> 16));
	out.push_back(uint8_t(value >> 8));
	out.push_back(uint8_t(value));
}

inline void put_chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data,
                      bool corrupt_crc = false) {
	put_be32(out, uint32_t(data.size()));
	std::vector<uint8_t> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	out.insert(out.end(), body.begin(), body.end());
	const uint32_t crc = crc32_of(body.data(), body.size()) ^ (corrupt_crc ? 1u : 0u);
	put_be32(out, crc);
}

inline std::vector<uint8_t> zlib_stored(const std::vector<uint8_t> &raw) {
	std::vector<uint8_t> out = {0x78, 0x01};
	size_t pos = 0;
	do {
		const size_t chunk = std::min<size_t>(65535, raw.size() - pos);
		const bool last = pos + chunk >= raw.size();
		out.push_back(last ? 1 : 0);
		out.push_back(uint8_t(chunk & 0xFF));
		out.push_back(uint8_t(chunk >> 8));
		out.push_back(uint8_t(~chunk & 0xFF));
		out.push_back(uint8_t((~chunk >> 8) & 0xFF));
		out.insert(out.end(), raw.begin() + long(pos), raw.begin() + long(pos + chunk));
		pos += chunk;
	} while (pos < raw.size());
	uint32_t a = 1, b = 0;
	for (const uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
	put_be32(out, (b << 16) | a);
	return out;
}

struct PngSpec {
	uint32_t width = 0, height = 0;
	uint8_t depth = 8, color_type = 6, interlace = 0;
	std::vector<uint8_t> rows; // filter byte + packed scanline, per row
	std::vector<uint8_t> palette, palette_alpha;
	bool corrupt_crc = false;
};

inline std::vector<uint8_t> make_png(const PngSpec &spec) {
	std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::vector<uint8_t> ihdr;
	put_be32(ihdr, spec.width);
	put_be32(ihdr, spec.height);
	ihdr.push_back(spec.depth);
	ihdr.push_back(spec.color_type);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(spec.interlace);
	put_chunk(out, "IHDR", ihdr);
	if (!spec.palette.empty()) put_chunk(out, "PLTE", spec.palette);
	if (!spec.palette_alpha.empty()) put_chunk(out, "tRNS", spec.palette_alpha);
	put_chunk(out, "IDAT", zlib_stored(spec.rows), spec.corrupt_crc);
	put_chunk(out, "IEND", {});
	return out;
}

// An RGBA image of `width` x `height` with a gradient (many distinct colors).
inline std::vector<uint8_t> gradient_png(uint32_t width, uint32_t height, uint8_t seed = 0) {
	PngSpec spec;
	spec.width = width;
	spec.height = height;
	for (uint32_t y = 0; y < height; ++y) {
		spec.rows.push_back(0);
		for (uint32_t x = 0; x < width; ++x) {
			spec.rows.push_back(uint8_t(x * 255 / std::max(1u, width - 1)));
			spec.rows.push_back(uint8_t(y * 255 / std::max(1u, height - 1)));
			spec.rows.push_back(uint8_t((x * 7 + y * 13 + seed) & 0xFF));
			spec.rows.push_back(255);
		}
	}
	return make_png(spec);
}

} // namespace editor_test
