#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

#include <cstdio>
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

// An indexed PCX's carried form (PcxIndexedForm): the header bytes the image does not decide, and the coding
// of the rows, the shipped coder's or JO's mission editor's [orig: JOTACmed.exe sub_45EB60, sub_45E8A0].
int test_forms() {
	// Rows that tell the coders apart: a row ending in a single byte below 0xC0, a row ending in a run, a row
	// holding a run of 70 (63 + 7), a lone 0xC5.
	opennova::IndexedImage8 image;
	image.width = 80;
	image.height = 3;
	image.indices.assign(size_t(80) * 3, 0);
	for (int x = 0; x < 80; ++x) image.indices[size_t(x)] = static_cast<uint8_t>(x % 7);
	for (int x = 0; x < 80; ++x) image.indices[size_t(80 + x)] = x < 10 ? 0xC5 : 4;
	for (int x = 0; x < 80; ++x) image.indices[size_t(160 + x)] = x < 70 ? 9 : static_cast<uint8_t>(x);
	image.palette[9][1] = 200;
	std::string error;

	// The default form is the shipped one, the bytes the writer has always written.
	std::vector<uint8_t> shipped;
	if (!expect(opennova::encode_pcx_indexed(image, shipped, error), "the shipped form should encode")) return 1;
	if (!expect(shipped[1] == 5 && shipped[2] == 1 && shipped[3] == 8 && shipped[12] == 72 && shipped[14] == 72 &&
	            shipped[16] == 0xFF && shipped[63] == 0xFF && shipped[64] == 0 && shipped[65] == 1 &&
	            shipped[66] == 80 && shipped[68] == 1 && shipped[74] == 0,
	            "the shipped header: version 5, 72 dpi, the EGA bytes 0xFF, one plane, the width a line")) return 1;
	// Row 0 ends in the single byte 2 (79 % 7 = 2): written alone.
	opennova::IndexedImage8 back;
	opennova::PcxIndexedForm form;
	if (!expect(opennova::decode_pcx_indexed(shipped.data(), shipped.size(), back, error, &form),
	            "the shipped bytes should decode")) return 1;
	if (!expect(back.indices == image.indices && form.coding == opennova::PcxCoding::Shipped && form.hdpi == 72,
	            "a shipped file reads as the shipped coding")) return 1;

	// MED's form: the header zero but for 0A 05 01 08, the window, one plane, the bytes a line, palette info 1;
	// each row's last run a count pair.
	const opennova::PcxIndexedForm med_form = opennova::pcx_med_form();
	std::vector<uint8_t> med;
	if (!expect(opennova::encode_pcx_indexed(image, med, error, &med_form), "MED's form should encode")) return 1;
	bool header_zero = true;
	for (size_t i = 12; i < 128; ++i)
		if (i != 65 && i != 66 && i != 68 && med[i] != 0) header_zero = false;
	if (!expect(med[0] == 0x0A && med[1] == 5 && med[2] == 1 && med[3] == 8 && med[8] == 79 && med[10] == 2 &&
	            med[65] == 1 && med[66] == 80 && med[68] == 1 && header_zero,
	            "MED's header: zero but the mark, the window, one plane, the bytes a line, palette info 1")) return 1;
	// Row 0's last byte (79 % 7 = 2) is a single byte below 0xC0: shipped writes 0x02, MED 0xC1 0x02.
	const size_t shipped_row0 = [&] {
		size_t pos = 128, got = 0;
		while (got < 80) {
			const uint8_t b = shipped[pos++];
			if ((b & 0xC0) == 0xC0) { got += b & 0x3F; ++pos; } else ++got;
		}
		return pos;
	}();
	if (!expect(shipped[shipped_row0 - 1] == 2 && shipped[shipped_row0 - 2] != 0xC1, "shipped writes the last byte alone")) return 1;
	if (!expect(med.size() == shipped.size() + 2, "MED's rows cost a count byte where a row ends in a lone byte")) return 1;
	if (!expect(med[shipped_row0 - 1] == 0xC1 && med[shipped_row0] == 2, "MED writes the last byte as a pair")) return 1;
	if (!expect(opennova::decode_pcx_indexed(med.data(), med.size(), back, error, &form), "MED's bytes should decode")) return 1;
	if (!expect(back.indices == image.indices && form.coding == opennova::PcxCoding::Med && form.hdpi == 0 &&
	            form.ega[0][0] == 0, "a MED file reads as MED's coding and header")) return 1;
	std::vector<uint8_t> again;
	if (!expect(opennova::encode_pcx_indexed(back, again, error, &form) && again == med,
	            "a MED file written again in its form is the same bytes")) return 1;

	// Every carried byte comes back.
	opennova::PcxIndexedForm odd;
	odd.version = 3;
	odd.window_x = 4;
	odd.window_y = 6;
	odd.hdpi = 300;
	odd.vdpi = 150;
	for (size_t i = 0; i < odd.ega.size(); ++i)
		for (size_t c = 0; c < 3; ++c) odd.ega[i][c] = static_cast<uint8_t>((i * 3 + c) * 5);
	odd.reserved = 7;
	odd.palette_info = 2;
	odd.hscreen = 640;
	odd.vscreen = 480;
	std::vector<uint8_t> carried;
	if (!expect(opennova::encode_pcx_indexed(image, carried, error, &odd), "an odd form should encode")) return 1;
	opennova::PcxIndexedForm read;
	if (!expect(opennova::decode_pcx_indexed(carried.data(), carried.size(), back, error, &read) &&
	            back.width == 80 && back.height == 3, "an odd form should decode at its size")) return 1;
	if (!expect(read.version == 3 && read.window_x == 4 && read.window_y == 6 && read.hdpi == 300 && read.vdpi == 150 &&
	            read.ega == odd.ega && read.reserved == 7 && read.palette_info == 2 && read.hscreen == 640 &&
	            read.vscreen == 480 && read.coding == opennova::PcxCoding::Shipped,
	            "every carried header byte reads back")) return 1;
	if (!expect(opennova::encode_pcx_indexed(back, again, error, &read) && again == carried,
	            "an odd form written again is the same bytes")) return 1;
	// The filler (bytes 74..127) is written zero and not carried: a file holding other bytes there is read, and
	// written again with zeros.
	bool filler_zero = true;
	for (size_t i = 74; i < 128; ++i) filler_zero = filler_zero && carried[i] == 0;
	if (!expect(filler_zero, "the filler is written zero")) return 1;
	std::vector<uint8_t> filled = carried;
	filled[100] = 9;
	if (!expect(opennova::decode_pcx_indexed(filled.data(), filled.size(), back, error, &read) &&
	            opennova::encode_pcx_indexed(back, again, error, &read) && again == carried,
	            "a non-zero filler is written again as zero")) return 1;

	// Rows neither coder writes: a run split in two (2 x 0xC2 0x05 for four 5s) reads as Other and is written
	// again in the shipped coding.
	opennova::IndexedImage8 four;
	four.width = 4;
	four.height = 1;
	four.indices = {5, 5, 5, 5};
	std::vector<uint8_t> split;
	if (!expect(opennova::encode_pcx_indexed(four, split, error), "four 5s should encode")) return 1;
	if (!expect(split[128] == 0xC4 && split[129] == 5, "four 5s are one run")) return 1;
	split.erase(split.begin() + 128, split.begin() + 130);
	const uint8_t halves[4] = {0xC2, 5, 0xC2, 5};
	split.insert(split.begin() + 128, halves, halves + 4);
	if (!expect(opennova::decode_pcx_indexed(split.data(), split.size(), back, error, &read) &&
	            back.indices == four.indices && read.coding == opennova::PcxCoding::Other,
	            "a split run reads as another program's coding")) return 1;
	if (!expect(opennova::encode_pcx_indexed(back, again, error, &read) && again.size() == split.size() - 2 &&
	            again[128] == 0xC4, "another program's rows are written again in the shipped coding")) return 1;

	// Rows both coders write alike (each ending in a run): the header decides, MED's zero resolution MED's.
	std::vector<uint8_t> tie;
	if (!expect(opennova::encode_pcx_indexed(four, tie, error, &med_form), "four 5s in MED's form should encode")) return 1;
	if (!expect(opennova::decode_pcx_indexed(tie.data(), tie.size(), back, error, &read) &&
	            read.coding == opennova::PcxCoding::Med, "alike rows under MED's header read as MED's")) return 1;
	if (!expect(opennova::encode_pcx_indexed(four, tie, error) &&
	            opennova::decode_pcx_indexed(tie.data(), tie.size(), back, error, &read) &&
	            read.coding == opennova::PcxCoding::Shipped, "alike rows under the shipped header read as shipped")) return 1;
	// A header that differs from MED's in any field (here only the resolution: 0 as MED's, but an EGA entry set)
	// is not MED's header: alike rows under it read as shipped.
	opennova::PcxIndexedForm near_med = med_form;
	near_med.ega[3][1] = 1;
	if (!expect(opennova::encode_pcx_indexed(four, tie, error, &near_med) &&
	            opennova::decode_pcx_indexed(tie.data(), tie.size(), back, error, &read) &&
	            read.coding == opennova::PcxCoding::Shipped, "alike rows under a header not MED's read as shipped")) return 1;

	// The row tails that tell the coders apart. `tail` holds one row's coded bytes as each coder writes it.
	const auto row_bytes = [&](const opennova::IndexedImage8 &one, const opennova::PcxIndexedForm *as) {
		std::vector<uint8_t> file;
		if (!opennova::encode_pcx_indexed(one, file, error, as)) return std::vector<uint8_t>();
		return std::vector<uint8_t>(file.begin() + 128, file.end() - 769);
	};
	const auto row_of = [](std::vector<uint8_t> indices) {
		opennova::IndexedImage8 one;
		one.width = static_cast<int>(indices.size());
		one.height = 1;
		one.indices = std::move(indices);
		return one;
	};
	using Bytes = std::vector<uint8_t>;
	// 64 equal bytes at a row's end: a full run of 63, then MED's pair of 1, the shipped coder's byte alone.
	const opennova::IndexedImage8 sixty_four = row_of(Bytes(64, 7));
	if (!expect(row_bytes(sixty_four, nullptr) == Bytes({0xFF, 7, 7}) &&
	            row_bytes(sixty_four, &med_form) == Bytes({0xFF, 7, 0xC1, 7}), "64 equal bytes at a row's end")) return 1;
	// 63 at its end: one full run, alike.
	const opennova::IndexedImage8 sixty_three = row_of(Bytes(63, 7));
	if (!expect(row_bytes(sixty_three, nullptr) == Bytes({0xFF, 7}) && row_bytes(sixty_three, &med_form) == Bytes({0xFF, 7}),
	            "63 equal bytes at a row's end")) return 1;
	// A one-pixel row: MED's pair, the shipped byte alone.
	const opennova::IndexedImage8 one_pixel = row_of(Bytes({7}));
	if (!expect(row_bytes(one_pixel, nullptr) == Bytes({7}) && row_bytes(one_pixel, &med_form) == Bytes({0xC1, 7}),
	            "a one-pixel row")) return 1;
	// A lone byte of 0xC0 or more at the tail: a pair in both.
	const opennova::IndexedImage8 high_tail = row_of(Bytes({7, 7, 0xC8}));
	if (!expect(row_bytes(high_tail, nullptr) == Bytes({0xC2, 7, 0xC1, 0xC8}) &&
	            row_bytes(high_tail, &med_form) == Bytes({0xC2, 7, 0xC1, 0xC8}), "a lone high byte at a row's end")) return 1;
	// An odd width: the bytes a line are the width (no pad byte), as MED writes them; each row read at that stride.
	opennova::IndexedImage8 odd_width;
	odd_width.width = 3;
	odd_width.height = 2;
	odd_width.indices = {1, 2, 3, 4, 5, 6};
	for (const opennova::PcxIndexedForm *as : {static_cast<const opennova::PcxIndexedForm *>(nullptr), &med_form}) {
		std::vector<uint8_t> file;
		if (!expect(opennova::encode_pcx_indexed(odd_width, file, error, as) && file[66] == 3 && file[67] == 0,
		            "an odd width's bytes a line are its width")) return 1;
		if (!expect(opennova::decode_pcx_indexed(file.data(), file.size(), back, error, &read) &&
		            back.indices == odd_width.indices && opennova::encode_pcx_indexed(back, again, error, &read) &&
		            again == file, "an odd width reads and writes again the same bytes")) return 1;
	}

	std::printf("OK: the indexed form carries every header byte and the rows' coding, MED's coder ported\n");
	return 0;
}

} // namespace

int main() {
	if (test_forms() != 0) return 1;

	opennova::IndexedImage8 source;
	source.width = 4;
	source.height = 2;
	source.indices = {
		0, 1, 2, 3,
		3, 2, 1, 0,
	};
	source.palette[0][0] = 0;
	source.palette[0][1] = 0;
	source.palette[0][2] = 0;
	source.palette[1][0] = 255;
	source.palette[1][1] = 0;
	source.palette[1][2] = 0;
	source.palette[2][0] = 0;
	source.palette[2][1] = 255;
	source.palette[2][2] = 0;
	source.palette[3][0] = 0;
	source.palette[3][1] = 0;
	source.palette[3][2] = 255;

	std::vector<uint8_t> encoded;
	std::string error;
	if (!opennova::encode_pcx_indexed(source, encoded, error)) {
		std::fprintf(stderr, "FAIL: encode_pcx_indexed failed: %s\n", error.c_str());
		return 1;
	}

	opennova::IndexedImage8 indexed;
	error.clear();
	if (!opennova::decode_pcx_indexed(encoded.data(), encoded.size(), indexed, error)) {
		std::fprintf(stderr, "FAIL: decode_pcx_indexed failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(indexed.width == source.width, "indexed width should round-trip")) return 1;
	if (!expect(indexed.height == source.height, "indexed height should round-trip")) return 1;
	if (!expect(indexed.indices == source.indices, "indexed pixels should round-trip")) return 1;
	if (!expect(indexed.palette[1][0] == 255 && indexed.palette[2][1] == 255 && indexed.palette[3][2] == 255,
	            "palette entries should round-trip")) return 1;

	opennova::RgbImage rgb;
	error.clear();
	if (!opennova::decode_pcx_rgb(encoded.data(), encoded.size(), rgb, error)) {
		std::fprintf(stderr, "FAIL: decode_pcx_rgb failed: %s\n", error.c_str());
		return 1;
	}

	if (!expect(rgb.width == source.width, "rgb width should match source")) return 1;
	if (!expect(rgb.height == source.height, "rgb height should match source")) return 1;
	if (!expect(rgb.pixels.size() == static_cast<size_t>(source.width * source.height * 3),
	            "rgb pixel payload should match source dimensions")) return 1;
	if (!expect(rgb.pixels[3] == 255 && rgb.pixels[4] == 0 && rgb.pixels[5] == 0,
	            "palette index 1 should decode to red")) return 1;

	// A 24-bit PCX (three planes of colour) of an odd width round-trips its colours, and reads as no
	// indexed image.
	opennova::RgbImage colours;
	colours.width = 3;
	colours.height = 2;
	for (int i = 0; i < colours.width * colours.height; ++i)
		colours.pixels.insert(colours.pixels.end(), {uint8_t(i * 40), uint8_t(200 - i * 30), uint8_t(i * 7 + 1)});
	std::vector<uint8_t> planes;
	error.clear();
	if (!expect(opennova::encode_pcx_rgb(colours, planes, error), "encode_pcx_rgb should write three planes")) return 1;
	if (!expect(planes.size() > 128 && planes[65] == 3, "the header should say three planes")) return 1;
	opennova::RgbImage back;
	if (!expect(opennova::decode_pcx_rgb(planes.data(), planes.size(), back, error), "the 24-bit PCX should decode")) return 1;
	if (!expect(back.width == 3 && back.height == 2 && back.pixels == colours.pixels, "24-bit colours should round-trip"))
		return 1;
	opennova::IndexedImage8 none;
	if (!expect(!opennova::decode_pcx_indexed(planes.data(), planes.size(), none, error),
	            "a 24-bit PCX is no indexed image"))
		return 1;

	std::printf("OK: pcx indexed/rgb round-trip preserved payload and palette; 24-bit planes round-trip\n");
	return 0;
}
