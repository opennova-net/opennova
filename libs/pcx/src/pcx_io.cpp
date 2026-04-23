#include "pcx/pcx_io.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace opennova {

namespace {

bool parse_header(const uint8_t *data,
                  size_t size,
                  int &width,
                  int &height,
                  int &planes,
                  int &bytes_per_line,
                  std::string &error) {
	if (size < 128) {
		error = "PCX header truncated";
		return false;
	}
	if (data[0] != 0x0A) {
		error = "Bad PCX manufacturer byte";
		return false;
	}

	const int xmin = data[4] | (data[5] << 8);
	const int ymin = data[6] | (data[7] << 8);
	const int xmax = data[8] | (data[9] << 8);
	const int ymax = data[10] | (data[11] << 8);
	width = xmax - xmin + 1;
	height = ymax - ymin + 1;
	planes = data[65];
	bytes_per_line = data[66] | (data[67] << 8);

	if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
		error = "PCX dimensions out of range";
		return false;
	}
	if (bytes_per_line < width) {
		bytes_per_line = width;
	}

	return true;
}

bool decode_scanline_rle(const uint8_t *data,
                         size_t size,
                         size_t &pos,
                         uint8_t *dst,
                         int count,
                         std::string &error) {
	int written = 0;
	while (written < count && pos < size) {
		const uint8_t byte = data[pos++];
		if ((byte & 0xC0) == 0xC0) {
			const int run = byte & 0x3F;
			if (pos >= size) {
				error = "PCX RLE run truncated";
				return false;
			}
			const uint8_t value = data[pos++];
			for (int i = 0; i < run && written < count; ++i) {
				dst[written++] = value;
			}
		} else {
			dst[written++] = byte;
		}
	}

	if (written != count) {
		error = "PCX scanline truncated";
		return false;
	}
	return true;
}

bool load_file_bytes(const std::string &path, std::vector<uint8_t> &bytes, std::string &error) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.is_open()) {
		error = "Failed to open file: " + path;
		return false;
	}

	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	if (size < 0) {
		error = "Failed to stat file: " + path;
		return false;
	}

	bytes.resize(static_cast<size_t>(size));
	if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()), size)) {
		error = "Failed to read file: " + path;
		return false;
	}

	return true;
}

} // namespace

bool decode_pcx_indexed(const uint8_t *data, size_t size, IndexedImage8 &out, std::string &error) {
	out = IndexedImage8{};

	int width = 0;
	int height = 0;
	int planes = 0;
	int bytes_per_line = 0;
	if (!parse_header(data, size, width, height, planes, bytes_per_line, error)) {
		return false;
	}
	if (planes != 1) {
		error = "PCX is not an 8-bit indexed image";
		return false;
	}
	if (size < 128 + 769) {
		error = "PCX palette missing";
		return false;
	}

	out.width = width;
	out.height = height;
	out.indices.assign(static_cast<size_t>(width * height), 0);

	size_t pos = 128;
	std::vector<uint8_t> row(static_cast<size_t>(bytes_per_line), 0);
	for (int y = 0; y < height; ++y) {
		if (!decode_scanline_rle(data, size, pos, row.data(), bytes_per_line, error)) {
			return false;
		}
		for (int x = 0; x < width; ++x) {
			out.indices[static_cast<size_t>(y * width + x)] = row[static_cast<size_t>(x)];
		}
	}

	const uint8_t *palette_start = data + size - 769;
	if (palette_start[0] != 0x0C) {
		error = "PCX 256-color palette marker missing";
		return false;
	}

	const uint8_t *palette = palette_start + 1;
	for (int i = 0; i < 256; ++i) {
		out.palette[i][0] = palette[i * 3 + 0];
		out.palette[i][1] = palette[i * 3 + 1];
		out.palette[i][2] = palette[i * 3 + 2];
	}

	return true;
}

bool decode_pcx_rgb(const uint8_t *data, size_t size, RgbImage &out, std::string &error) {
	out = RgbImage{};

	int width = 0;
	int height = 0;
	int planes = 0;
	int bytes_per_line = 0;
	if (!parse_header(data, size, width, height, planes, bytes_per_line, error)) {
		return false;
	}

	out.width = width;
	out.height = height;
	out.pixels.assign(static_cast<size_t>(width * height * 3), 0);

	if (planes == 3) {
		size_t pos = 128;
		std::vector<uint8_t> scanline(static_cast<size_t>(bytes_per_line * 3), 0);
		for (int y = 0; y < height; ++y) {
			if (!decode_scanline_rle(data, size, pos, scanline.data(), bytes_per_line * 3, error)) {
				return false;
			}
			for (int x = 0; x < width; ++x) {
				const size_t dst = static_cast<size_t>((y * width + x) * 3);
				out.pixels[dst + 0] = scanline[static_cast<size_t>(x)];
				out.pixels[dst + 1] = scanline[static_cast<size_t>(bytes_per_line + x)];
				out.pixels[dst + 2] = scanline[static_cast<size_t>(bytes_per_line * 2 + x)];
			}
		}
		return true;
	}

	if (planes == 1) {
		IndexedImage8 indexed;
		if (!decode_pcx_indexed(data, size, indexed, error)) {
			return false;
		}

		out.width = indexed.width;
		out.height = indexed.height;
		out.pixels.resize(static_cast<size_t>(indexed.width * indexed.height * 3));
		for (int i = 0; i < indexed.width * indexed.height; ++i) {
			const uint8_t idx = indexed.indices[static_cast<size_t>(i)];
			out.pixels[static_cast<size_t>(i * 3 + 0)] = indexed.palette[idx][0];
			out.pixels[static_cast<size_t>(i * 3 + 1)] = indexed.palette[idx][1];
			out.pixels[static_cast<size_t>(i * 3 + 2)] = indexed.palette[idx][2];
		}
		return true;
	}

	error = "Unsupported PCX plane count";
	return false;
}

bool encode_pcx_indexed(const IndexedImage8 &image, std::vector<uint8_t> &out, std::string &error) {
	out.clear();

	if (image.width <= 0 || image.height <= 0) {
		error = "Invalid PCX dimensions";
		return false;
	}
	if (image.indices.size() < static_cast<size_t>(image.width * image.height)) {
		error = "PCX indexed payload too small";
		return false;
	}

	const int bytes_per_line = image.width + (image.width & 1);
	out.reserve(static_cast<size_t>(128 + image.height * bytes_per_line + 769));

	uint8_t header[128] = {};
	header[0] = 0x0A;
	header[1] = 5;
	header[2] = 1;
	header[3] = 8;
	std::fill(header + 16, header + 64, 0xFF);
	header[8] = static_cast<uint8_t>((image.width - 1) & 0xFF);
	header[9] = static_cast<uint8_t>(((image.width - 1) >> 8) & 0xFF);
	header[10] = static_cast<uint8_t>((image.height - 1) & 0xFF);
	header[11] = static_cast<uint8_t>(((image.height - 1) >> 8) & 0xFF);
	header[12] = 72;
	header[14] = 72;
	header[65] = 1;
	header[66] = static_cast<uint8_t>(bytes_per_line & 0xFF);
	header[67] = static_cast<uint8_t>((bytes_per_line >> 8) & 0xFF);
	header[68] = 1;
	out.insert(out.end(), header, header + 128);

	std::vector<uint8_t> row(static_cast<size_t>(bytes_per_line), 0);
	for (int y = 0; y < image.height; ++y) {
		for (int x = 0; x < image.width; ++x) {
			row[static_cast<size_t>(x)] = image.indices[static_cast<size_t>(y * image.width + x)];
		}
		if (bytes_per_line > image.width) {
			row[static_cast<size_t>(image.width)] = 0;
		}

		int pos = 0;
		while (pos < bytes_per_line) {
			const uint8_t value = row[static_cast<size_t>(pos)];
			int run = 1;
			while (pos + run < bytes_per_line && run < 63 && row[static_cast<size_t>(pos + run)] == value) {
				++run;
			}

			if (run > 1 || (value & 0xC0) == 0xC0) {
				out.push_back(static_cast<uint8_t>(0xC0 | run));
				out.push_back(value);
			} else {
				out.push_back(value);
			}
			pos += run;
		}
	}

	out.push_back(0x0C);
	for (int i = 0; i < 256; ++i) {
		out.push_back(image.palette[i][0]);
		out.push_back(image.palette[i][1]);
		out.push_back(image.palette[i][2]);
	}

	return true;
}

bool load_pcx_rgb_file(const std::string &path, RgbImage &out, std::string &error) {
	std::vector<uint8_t> bytes;
	if (!load_file_bytes(path, bytes, error)) {
		return false;
	}
	return decode_pcx_rgb(bytes.data(), bytes.size(), out, error);
}

bool load_pcx_indexed_file(const std::string &path, IndexedImage8 &out, std::string &error) {
	std::vector<uint8_t> bytes;
	if (!load_file_bytes(path, bytes, error)) {
		return false;
	}
	return decode_pcx_indexed(bytes.data(), bytes.size(), out, error);
}

} // namespace opennova
