// Generator + guard for fixtures/cbin:
//  * synth_nlist.kda, synth_nlist_jox01.kda, synth_nlist_bhd.kda — three
//    synthetic credits lists written by cbin::encode from authored rows, in
//    the shapes the shipped JO, JOX01 and BHD nlist.kda take (the BHD one
//    carries the top_y/bottom_y bounds); a fixed XOR key per file keeps the
//    bytes reproducible. The shipped trio is cbin_roundtrip's reference-tree leg.
//  * credits_image.png — the 8x8 RGB image the credits tests stage beside a
//    .kda under the name its ~F image row references (a self-contained PNG
//    writer with one stored zlib block, so no compressor is involved and every
//    platform mints the same bytes; the pixels are an integer gradient).
//  * particle_dot.tga — the 8x8 BGRA sprite the effect-world test's synthetic
//    particle file names as its layer texture (a soft white dot on transparent).
// No retail file is carried.
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <formats/cbin/cbin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace {

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 8;

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

uint32_t crc32_of(const uint8_t *data, size_t size) {
	static uint32_t table[256];
	static bool built = false;
	if (!built) {
		for (uint32_t n = 0; n < 256; ++n) {
			uint32_t c = n;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[n] = c;
		}
		built = true;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

void put_be32(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(static_cast<uint8_t>(v >> 24));
	out.push_back(static_cast<uint8_t>(v >> 16));
	out.push_back(static_cast<uint8_t>(v >> 8));
	out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data) {
	put_be32(out, static_cast<uint32_t>(data.size()));
	std::vector<uint8_t> crc_input(type, type + 4);
	crc_input.insert(crc_input.end(), data.begin(), data.end());
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data.begin(), data.end());
	put_be32(out, crc32_of(crc_input.data(), crc_input.size()));
}

std::vector<uint8_t> make_png() {
	std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

	std::vector<uint8_t> ihdr;
	put_be32(ihdr, kWidth);
	put_be32(ihdr, kHeight);
	ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
	put_chunk(out, "IHDR", ihdr);

	// Raw scanlines: filter byte 0 + RGB per pixel; an integer gradient.
	std::vector<uint8_t> raw;
	for (uint32_t y = 0; y < kHeight; ++y) {
		raw.push_back(0);
		for (uint32_t x = 0; x < kWidth; ++x) {
			raw.push_back(static_cast<uint8_t>(x * 32));
			raw.push_back(static_cast<uint8_t>(y * 32));
			raw.push_back(static_cast<uint8_t>(255 - (x + y) * 16));
		}
	}
	// zlib: header, one final stored block, adler32.
	std::vector<uint8_t> idat = {0x78, 0x01, 0x01};
	const uint16_t len = static_cast<uint16_t>(raw.size());
	idat.push_back(static_cast<uint8_t>(len & 0xFF));
	idat.push_back(static_cast<uint8_t>(len >> 8));
	idat.push_back(static_cast<uint8_t>(~len & 0xFF));
	idat.push_back(static_cast<uint8_t>((~len >> 8) & 0xFF));
	idat.insert(idat.end(), raw.begin(), raw.end());
	uint32_t a = 1, b = 0;
	for (uint8_t byte : raw) {
		a = (a + byte) % 65521u;
		b = (b + a) % 65521u;
	}
	put_be32(idat, (b << 16) | a);
	put_chunk(out, "IDAT", idat);
	put_chunk(out, "IEND", {});
	return out;
}

// An uncompressed 32-bit true-color TGA (image type 2, bottom-left origin,
// 8 alpha bits): the 18-byte header, then BGRA per pixel. The dot is a
// radial alpha falloff on white, so a blended particle draw has coverage.
std::vector<uint8_t> make_tga() {
	std::vector<uint8_t> out(18, 0);
	out[2] = 2;  // uncompressed true-color
	out[12] = static_cast<uint8_t>(kWidth & 0xFF);
	out[13] = static_cast<uint8_t>(kWidth >> 8);
	out[14] = static_cast<uint8_t>(kHeight & 0xFF);
	out[15] = static_cast<uint8_t>(kHeight >> 8);
	out[16] = 32;  // bits per pixel
	out[17] = 8;   // 8 alpha bits, bottom-left origin
	for (uint32_t y = 0; y < kHeight; ++y) {
		for (uint32_t x = 0; x < kWidth; ++x) {
			// Integer radial falloff from the center (3.5, 3.5): 255 at the
			// middle four texels, 0 at the corners.
			const int dx = static_cast<int>(x) * 2 - 7;
			const int dy = static_cast<int>(y) * 2 - 7;
			const int distance_sq = dx * dx + dy * dy;  // 2 .. 98
			int alpha = 255 - (distance_sq - 2) * 255 / 96;
			if (alpha < 0) alpha = 0;
			out.push_back(255);  // B
			out.push_back(255);  // G
			out.push_back(255);  // R
			out.push_back(static_cast<uint8_t>(alpha));
		}
	}
	return out;
}

using test_io::read_file;

// 0 = byte-identical (or written), 1 = mismatch/missing.
int guard(const std::string &path, const std::vector<uint8_t> &bytes, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str())) return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str())) return 1;
	std::printf("OK: %s byte-reproducible (%zu bytes)\n", path.c_str(), bytes.size());
	return 0;
}

// --- the credits lists -----------------------------------------------------

using opennova::cbin::Credits;
using opennova::cbin::Entry;
using opennova::cbin::EntryType;
using opennova::cbin::Justify;

Entry text_with_font(const std::string &text, const std::string &font) {
	Entry e = Entry::make_text(text, font);
	e.binary_type = 2; // the text + font pair element
	return e;
}

// A credits list: a title block (centered, a colour, the logo image row the
// tests resolve as cr_logo.png, the title in the Synth24 font the tests
// resolve as Synth24.fnt), then `sections` credit sections of a heading and
// two names each; every section re-colours and the last one right-justifies.
// `sections` sets the text-with-font count: 1 + 3 * sections.
Credits make_credits(const char *title, int sections, uint32_t xor_key) {
	Credits c;
	c.scroll_rate = 0.5f;
	c.vertical_space = 14;
	c.center_x = 400;
	c.xor_key = xor_key;
	c.entries.push_back(Entry::make_justify(Justify::Center));
	c.entries.push_back(Entry::make_color(0xFFFFD0));
	c.entries.push_back(Entry::make_image("cr_logo.png", 300, 20));
	c.entries.push_back(Entry::make_newline());
	c.entries.push_back(text_with_font(title, "Synth24"));
	c.entries.push_back(Entry::make_newline());
	static const char *const kHeadings[] = {"ENGINE", "FORMATS", "NETWORK", "AUDIO", "TERRAIN",
	                                        "MENUS", "TOOLS", "TESTING", "DOCS", "THANKS"};
	for (int s = 0; s < sections; ++s) {
		c.entries.push_back(Entry::make_color(0x80C0FF - 0x100000 * (s % 4)));
		if (s == sections - 1) c.entries.push_back(Entry::make_justify(Justify::Right));
		c.entries.push_back(text_with_font(kHeadings[s % 10], "Synth24"));
		c.entries.push_back(Entry::make_newline());
		c.entries.push_back(text_with_font("Contributor " + std::to_string(2 * s + 1), "Synth16"));
		c.entries.push_back(Entry::make_newline());
		c.entries.push_back(text_with_font("Contributor " + std::to_string(2 * s + 2), "Synth16"));
		c.entries.push_back(Entry::make_newline());
	}
	return c;
}

// Encode a list, prove the bytes decode back to the same entry sequence and
// re-encode identically (with the string table the decoder recovered), and
// return them.
bool build_credits(const Credits &credits, std::vector<uint8_t> &bytes, std::string &err) {
	if (!opennova::cbin::encode(credits, bytes, err)) return false;
	Credits back;
	if (!opennova::cbin::decode_credits(bytes.data(), bytes.size(), back, err)) return false;
	if (back.entries.size() != credits.entries.size()) {
		err = "the decoded list lost entries";
		return false;
	}
	size_t with_font = 0;
	for (const Entry &e : back.entries)
		if (e.type == EntryType::Text && !e.font.empty()) ++with_font;
	size_t expected_with_font = 0;
	for (const Entry &e : credits.entries)
		if (e.type == EntryType::Text && !e.font.empty()) ++expected_with_font;
	if (with_font != expected_with_font || back.has_bhd_bounds() != credits.has_bhd_bounds()) {
		err = "the decoded list lost a font name or the BHD bounds";
		return false;
	}
	std::vector<uint8_t> again;
	if (!opennova::cbin::encode(back, again, err)) return false;
	if (again != bytes) {
		err = "encode(decode(encode(credits))) is not byte-stable";
		return false;
	}
	return true;
}

int guard_credits(const std::string &path, const Credits &credits, bool write_mode) {
	std::vector<uint8_t> bytes;
	std::string err;
	if (!expect(build_credits(credits, bytes, err), (path + ": " + err).c_str())) return 1;
	return guard(path, bytes, write_mode);
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/cbin";
	int failures = 0;
	failures += guard(dir + "/credits_image.png", make_png(), write_mode);
	failures += guard(dir + "/particle_dot.tga", make_tga(), write_mode);
	// JO: 8 sections (25 text entries with fonts, the count kda_load_test pins).
	failures += guard_credits(dir + "/synth_nlist.kda", make_credits("OPENNOVA", 8, 0x5EED0001u), write_mode);
	// JOX01: the expansion's longer list.
	failures += guard_credits(dir + "/synth_nlist_jox01.kda", make_credits("OPENNOVA JOX", 10, 0x5EED0002u),
	                          write_mode);
	// BHD: the top_y/bottom_y viewport bounds the BHD-era encoding adds.
	Credits bhd = make_credits("OPENNOVA BHD", 6, 0x5EED0003u);
	bhd.has_top_y = true;
	bhd.top_y = 60;
	bhd.has_bottom_y = true;
	bhd.bottom_y = 580;
	failures += guard_credits(dir + "/synth_nlist_bhd.kda", bhd, write_mode);
	return failures == 0 ? 0 : 1;
}
