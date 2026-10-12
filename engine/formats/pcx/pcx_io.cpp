#include <formats/pcx/pcx_io.h>

#include <algorithm>
#include <vector>

namespace opennova {

namespace {

// The game's PCX readers allocate the buffer the header sizes and fail the load (code 2)
// when that allocation fails [orig: Texture_LoadPCXFromPFF32 @ 0x56EB1E;
// Texture_LoadPCXFromPFF8Bit @ 0x56E0A0]. The port allocates it only when the file's
// data can describe it, 32 pixels a byte after the 128-byte header (a two-byte run
// pair expands to at most 63), and never past Godot's own image limit.
constexpr size_t kMaxPcxPixels = size_t(1) << 28;

bool pcx_pixels_fit(size_t pixels, size_t size) {
	const size_t present = size > 128 ? size - 128 : 0;
	return pixels <= kMaxPcxPixels && pixels <= present * 32u;
}

bool parse_header(const uint8_t *data,
                  size_t size,
                  int &width,
                  int &height,
                  int &planes,
                  int &bytes_per_line,
                  std::string &error) {
	PcxHeader header;
	if (!pcx_read_header(data, size, header)) {
		error = size < 128 ? "PCX header truncated" : "Bad PCX manufacturer byte";
		return false;
	}
	width = header.width;
	height = header.height;
	planes = header.planes;
	bytes_per_line = header.bytes_per_line;

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

// One plane's row, run-length coded as the readers decode it: a run of 2 to 63 equal bytes, or a byte with
// its top two bits set, a count byte (0xC0 | n) and the byte; any other a byte alone (PcxCoding::Shipped).
void append_rle_row(const uint8_t *row, int length, std::vector<uint8_t> &out) {
	int pos = 0;
	while (pos < length) {
		const uint8_t value = row[pos];
		int run = 1;
		while (pos + run < length && run < 63 && row[pos + run] == value) ++run;
		if (run > 1 || (value & 0xC0) == 0xC0) {
			out.push_back(static_cast<uint8_t>(0xC0 | run));
			out.push_back(value);
		} else {
			out.push_back(value);
		}
		pos += run;
	}
}

// JO's mission editor's row coder (PcxCoding::Med): the shipped coding but for the row's last run, which it
// writes as a count pair whatever it holds [orig: JOTACmed sub_45E8A0 @ 0x45E8A0: a full run of 63 written
// as 0xFF and the byte, the current byte opening the next run (@ 0x45E8EB..0x45E913); a run of one below
// 0xC0 alone, any other a pair (@ 0x45E946..0x45E980); the tail pair @ 0x45E9BF..0x45E9E7].
void append_med_rle_row(const uint8_t *row, int length, std::vector<uint8_t> &out) {
	if (length <= 0) return;
	uint8_t value = row[0];
	int run = 1;
	for (int i = 1; i < length; ++i) {
		if (row[i] == value) {
			if (run == 63) {
				out.push_back(0xFF);
				out.push_back(value);
				run = 1;
			} else {
				++run;
			}
			continue;
		}
		if (run != 1 || (value & 0xC0) == 0xC0) {
			out.push_back(static_cast<uint8_t>(0xC0 | run));
			out.push_back(value);
		} else {
			out.push_back(value);
		}
		value = row[i];
		run = 1;
	}
	out.push_back(static_cast<uint8_t>(0xC0 | run));
	out.push_back(value);
}

// The rows of an indexed image, each `width` bytes, in a coding (Other codes as Shipped).
std::vector<uint8_t> coded_rows(const IndexedImage8 &image, PcxCoding coding) {
	std::vector<uint8_t> out;
	out.reserve(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) / 2u + 16u);
	for (int y = 0; y < image.height; ++y) {
		const uint8_t *row = image.indices.data() + static_cast<size_t>(y) * static_cast<size_t>(image.width);
		if (coding == PcxCoding::Med)
			append_med_rle_row(row, image.width, out);
		else
			append_rle_row(row, image.width, out);
	}
	return out;
}

uint16_t u16_at(const uint8_t *data, size_t at) {
	return static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
}

void put_u16(uint8_t *data, size_t at, uint16_t value) {
	data[at] = static_cast<uint8_t>(value & 0xFF);
	data[at + 1] = static_cast<uint8_t>(value >> 8);
}

// The form a decoded indexed image's file holds: its header bytes, and the coding its rows show. The rows
// can be either coder's only where the header says one 8-bit plane of RLE rows exactly the width long and
// the palette follows them.
PcxIndexedForm read_indexed_form(const uint8_t *data, size_t size, size_t rows_end, const IndexedImage8 &image) {
	PcxIndexedForm form;
	form.version = data[1];
	form.window_x = u16_at(data, 4);
	form.window_y = u16_at(data, 6);
	form.hdpi = u16_at(data, 12);
	form.vdpi = u16_at(data, 14);
	for (size_t entry = 0; entry < form.ega.size(); ++entry)
		std::copy(data + 16 + entry * 3, data + 19 + entry * 3, form.ega[entry].begin());
	form.reserved = data[64];
	form.palette_info = u16_at(data, 68);
	form.hscreen = u16_at(data, 70);
	form.vscreen = u16_at(data, 72);
	form.coding = PcxCoding::Other;
	const bool plain = data[2] == 1 && data[3] == 8 && data[65] == 1 &&
	                   u16_at(data, 66) == static_cast<uint16_t>(image.width) && rows_end == size - 769;
	if (!plain) return form;
	const size_t body = rows_end - 128;
	const auto matches = [&](PcxCoding coding) {
		const std::vector<uint8_t> rows = coded_rows(image, coding);
		return rows.size() == body && std::equal(rows.begin(), rows.end(), data + 128);
	};
	const bool shipped = matches(PcxCoding::Shipped);
	const bool med = matches(PcxCoding::Med);
	if (shipped && med)
		form.coding = form.same_header(pcx_med_form()) ? PcxCoding::Med : PcxCoding::Shipped;
	else if (shipped)
		form.coding = PcxCoding::Shipped;
	else if (med)
		form.coding = PcxCoding::Med;
	return form;
}

} // namespace

PcxEgaPalette pcx_ega_filled(uint8_t value) {
	PcxEgaPalette out{};
	for (std::array<uint8_t, 3> &entry : out) entry.fill(value);
	return out;
}

PcxIndexedForm pcx_med_form() {
	// [orig: JOTACmed sub_45EB60 @ 0x45EBA0 (the 128 bytes zeroed), 0x45EBDA (0A 05), 0x45EBE4 (01 08),
	//  0x45EBEE (one plane), 0x45EBF3 (palette info 1), 0x45EBFF (the bytes a line)]
	PcxIndexedForm form;
	form.hdpi = 0;
	form.vdpi = 0;
	form.ega = pcx_ega_filled(0);
	form.palette_info = 1;
	form.coding = PcxCoding::Med;
	return form;
}

bool pcx_read_header(const uint8_t *data, size_t size, PcxHeader &out) {
	out = PcxHeader{};
	if (data == nullptr || size < 128 || data[0] != 0x0A) {
		return false;
	}
	const int xmin = data[4] | (data[5] << 8);
	const int ymin = data[6] | (data[7] << 8);
	const int xmax = data[8] | (data[9] << 8);
	const int ymax = data[10] | (data[11] << 8);
	out.bits = data[3];
	out.width = xmax - xmin + 1;
	out.height = ymax - ymin + 1;
	out.planes = data[65];
	out.bytes_per_line = static_cast<uint16_t>(data[66] | (data[67] << 8));
	return true;
}

// [orig: Texture_LoadPCXFromPFF32 @ 0x56EA30 — the 24-bit path @ 0x56EB31, the indexed
//  rows to BytesPerLine at a stride of the width @ 0x56ED70..0x56EDFC]
PcxRowFit pcx_row_fit(const PcxHeader &header) {
	if (header.planes == 3) {
		return PcxRowFit::Exact;
	}
	const uint32_t width = static_cast<uint32_t>(header.width);
	if (header.bytes_per_line > width) {
		return PcxRowFit::Overrun;
	}
	return header.bytes_per_line < width ? PcxRowFit::Short : PcxRowFit::Exact;
}

bool decode_pcx_indexed(const uint8_t *data, size_t size, IndexedImage8 &out, std::string &error,
                        PcxIndexedForm *form) {
	out = IndexedImage8{};
	if (form != nullptr) *form = PcxIndexedForm{};

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

	if (form != nullptr) *form = read_indexed_form(data, size, pos, out);
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

// [orig: Texture_LoadPCXFromPFF8Bit @ 0x56E0A0 — the 0x46-byte header read, the bits
// per pixel test (code 3, @ 0x56E109), the sides from the window (@ 0x56E126..0x56E14A),
// one buffer of the pixels plus 1024 bytes with the palette at its end, the byte before
// the last 768 read and never tested, the palette stored B, G, R, 0, the RLE rows of
// BytesPerLine indices at a stride of `width` read from offset 128 through the file;
// then Texture_LoadFromArchive @ 0x58B980's luminance table (@ 0x58BC35..0x58BCA9)
// and per-pixel alpha (@ 0x58BCEE)]
bool decode_pcx_luminance_alpha(const uint8_t *data, size_t size, RgbaImage &out, std::string &error) {
	out = RgbaImage{};
	if (data == nullptr || size < 0x46) {
		error = "PCX header truncated";
		return false;
	}
	if (data[3] != 8) {
		error = "PCX is not 8 bits per pixel";
		return false;
	}
	const int16_t xmin = static_cast<int16_t>(data[4] | (data[5] << 8));
	const int16_t ymin = static_cast<int16_t>(data[6] | (data[7] << 8));
	const int16_t xmax = static_cast<int16_t>(data[8] | (data[9] << 8));
	const int16_t ymax = static_cast<int16_t>(data[10] | (data[11] << 8));
	const int16_t width = static_cast<int16_t>(xmax - xmin + 1);
	const int16_t height = static_cast<int16_t>(ymax - ymin + 1);
	const int bytes_per_line = data[0x42] | (data[0x43] << 8);
	if (width <= 0 || height <= 0) {
		error = "PCX dimensions out of range";
		return false;
	}
	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (!pcx_pixels_fit(pixels, size)) {
		error = "PCX names more pixels than its data can describe";
		return false;
	}
	const auto byte_at = [&](size_t offset) -> uint8_t { return offset < size ? data[offset] : 0; };
	// The indices, then the palette as the reader stores it: B, G, R, 0 per entry.
	std::vector<uint8_t> buffer(pixels + 1024u, 0);
	const size_t palette_at = size >= 768 ? size - 768 : 0;
	for (size_t i = 0; i < 256; ++i) {
		uint8_t *entry = &buffer[pixels + 4u * i];
		entry[0] = byte_at(palette_at + 3u * i + 2u);
		entry[1] = byte_at(palette_at + 3u * i + 1u);
		entry[2] = byte_at(palette_at + 3u * i);
		entry[3] = 0;
	}
	// The rows: a row's pad pixels spill onto the next row's start, the last row's onto
	// the palette that follows the indices (retail writes past the buffer after 1024
	// bytes; this port stops there). Bytes past the file read as 0.
	size_t pos = 128;
	const auto write = [&](size_t at, uint8_t value) {
		if (at < buffer.size()) buffer[at] = value;
	};
	for (int row = 0; row < height; ++row) {
		const size_t row_start = static_cast<size_t>(row) * static_cast<size_t>(width);
		int col = 0;
		if (bytes_per_line == 0) continue;
		do {
			const uint8_t byte = byte_at(pos++);
			if ((byte & 0xC0) == 0xC0) {
				const uint8_t value = byte_at(pos++);
				const int run = byte & 0x3F;
				for (int n = 0; n < run; ++n) write(row_start + static_cast<size_t>(col + n), value);
				col += run;
			} else {
				write(row_start + static_cast<size_t>(col++), byte);
			}
		} while (col < bytes_per_line);
	}
	// The luminance of each palette entry as the decode left it: (85 * (b + g + r)) >> 8
	// in 16 bits.
	uint8_t lum[256];
	for (size_t i = 0; i < 256; ++i) {
		const uint8_t *entry = &buffer[pixels + 4u * i];
		const uint16_t sum = static_cast<uint16_t>(entry[0] + entry[1] + entry[2]);
		lum[i] = static_cast<uint8_t>(static_cast<uint16_t>(85u * sum) >> 8);
	}
	out.width = width;
	out.height = height;
	out.pixels.resize(pixels * 4u);
	for (size_t i = 0; i < pixels; ++i) {
		const uint8_t idx = buffer[i];
		const uint8_t *entry = &buffer[pixels + 4u * idx];
		out.pixels[4 * i + 0] = entry[2];
		out.pixels[4 * i + 1] = entry[1];
		out.pixels[4 * i + 2] = entry[0];
		out.pixels[4 * i + 3] = lum[idx];
	}
	return true;
}

// [orig: load_pcx_to_argb @ 0x664cc0 — the header read (0x46 bytes), the BPP check
// (returns 3), width/height from the window, then the 8-bit path (Seek(-768, 2), the
// 0xFF000000 | rgb palette, the RLE loop to BytesPerLine) or the NPlanes == 3 path]
bool decode_pcx_menu_rgba(const uint8_t *data, size_t size, RgbaImage &out, std::string &error, PcxIndexed *indexed) {
	out = RgbaImage{};
	if (indexed) *indexed = PcxIndexed{};
	if (size < 0x46) {
		error = "PCX header truncated";
		return false;
	}
	if (data[3] != 8) {
		error = "PCX is not 8 bits per pixel";
		return false;
	}
	const int16_t xmin = static_cast<int16_t>(data[4] | (data[5] << 8));
	const int16_t ymin = static_cast<int16_t>(data[6] | (data[7] << 8));
	const int16_t xmax = static_cast<int16_t>(data[8] | (data[9] << 8));
	const int16_t ymax = static_cast<int16_t>(data[10] | (data[11] << 8));
	const int16_t width = static_cast<int16_t>(xmax - xmin + 1);
	const int16_t height = static_cast<int16_t>(ymax - ymin + 1);
	const int planes = data[0x41];
	const int bytes_per_line = data[0x42] | (data[0x43] << 8);
	if (width <= 0 || height <= 0) {
		error = "PCX dimensions out of range";
		return false;
	}
	const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	if (!pcx_pixels_fit(pixels, size)) {
		error = "PCX names more pixels than its data can describe";
		return false;
	}
	// ARGB words; the 8-bit path's row writes run to BytesPerLine (plus a run's
	// overshoot), so the scratch holds one row more than the image.
	std::vector<uint32_t> argb(pixels + static_cast<size_t>(bytes_per_line) + 64u, 0u);
	const auto byte_at = [&](size_t offset) -> uint8_t { return offset < size ? data[offset] : 0; };
	if (planes != 3) {
		uint32_t palette[256];
		const size_t palette_at = size >= 768 ? size - 768 : 0;
		for (int i = 0; i < 256; ++i) {
			palette[i] = 0xFF000000u | (static_cast<uint32_t>(byte_at(palette_at + 3u * i)) << 16) |
					(static_cast<uint32_t>(byte_at(palette_at + 3u * i + 1u)) << 8) |
					static_cast<uint32_t>(byte_at(palette_at + 3u * i + 2u));
		}
		// The indices beside the colours, where the caller asks them.
		std::vector<uint8_t> indices;
		if (indexed) {
			indexed->indexed = true;
			for (int i = 0; i < 256; ++i)
				for (int c = 0; c < 3; ++c) indexed->palette[i][c] = byte_at(palette_at + 3u * i + static_cast<size_t>(c));
			indices.assign(argb.size(), 0);
		}
		const auto put = [&](size_t at, uint8_t index) {
			if (at >= argb.size()) return;
			argb[at] = palette[index];
			if (indexed) indices[at] = index;
		};
		const size_t data_end = size >= 896 ? 128 + (size - 896) : 128;
		size_t pos = 128;
		const auto next = [&]() -> uint8_t { return pos < data_end ? data[pos++] : (++pos, 0); };
		for (int row = 0; row < height; ++row) {
			const size_t row_base = static_cast<size_t>(row) * static_cast<size_t>(width);
			int col = 0;
			if (bytes_per_line == 0) continue;
			do {
				const uint8_t byte = next();
				if ((byte & 0xC0) == 0xC0) {
					const int run = byte & 0x3F;
					const uint8_t index = next();
					for (int n = 0; n < run; ++n, ++col) put(row_base + static_cast<size_t>(col), index);
				} else {
					put(row_base + static_cast<size_t>(col++), byte);
				}
			} while (col < bytes_per_line);
		}
		if (indexed) {
			indices.resize(pixels);
			indexed->indices = std::move(indices);
		}
	} else {
		std::vector<uint8_t> scanline(static_cast<size_t>(3 * bytes_per_line) + 64u, 0);
		size_t pos = 128;
		const auto next = [&]() -> uint8_t { return pos < size ? data[pos++] : (++pos, 0); };
		for (int row = 0; row < height; ++row) {
			int col = 0;
			while (col < 3 * bytes_per_line) {
				const uint8_t byte = next();
				if ((byte & 0xC0) == 0xC0) {
					const int run = byte & 0x3F;
					const uint8_t value = next();
					for (int n = 0; n < run; ++n, ++col)
						if (static_cast<size_t>(col) < scanline.size()) scanline[static_cast<size_t>(col)] = value;
				} else {
					if (static_cast<size_t>(col) < scanline.size()) scanline[static_cast<size_t>(col)] = byte;
					++col;
				}
			}
			const size_t row_base = static_cast<size_t>(row) * static_cast<size_t>(width);
			for (int i = 0; i < width; ++i) {
				const auto plane = [&](int p) -> uint32_t {
					const size_t at = static_cast<size_t>(p) * static_cast<size_t>(width) + static_cast<size_t>(i);
					return at < scanline.size() ? scanline[at] : 0u;
				};
				argb[row_base + static_cast<size_t>(i)] = 0xFF000000u | (plane(0) << 16) | (plane(1) << 8) | plane(2);
			}
		}
	}
	out.width = width;
	out.height = height;
	out.pixels.resize(pixels * 4u);
	for (size_t i = 0; i < pixels; ++i) {
		const uint32_t c = argb[i];
		out.pixels[4 * i + 0] = static_cast<uint8_t>(c >> 16);
		out.pixels[4 * i + 1] = static_cast<uint8_t>(c >> 8);
		out.pixels[4 * i + 2] = static_cast<uint8_t>(c);
		out.pixels[4 * i + 3] = static_cast<uint8_t>(c >> 24);
	}
	return true;
}

// [orig: Texture_LoadPCXFromPFF32 @ 0x56EB31 — the 24-bit path, 3 * BytesPerLine RLE bytes a row, the
// planes `width` apart; load_pcx_to_argb @ 0x664cc0 likewise]
bool encode_pcx_rgb(const RgbImage &image, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	if (image.width <= 0 || image.height <= 0 || image.width > 0xFFFF || image.height > 0xFFFF) {
		error = "Invalid PCX dimensions";
		return false;
	}
	if (image.pixels.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3u) {
		error = "PCX colour payload too small";
		return false;
	}
	uint8_t header[128] = {};
	header[0] = 0x0A;
	header[1] = 5;
	header[2] = 1;
	header[3] = 8;
	header[8] = static_cast<uint8_t>((image.width - 1) & 0xFF);
	header[9] = static_cast<uint8_t>(((image.width - 1) >> 8) & 0xFF);
	header[10] = static_cast<uint8_t>((image.height - 1) & 0xFF);
	header[11] = static_cast<uint8_t>(((image.height - 1) >> 8) & 0xFF);
	header[12] = 72;
	header[14] = 72;
	header[65] = 3;
	header[66] = static_cast<uint8_t>(image.width & 0xFF);
	header[67] = static_cast<uint8_t>((image.width >> 8) & 0xFF);
	header[68] = 1;
	out.insert(out.end(), header, header + 128);
	std::vector<uint8_t> plane(static_cast<size_t>(image.width));
	for (int y = 0; y < image.height; ++y)
		for (int p = 0; p < 3; ++p) {
			for (int x = 0; x < image.width; ++x)
				plane[static_cast<size_t>(x)] = image.pixels[(static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 3u + static_cast<size_t>(p)];
			append_rle_row(plane.data(), image.width, out);
		}
	return true;
}

bool encode_pcx_indexed(const IndexedImage8 &image, std::vector<uint8_t> &out, std::string &error,
                        const PcxIndexedForm *form) {
	out.clear();

	const PcxIndexedForm shipped;
	const PcxIndexedForm &f = form != nullptr ? *form : shipped;
	if (image.width <= 0 || image.height <= 0 || image.width > 0xFFFF || image.height > 0xFFFF ||
	    f.window_x + image.width - 1 > 0xFFFF || f.window_y + image.height - 1 > 0xFFFF) {
		error = "Invalid PCX dimensions";
		return false;
	}
	if (image.indices.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) {
		error = "PCX indexed payload too small";
		return false;
	}

	// A row of exactly the image's width, odd or even: the game's readers decode BytesPerLine bytes a row at
	// a stride of the width, so a padded odd row spills into the next and the last past the buffer [orig:
	// Texture_LoadPCXFromPFF32 @ 0x56ED70..0x56EDFC; load_pcx_to_argb @ 0x664cc0], and every PCX the game
	// ships holds rows of its width.
	uint8_t header[128] = {};
	header[0] = 0x0A;
	header[1] = f.version;
	header[2] = 1;
	header[3] = 8;
	put_u16(header, 4, f.window_x);
	put_u16(header, 6, f.window_y);
	put_u16(header, 8, static_cast<uint16_t>(f.window_x + image.width - 1));
	put_u16(header, 10, static_cast<uint16_t>(f.window_y + image.height - 1));
	put_u16(header, 12, f.hdpi);
	put_u16(header, 14, f.vdpi);
	for (size_t entry = 0; entry < f.ega.size(); ++entry)
		std::copy(f.ega[entry].begin(), f.ega[entry].end(), header + 16 + entry * 3);
	header[64] = f.reserved;
	header[65] = 1;
	put_u16(header, 66, static_cast<uint16_t>(image.width));
	put_u16(header, 68, f.palette_info);
	put_u16(header, 70, f.hscreen);
	put_u16(header, 72, f.vscreen);
	// Bytes 74..127, the filler: zero, as every shipped map and MED's writer hold it.

	const std::vector<uint8_t> rows = coded_rows(image, f.coding);
	out.reserve(128 + rows.size() + 769);
	out.insert(out.end(), header, header + 128);
	out.insert(out.end(), rows.begin(), rows.end());
	out.push_back(0x0C);
	for (int i = 0; i < 256; ++i) {
		out.push_back(image.palette[i][0]);
		out.push_back(image.palette[i][1]);
		out.push_back(image.palette[i][2]);
	}

	return true;
}

} // namespace opennova
