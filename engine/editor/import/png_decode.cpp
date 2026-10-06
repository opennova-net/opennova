#include <editor/import/png_decode.h>

#include <cstdlib>
#include <cstring>

#include <miniz.h> // third_party/miniz: the zlib inflate and the CRC

namespace opennova::editor {

namespace {

const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
// The largest image the importer takes, per side: far past any texture the game
// draws, small enough that a corrupt header cannot ask for gigabytes.
constexpr uint32_t kMaxSidePixels = 16384;

uint32_t read_be32(const uint8_t *p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct Header {
	uint32_t width = 0, height = 0;
	int bit_depth = 0, color_type = 0, interlace = 0;
	int channels = 0; // samples per pixel
};

bool channels_for(int color_type, int &channels) {
	switch (color_type) {
	case 0: channels = 1; return true; // grayscale
	case 2: channels = 3; return true; // RGB
	case 3: channels = 1; return true; // palette indices
	case 4: channels = 2; return true; // grayscale + alpha
	case 6: channels = 4; return true; // RGBA
	default: return false;
	}
}

bool depth_allowed(int color_type, int depth) {
	switch (color_type) {
	case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
	case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
	default: return depth == 8 || depth == 16;
	}
}

int paeth(int a, int b, int c) {
	const int p = a + b - c;
	const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	return pb <= pc ? b : c;
}

// One scanline's samples as 0..255 values (a sub-byte depth unpacked, 16 bits kept high).
uint8_t sample_at(const uint8_t *row, size_t index, int depth) {
	switch (depth) {
	case 8: return row[index];
	case 16: return row[index * 2];
	case 4: return uint8_t((row[index / 2] >> ((1 - index % 2) * 4)) & 0x0F);
	case 2: return uint8_t((row[index / 4] >> ((3 - index % 4) * 2)) & 0x03);
	case 1: return uint8_t((row[index / 8] >> (7 - index % 8)) & 0x01);
	default: return 0;
	}
}

// A sub-byte grayscale sample scaled to 0..255.
uint8_t gray_scale(uint8_t sample, int depth) {
	switch (depth) {
	case 1: return sample ? 255 : 0;
	case 2: return uint8_t(sample * 85);
	case 4: return uint8_t(sample * 17);
	default: return sample;
	}
}

} // namespace

bool is_png(const std::vector<uint8_t> &bytes) {
	return bytes.size() >= 8 && std::memcmp(bytes.data(), kSignature, 8) == 0;
}

bool png_header_size(const std::vector<uint8_t> &bytes, uint32_t &width, uint32_t &height) {
	// The signature, then the IHDR chunk's length and type, then its width and height.
	if (!is_png(bytes) || bytes.size() < 24 || std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
		return false;
	width = read_be32(bytes.data() + 16);
	height = read_be32(bytes.data() + 20);
	return true;
}

namespace {

// A PNG read to its unfiltered scanlines: its header, its palette (RGB triples) and the palette's
// alpha, and the rows, each behind its filter byte (stride + 1 bytes a row).
struct Scanlines {
	Header header;
	std::vector<uint8_t> palette;
	std::vector<uint8_t> palette_alpha;
	std::vector<uint8_t> raw;
	size_t stride = 0;
	const uint8_t *row(size_t y) const { return &raw[y * (stride + 1) + 1]; }
};

bool read_scanlines(const std::vector<uint8_t> &bytes, Scanlines &image, std::string &error) {
	error.clear();
	if (!is_png(bytes)) {
		error = "Not a PNG file.";
		return false;
	}
	Header &header = image.header;
	bool have_header = false, done = false;
	std::vector<uint8_t> &palette = image.palette; // RGB triples
	std::vector<uint8_t> &palette_alpha = image.palette_alpha;
	std::vector<uint8_t> compressed;
	size_t pos = 8;
	while (!done) {
		if (pos + 8 > bytes.size()) {
			error = "Truncated PNG: a chunk header is cut off.";
			return false;
		}
		const uint32_t length = read_be32(&bytes[pos]);
		const uint8_t *type = &bytes[pos + 4];
		if (length > bytes.size() - pos - 12) {
			error = "Truncated PNG: a chunk runs past the end of the file.";
			return false;
		}
		const uint8_t *data = &bytes[pos + 8];
		const uint32_t crc = read_be32(data + length);
		const mz_ulong computed = mz_crc32(mz_crc32(0, type, 4), data, length);
		if (uint32_t(computed) != crc) {
			error = std::string("Corrupt PNG: the ") + std::string(reinterpret_cast<const char *>(type), 4) + " chunk fails its CRC.";
			return false;
		}
		const std::string name(reinterpret_cast<const char *>(type), 4);
		if (name == "IHDR") {
			if (length != 13) { error = "Corrupt PNG: a bad IHDR length."; return false; }
			header.width = read_be32(data);
			header.height = read_be32(data + 4);
			header.bit_depth = data[8];
			header.color_type = data[9];
			header.interlace = data[12];
			if (header.width == 0 || header.height == 0 || header.width > kMaxSidePixels || header.height > kMaxSidePixels) {
				error = "Unsupported PNG: the image is empty or larger than " + std::to_string(kMaxSidePixels) + " pixels a side.";
				return false;
			}
			if (!channels_for(header.color_type, header.channels) || !depth_allowed(header.color_type, header.bit_depth)) {
				error = "Unsupported PNG: color type " + std::to_string(header.color_type) + " at " +
				        std::to_string(header.bit_depth) + " bits.";
				return false;
			}
			if (data[10] != 0 || data[11] != 0) { error = "Unsupported PNG: an unknown compression or filter method."; return false; }
			if (header.interlace != 0) { error = "Unsupported PNG: interlaced images are not imported; save it without interlacing."; return false; }
			have_header = true;
		} else if (name == "PLTE") {
			if (length % 3 != 0 || length > 768) { error = "Corrupt PNG: a bad palette length."; return false; }
			palette.assign(data, data + length);
		} else if (name == "tRNS") {
			if (header.color_type == 3) palette_alpha.assign(data, data + length);
		} else if (name == "IDAT") {
			compressed.insert(compressed.end(), data, data + length);
		} else if (name == "IEND") {
			done = true;
		}
		pos += 12 + length;
	}
	if (!have_header) { error = "Corrupt PNG: no IHDR."; return false; }
	if (header.color_type == 3 && palette.empty()) { error = "Corrupt PNG: a palette image without a palette."; return false; }
	if (compressed.empty()) { error = "Corrupt PNG: no image data."; return false; }

	const size_t bits_per_pixel = size_t(header.channels) * size_t(header.bit_depth);
	const size_t stride = (size_t(header.width) * bits_per_pixel + 7) / 8;
	image.stride = stride;
	const size_t filter_bytes = std::max<size_t>(1, bits_per_pixel / 8);
	const size_t raw_size = (stride + 1) * size_t(header.height);
	std::vector<uint8_t> &raw = image.raw;
	raw.assign(raw_size, 0);
	mz_ulong produced = mz_ulong(raw_size);
	const int inflated = mz_uncompress(raw.data(), &produced, compressed.data(), mz_ulong(compressed.size()));
	if (inflated != MZ_OK || produced != raw_size) {
		error = "Corrupt PNG: the image data does not inflate to the expected size.";
		return false;
	}

	// Unfilter in place, scanline by scanline.
	std::vector<uint8_t> previous(stride, 0);
	for (size_t y = 0; y < header.height; ++y) {
		uint8_t *line = &raw[y * (stride + 1)];
		const uint8_t filter = line[0];
		uint8_t *row = line + 1;
		for (size_t i = 0; i < stride; ++i) {
			const int a = i >= filter_bytes ? row[i - filter_bytes] : 0;
			const int b = previous[i];
			const int c = i >= filter_bytes ? previous[i - filter_bytes] : 0;
			int value = row[i];
			switch (filter) {
			case 0: break;
			case 1: value += a; break;
			case 2: value += b; break;
			case 3: value += (a + b) / 2; break;
			case 4: value += paeth(a, b, c); break;
			default: error = "Corrupt PNG: an unknown scanline filter."; return false;
			}
			row[i] = uint8_t(value);
		}
		std::memcpy(previous.data(), row, stride);
	}
	return true;
}

// A 16-bit sample, big-endian, at `index` of a 16-bit row.
uint16_t sample16_at(const uint8_t *row, size_t index) {
	return uint16_t((uint16_t(row[index * 2]) << 8) | row[index * 2 + 1]);
}

} // namespace

bool decode_png(const std::vector<uint8_t> &bytes, RgbaImage &out, std::string &error) {
	Scanlines image;
	if (!read_scanlines(bytes, image, error)) return false;
	const Header &header = image.header;
	const std::vector<uint8_t> &palette = image.palette;
	const std::vector<uint8_t> &palette_alpha = image.palette_alpha;

	out.width = int(header.width);
	out.height = int(header.height);
	out.pixels.assign(size_t(out.width) * size_t(out.height) * 4, 255);
	const size_t palette_entries = palette.size() / 3;
	for (size_t y = 0; y < header.height; ++y) {
		const uint8_t *row = image.row(y);
		for (size_t x = 0; x < header.width; ++x) {
			uint8_t *pixel = &out.pixels[(y * header.width + x) * 4];
			switch (header.color_type) {
			case 0: {
				const uint8_t g = gray_scale(sample_at(row, x, header.bit_depth), header.bit_depth);
				pixel[0] = pixel[1] = pixel[2] = g;
				break;
			}
			case 2:
				pixel[0] = sample_at(row, x * 3, header.bit_depth);
				pixel[1] = sample_at(row, x * 3 + 1, header.bit_depth);
				pixel[2] = sample_at(row, x * 3 + 2, header.bit_depth);
				break;
			case 3: {
				const size_t index = sample_at(row, x, header.bit_depth);
				if (index >= palette_entries) { error = "Corrupt PNG: a pixel indexes past the palette."; return false; }
				pixel[0] = palette[index * 3];
				pixel[1] = palette[index * 3 + 1];
				pixel[2] = palette[index * 3 + 2];
				if (index < palette_alpha.size()) pixel[3] = palette_alpha[index];
				break;
			}
			case 4: {
				const uint8_t g = sample_at(row, x * 2, header.bit_depth);
				pixel[0] = pixel[1] = pixel[2] = g;
				pixel[3] = sample_at(row, x * 2 + 1, header.bit_depth);
				break;
			}
			case 6:
				pixel[0] = sample_at(row, x * 4, header.bit_depth);
				pixel[1] = sample_at(row, x * 4 + 1, header.bit_depth);
				pixel[2] = sample_at(row, x * 4 + 2, header.bit_depth);
				pixel[3] = sample_at(row, x * 4 + 3, header.bit_depth);
				break;
			default: break;
			}
		}
	}
	return true;
}

bool decode_png_gray(const std::vector<uint8_t> &bytes, GrayImage &out, std::string &error) {
	Scanlines image;
	if (!read_scanlines(bytes, image, error)) return false;
	const Header &header = image.header;
	const bool wide = header.bit_depth == 16;
	out.width = int(header.width);
	out.height = int(header.height);
	out.max_value = wide ? 65535 : 255;
	out.samples.assign(size_t(out.width) * size_t(out.height), 0);
	const size_t palette_entries = image.palette.size() / 3;
	// A sample of the image's own depth: 16 bits whole, 8 as they are, fewer scaled to 0..255.
	const auto sample = [&](const uint8_t *row, size_t index) -> uint32_t {
		if (wide) return sample16_at(row, index);
		return header.color_type == 0 ? gray_scale(sample_at(row, index, header.bit_depth), header.bit_depth)
		                              : sample_at(row, index, header.bit_depth);
	};
	for (size_t y = 0; y < header.height; ++y) {
		const uint8_t *row = image.row(y);
		for (size_t x = 0; x < header.width; ++x) {
			uint32_t value = 0;
			switch (header.color_type) {
			case 0: value = sample(row, x); break;
			case 4: value = sample(row, x * 2); break;
			case 2: value = (sample(row, x * 3) + sample(row, x * 3 + 1) + sample(row, x * 3 + 2) + 1) / 3; break;
			case 6: value = (sample(row, x * 4) + sample(row, x * 4 + 1) + sample(row, x * 4 + 2) + 1) / 3; break;
			case 3: {
				const size_t index = sample_at(row, x, header.bit_depth);
				if (index >= palette_entries) { error = "Corrupt PNG: a pixel indexes past the palette."; return false; }
				value = (uint32_t(image.palette[index * 3]) + image.palette[index * 3 + 1] + image.palette[index * 3 + 2] + 1) / 3;
				break;
			}
			default: break;
			}
			out.samples[y * header.width + x] = uint16_t(value);
		}
	}
	return true;
}

} // namespace opennova::editor
