// Generator + guard for fixtures/cbin:
//  * synth_nlist.kda, synth_nlist_jox01.kda, synth_nlist_bhd.kda — three
//    synthetic credits lists written by cbin::encode_binary_config from authored rows, in
//    the shapes the shipped JO, JOX01 and BHD nlist.kda take (the BHD one
//    carries the top_y/bottom_y bounds); a fixed XOR key per file keeps the
//    bytes reproducible.
//  * synth_nlist_controls.kda, synth_nlist_bhd_plain.kda — the shipped lists'
//    own shapes beyond those three (JO's and JOX01's control runs and images,
//    BHD's plain lines, its ~BINK_LOGO markup and bounds), in the shipped
//    encoder's key: the variants the retired cbin_roundtrip_retail and
//    menu_credits_retail rows read from the shipped trio.
//  * credits_image.png — the 8x8 RGB image the credits tests stage beside a
//    .kda under the name its ~F image row references (the tests' PNG writer,
//    common/png_test_support.h, with one stored zlib block, so no compressor is
//    involved and every platform mints the same bytes; the pixels are an
//    integer gradient).
//  * particle_dot.tga — the 8x8 BGRA sprite the effect-world test's synthetic
//    particle file names as its layer texture (a soft white dot on transparent).
// No retail file is carried.
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <formats/cbin/binary_config.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/png_test_support.h"

namespace {

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 8;

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

// The credits image: 8-bit RGB, no interlace, each row behind filter 0, its pixels an
// integer gradient, written by the shared stored-deflate PNG writer.
std::vector<uint8_t> make_png() {
	test_png::PngSpec spec;
	spec.width = kWidth;
	spec.height = kHeight;
	spec.color_type = 2; // RGB
	for (uint32_t y = 0; y < kHeight; ++y) {
		spec.rows.push_back(0);
		for (uint32_t x = 0; x < kWidth; ++x) {
			spec.rows.push_back(static_cast<uint8_t>(x * 32));
			spec.rows.push_back(static_cast<uint8_t>(y * 32));
			spec.rows.push_back(static_cast<uint8_t>(255 - (x + y) * 16));
		}
	}
	return test_png::make_png(spec);
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

// A credits list as authored rows: the [ENV] words and the [TEXT] lines, each a text with its font
// when it names one (a text line's font, a markup line's), else the line alone (a control code, a
// line end, an image).
struct Credits {
	float scroll_rate = 0.5f;
	int vertical_space = 14;
	int center_x = 400;
	bool has_top_y = false;
	int top_y = 0;
	bool has_bottom_y = false;
	int bottom_y = 0;
	uint32_t xor_key = 0;
	struct Line {
		std::string text;
		std::string font;
	};
	std::vector<Line> lines;
};

enum class Justify { Left, Center, Right };

Credits::Line text_with_font(const std::string &text, const std::string &font) {
	return {text, font};
}

Credits::Line make_justify(Justify j) {
	return {j == Justify::Left ? "~JL" : j == Justify::Right ? "~JR" : "~JC", ""};
}

Credits::Line make_color(uint32_t rgb) {
	char buf[16];
	std::snprintf(buf, sizeof(buf), "~C%06X", rgb & 0xFFFFFFu);
	return {buf, ""};
}

Credits::Line make_image(const std::string &path, int x, int y) {
	return {"~F" + std::to_string(x) + "|" + std::to_string(y) + "|" + path, ""};
}

Credits::Line make_simple_image(const std::string &path) {
	return {"~I" + path, ""};
}

Credits::Line make_newline() {
	return {"<CR>", ""};
}

// The list in the CBIN form, its string table in the order the game's writer interns it, each string
// at its first use as the writer lays the form out: the label names, then the entry names, then the
// string values [orig: the ConfigFile writer @0x75e170 - its intern ConfigFile_InternString called @0x75e3a0
// (the labels), @0x75e450 (the names), @0x75e53f (the values)].
opennova::cbin::BinaryConfig credits_config(const Credits &c) {
	using opennova::cbin::BinaryConfig;
	BinaryConfig config;
	config.xor_key = c.xor_key;
	std::map<std::string, uint32_t> index;
	const auto add = [&](const std::string &text) -> uint32_t {
		const auto it = index.find(text);
		if (it != index.end()) return it->second;
		config.strings.push_back(text);
		const uint32_t at = static_cast<uint32_t>(config.strings.size());
		index[text] = at;
		return at;
	};
	BinaryConfig::Label env;
	BinaryConfig::Label text;
	env.name = add("env");
	text.name = add("text");
	const auto number = [&](const char *name, uint32_t raw, uint32_t flags) {
		BinaryConfig::Entry entry;
		entry.name = add(name);
		entry.values.push_back({raw, flags});
		env.entries.push_back(entry);
	};
	uint32_t scroll_bits = 0;
	std::memcpy(&scroll_bits, &c.scroll_rate, sizeof scroll_bits);
	number("scroll_rate", scroll_bits, BinaryConfig::kFloat);
	number("vertical_space", static_cast<uint32_t>(c.vertical_space), BinaryConfig::kInteger);
	number("center_x", static_cast<uint32_t>(c.center_x), BinaryConfig::kInteger);
	if (c.has_top_y) number("top_y", static_cast<uint32_t>(c.top_y), BinaryConfig::kInteger);
	if (c.has_bottom_y) number("bottom_y", static_cast<uint32_t>(c.bottom_y), BinaryConfig::kInteger);
	const uint32_t line_name = add("text");
	for (const Credits::Line &line : c.lines) {
		BinaryConfig::Entry entry;
		entry.name = line_name;
		entry.values.push_back({add(line.text), BinaryConfig::kString});
		if (!line.font.empty()) entry.values.push_back({add(line.font), BinaryConfig::kString});
		text.entries.push_back(entry);
	}
	config.labels = {env, text};
	return config;
}

// A credits list: a title block (centered, a colour, the logo image row the
// tests resolve as cr_logo.png, the title in the Synth24 font the tests
// resolve as Synth24.fnt), then `sections` credit sections of a heading and
// two names each; every section re-colours and the last one right-justifies.
// `sections` sets the text-with-font count: 1 + 3 * sections.
Credits make_credits(const char *title, int sections, uint32_t xor_key) {
	Credits c;
	c.xor_key = xor_key;
	c.lines.push_back(make_justify(Justify::Center));
	c.lines.push_back(make_color(0xFFFFD0));
	c.lines.push_back(make_image("cr_logo.png", 300, 20));
	c.lines.push_back(make_newline());
	c.lines.push_back(text_with_font(title, "Synth24"));
	c.lines.push_back(make_newline());
	static const char *const kHeadings[] = {"ENGINE", "FORMATS", "NETWORK", "AUDIO", "TERRAIN",
	                                        "MENUS", "TOOLS", "TESTING", "DOCS", "THANKS"};
	for (int s = 0; s < sections; ++s) {
		c.lines.push_back(make_color(0x80C0FF - 0x100000 * (s % 4)));
		if (s == sections - 1) c.lines.push_back(make_justify(Justify::Right));
		c.lines.push_back(text_with_font(kHeadings[s % 10], "Synth24"));
		c.lines.push_back(make_newline());
		c.lines.push_back(text_with_font("Contributor " + std::to_string(2 * s + 1), "Synth16"));
		c.lines.push_back(make_newline());
		c.lines.push_back(text_with_font("Contributor " + std::to_string(2 * s + 2), "Synth16"));
		c.lines.push_back(make_newline());
	}
	return c;
}

// The XOR key the retail encoder wrote all three shipped lists with (JO's, JOX01's and BHD's nlist.kda).
constexpr uint32_t kShippedKey = 0x01E177CEu;

// The JO and JOX01 lists' own shape, beyond make_credits' (the shipped pair, read once on 2026-10-19 by
// the retired cbin_roundtrip_retail row): every word joined by '_' (the text form carries no space), a
// run of two controls at the head (~JR, then ~CFF0000), controls ahead of a line end or an image and not
// only of a line, fading ~F images at 0|0 among the lines (17 in each) and two ~I spacing images at the
// end, every line a text with its font, the shipped encoder's key.
Credits make_controls_credits() {
	Credits c;
	c.xor_key = kShippedKey;
	c.lines.push_back(make_justify(Justify::Right));
	c.lines.push_back(make_color(0xFF0000));
	c.lines.push_back(text_with_font("Open_Nova:", "Synth24"));
	c.lines.push_back(text_with_font("Combined_Shapes", "Synth24"));
	c.lines.push_back(make_newline());
	c.lines.push_back(make_newline());
	c.lines.push_back(text_with_font("Contributors", "Synth16"));
	c.lines.push_back(make_color(0xFFFFFF));
	c.lines.push_back(make_image("synth1.png", 0, 0));
	for (int i = 0; i < 3; ++i) c.lines.push_back(make_newline());
	static const char *const kHeadings[] = {"Engine_Team", "Format_Team", "Network_Team"};
	for (int s = 0; s < 3; ++s) {
		c.lines.push_back(make_color(0xFF0000));
		c.lines.push_back(text_with_font(kHeadings[s], "Synth24"));
		c.lines.push_back(make_color(0xFFFFFF));
		c.lines.push_back(make_newline());
		c.lines.push_back(text_with_font("Contributor_" + std::to_string(2 * s + 1), "Synth16"));
		c.lines.push_back(text_with_font("Contributor_" + std::to_string(2 * s + 2), "Synth16"));
		c.lines.push_back(make_newline());
		c.lines.push_back(make_image("synth" + std::to_string(s + 2) + ".png", 0, 0));
		c.lines.push_back(make_newline());
	}
	c.lines.push_back(make_justify(Justify::Center));
	c.lines.push_back(make_simple_image("synth_a.tga"));
	c.lines.push_back(make_newline());
	c.lines.push_back(make_simple_image("synth_b.tga"));
	return c;
}

// BHD's list (nlist.bhd.reference.kda, read once on 2026-10-19): no control code at all (no ~C, ~J, ~F
// or ~I), every line a text with its font, line ends, one ~BINK_LOGO markup line with a font, which
// draws nothing; a wider line spacing (vertical_space 20) and its viewport bounds, top_y 66 and
// bottom_y 588, the shipped encoder's key.
Credits make_bhd_plain_credits() {
	Credits c;
	c.vertical_space = 20;
	c.has_top_y = true;
	c.top_y = 66;
	c.has_bottom_y = true;
	c.bottom_y = 588;
	c.xor_key = kShippedKey;
	c.lines.push_back(text_with_font("OpenNova_Black_Hawk_Shapes", "Synth36"));
	c.lines.push_back(make_newline());
	c.lines.push_back(make_newline());
	static const char *const kHeadings[] = {"Lead_Engine", "Formats", "Network"};
	for (int s = 0; s < 3; ++s) {
		c.lines.push_back(text_with_font(kHeadings[s], "Synth24"));
		c.lines.push_back(make_newline());
		for (int n = 0; n < 3; ++n)
			c.lines.push_back(text_with_font("Contributor_" + std::to_string(3 * s + n + 1), "Synth16"));
		c.lines.push_back(make_newline());
	}
	c.lines.push_back(text_with_font("~BINK_LOGO", "Synth16"));
	c.lines.push_back(make_newline());
	c.lines.push_back(text_with_font("*", "Synth16"));
	return c;
}

// Write a list in the form, prove the bytes read back through the form's reader and write back the same,
// and return them.
bool build_credits(const Credits &credits, std::vector<uint8_t> &bytes, std::string &err) {
	using opennova::cbin::BinaryConfig;
	if (!opennova::cbin::encode_binary_config(credits_config(credits), bytes, err)) return false;
	BinaryConfig back;
	if (!opennova::cbin::decode_binary_config(bytes.data(), bytes.size(), back, err)) return false;
	if (back.labels.size() != 2 || back.labels[1].entries.size() != credits.lines.size()) {
		err = "the read-back list lost lines";
		return false;
	}
	std::vector<uint8_t> again;
	if (!opennova::cbin::encode_binary_config(back, again, err)) return false;
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
	// The shipped lists' own shapes: JO's and JOX01's controls and images, BHD's plain lines.
	failures += guard_credits(dir + "/synth_nlist_controls.kda", make_controls_credits(), write_mode);
	failures += guard_credits(dir + "/synth_nlist_bhd_plain.kda", make_bhd_plain_credits(), write_mode);
	return failures == 0 ? 0 : 1;
}
