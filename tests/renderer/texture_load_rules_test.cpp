// The game's texture loaders as rules (renderer/texture_load_rules.h): the file each
// loader opens and the reader it names, no alternate name ever, the transforms the
// loaders make, the normal-map cap's halving, and the PCX reader every loader uses.

#include <formats/pcx/pcx_io.h>
#include <runtime/renderer/texture_load_rules.h>

#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace opennova::renderer;

namespace {

int failures = 0;

#define CHECK(cond, msg)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);       \
			++failures;                                                        \
		}                                                                      \
	} while (0)

// A mounted set: `files` exist (case-insensitively), `loose` are the loose-first hits.
TextureFileQuery mounted(std::set<std::string> files, std::set<std::string> loose = {}) {
	const auto lower = [](std::string s) {
		for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	};
	std::set<std::string> lowered, loose_lowered;
	for (const std::string &f : files) lowered.insert(lower(f));
	for (const std::string &f : loose) loose_lowered.insert(lower(f));
	TextureFileQuery query;
	query.exists = [lowered, lower](const std::string &name) { return lowered.count(lower(name)) != 0; };
	query.loose_first_hit = [loose_lowered, lower](const std::string &name) {
		return loose_lowered.count(lower(name)) != 0;
	};
	return query;
}

TextureLoad only(TextureLoader loader, const std::string &name, const TextureFileQuery &files) {
	const std::vector<TextureLoad> attempts = texture_load_attempts(loader, name, files);
	return attempts.size() == 1 ? attempts[0] : TextureLoad{};
}

void test_no_alternate_names() {
	// No loader reads an _O twin or a png/jpg/bmp beside the named file.
	const TextureFileQuery files = mounted({"trim_O.tga", "trim.png", "trim.jpg", "trim.bmp"});
	for (TextureLoader loader : {TextureLoader::Stage, TextureLoader::Archive, TextureLoader::Tga,
				 TextureLoader::HudColor, TextureLoader::File}) {
		for (const TextureLoad &load : texture_load_attempts(loader, "trim.tga", files))
			CHECK(load.file == "trim.tga", "the loader opens the name it was given");
	}
	CHECK(only(TextureLoader::Stage, "trim.png", files).reader == TextureReader::None,
			"a .png name loads nothing outside the menus");
}

void test_stage() {
	// The .dds sibling of the cut query wins; ".MDT" and a loose hit keep the name.
	CHECK(only(TextureLoader::Stage, "x.tga", mounted({"x.tga", "x.dds"})).file == "x.dds",
			"the .dds beside the name wins");
	CHECK(only(TextureLoader::Stage, "x.tga", mounted({"x.tga", "x.dds"}, {"x.tga"})).reader ==
					TextureReader::Tga,
			"a loose-first hit keeps the TGA");
	CHECK(only(TextureLoader::Stage, "x.dds.tga", mounted({"x.dds"})).file == "x.dds",
			"x.dds.tga cuts to x.dds");
	CHECK(only(TextureLoader::Stage, "x.pcx", mounted({"x.pcx"})).reader == TextureReader::Pcx,
			"a PCX through the PCX reader");
}

void test_plain_upper_pcx_mask() {
	CHECK(only(TextureLoader::Plain, "SKIN.PCX", mounted({})).transform ==
					TextureLoadTransform::WhiteAlphaFromBlue,
			"an upper-case .PCX turns white with its blue as alpha");
	CHECK(only(TextureLoader::Plain, "skin.pcx", mounted({})).transform == TextureLoadTransform::None,
			"a lower-case .pcx stays colour");
	CHECK(only(TextureLoader::Plain, "skin.tga", mounted({"skin.dds"})).file == "skin.tga",
			"no DDS probe");
}

void test_archive() {
	CHECK(only(TextureLoader::Archive, "wake5.tga", mounted({"wake5.tga", "wake5.dds"})).reader ==
					TextureReader::Dds,
			"the .dds sibling first");
	const TextureLoad empty_alpha = only(TextureLoader::Archive, "cld.pcx", mounted({"cld.pcx"}));
	CHECK(empty_alpha.reader == TextureReader::Pcx &&
					empty_alpha.transform == TextureLoadTransform::None,
			"a PCX with an empty alpha name stays opaque");
	CHECK(only(TextureLoader::ArchiveSelfAlpha, "cld.pcx", mounted({"cld.pcx"})).transform ==
					TextureLoadTransform::PaletteLuminanceAlpha,
			"a PCX naming itself as alpha takes its palette luminance");
	CHECK(only(TextureLoader::Archive, "x.bmp", mounted({"x.bmp"})).reader == TextureReader::None,
			"any other extension loads nothing");
}

void test_file() {
	CHECK(only(TextureLoader::File, "TSDicon.tga", mounted({})).reader == TextureReader::Tga, "a TGA");
	CHECK(only(TextureLoader::File, "icons.bmp", mounted({})).reader == TextureReader::Pcx,
			"any other name through the PCX reader");
	CHECK(file_texture_load("x.pcx", true).transform == TextureLoadTransform::WhiteAlphaFromBlue,
			"flag 0x200000 whitens a PCX");
	CHECK(file_texture_load("x.tga", true).transform == TextureLoadTransform::None,
			"flag 0x200000 leaves a TGA alone");
}

void test_hud() {
	const TextureLoad pcx = only(TextureLoader::HudColor, "rockpip.pcx", mounted({}));
	CHECK(pcx.reader == TextureReader::Pcx && pcx.transform == TextureLoadTransform::WhiteAlphaFromBlue,
			"a HUD PCX turns white with its blue as alpha");
	CHECK(!pcx.alpha_only, "colour mode");
	const TextureLoad full = only(TextureLoader::HudAlpha, "stance1.tga.full", mounted({}));
	CHECK(full.file == "stance1.tga" && !full.alpha_only, ".FULL cuts off and makes it colour");
	const TextureLoad alpha = only(TextureLoader::HudColor, "logo.TGA.Alpha", mounted({}));
	CHECK(alpha.file == "logo.TGA" && alpha.alpha_only, ".ALPHA cuts off and makes it alpha-only");
	CHECK(only(TextureLoader::HudAlpha, "scopexh.tga", mounted({})).alpha_only, "alpha mode");
	CHECK(only(TextureLoader::HudColor, "x.dds", mounted({"x.dds"})).reader == TextureReader::None,
			"the HUD reads TGA and PCX only");
}

void test_menu_and_cine() {
	CHECK(only(TextureLoader::Menu, "a.tga", mounted({"a.tga"})).reader == TextureReader::Tga,
			"menu: the TGA when it exists");
	const TextureLoad dds = only(TextureLoader::Menu, "a.b.tga", mounted({}));
	CHECK(dds.file == "a.dds" && dds.reader == TextureReader::Dds,
			"menu: else the name from its first dot replaced by dds");
	CHECK(only(TextureLoader::Menu, "a.png", mounted({})).reader == TextureReader::Png, "menu: PNG");
	CHECK(only(TextureLoader::Menu, "a.bmp", mounted({"a.bmp"})).reader == TextureReader::None,
			"menu: BMP nothing");
	const std::vector<TextureLoad> fade = texture_load_attempts(TextureLoader::CineFade, "win.pcx", mounted({}));
	CHECK(fade.size() == 2 && fade[0].reader == TextureReader::Pcx && fade[1].reader == TextureReader::Tga,
			"cine: a non-TGA name tries the PCX reader, then the TGA reader");
	const std::vector<TextureLoad> missing = texture_load_attempts(TextureLoader::CineFade, "win.tga", mounted({}));
	CHECK(missing.size() == 1 && missing[0].file == "win.dds" && missing[0].reader == TextureReader::Dds,
			"cine: a missing .tga reads its .dds");
}

void test_transforms() {
	std::vector<uint8_t> rgba = {10, 20, 30, 40, 1, 2, 3, 4};
	apply_texture_load_transform(TextureLoadTransform::WhiteAlphaFromBlue, false, rgba.data(), 2);
	CHECK((rgba == std::vector<uint8_t>{255, 255, 255, 30, 255, 255, 255, 3}), "white, blue as alpha");
	rgba = {10, 20, 30, 40};
	apply_texture_load_transform(TextureLoadTransform::None, true, rgba.data(), 1);
	CHECK((rgba == std::vector<uint8_t>{255, 255, 255, 40}), "alpha-only keeps the alpha under white");
}

void test_halving() {
	// 4x2 -> cap 2 -> 2x1: each channel the truncated mean of its 2x2 block.
	std::vector<uint8_t> rgba(4 * 2 * 4, 0);
	const uint8_t reds[8] = {1, 2, 10, 20, 3, 5, 30, 41};
	for (int i = 0; i < 8; ++i) {
		rgba[static_cast<size_t>(i) * 4] = reds[i];
		rgba[static_cast<size_t>(i) * 4 + 3] = 255;
	}
	uint32_t w = 4, h = 2;
	halve_rgba_to_cap(rgba, w, h, 2);
	CHECK(w == 2 && h == 1, "halved once");
	CHECK(rgba[0] == (1 + 2 + 3 + 5) / 4 && rgba[4] == (10 + 20 + 30 + 41) / 4, "box mean, truncated");
	CHECK(rgba[3] == 255, "alpha averaged too");
	std::vector<uint8_t> small(4 * 4, 7);
	uint32_t sw = 2, sh = 2;
	halve_rgba_to_cap(small, sw, sh, kNormalMapSideCap);
	CHECK(sw == 2 && sh == 2 && small.size() == 16, "under the cap nothing changes");
}

// A minimal 8-bit PCX: `width` x `height`, BytesPerLine `bpl`, raw (no runs) rows
// of indices, then the 0x0C marker and a palette whose entry i is (i, 2i, 3i).
std::vector<uint8_t> pcx8(int width, int height, int bpl, const std::vector<uint8_t> &rows) {
	std::vector<uint8_t> bytes(128, 0);
	bytes[0] = 0x0A;
	bytes[1] = 5;
	bytes[2] = 1;
	bytes[3] = 8;
	bytes[8] = static_cast<uint8_t>(width - 1);
	bytes[10] = static_cast<uint8_t>(height - 1);
	bytes[0x41] = 1;
	bytes[0x42] = static_cast<uint8_t>(bpl);
	for (uint8_t b : rows) bytes.push_back(b);
	bytes.push_back(0x0C);
	for (int i = 0; i < 256; ++i) {
		bytes.push_back(static_cast<uint8_t>(i));
		bytes.push_back(static_cast<uint8_t>(2 * i));
		bytes.push_back(static_cast<uint8_t>(3 * i));
	}
	return bytes;
}

void test_pcx_reader() {
	// A 3x2 image with BytesPerLine 4: each row's pad pixel lands on the next row's
	// first pixel, which that row then overwrites; every pixel opaque
	// [orig: Texture_LoadPCXFromPFF32 @ 0x56EA30, the rows @ 0x56ED70..0x56EDFC].
	const std::vector<uint8_t> bytes = pcx8(3, 2, 4, {1, 2, 3, 9, 4, 5, 6, 7});
	opennova::RgbaImage image;
	std::string error;
	CHECK(opennova::decode_pcx_menu_rgba(bytes.data(), bytes.size(), image, error), "decodes");
	CHECK(image.width == 3 && image.height == 2, "3x2");
	CHECK(image.pixels.size() == 24 && image.pixels[3 * 4] == 4 && image.pixels[3 * 4 + 3] == 255,
			"the second row starts with its own first pixel, opaque");
	std::vector<uint8_t> not8 = bytes;
	not8[3] = 4;
	CHECK(!opennova::decode_pcx_menu_rgba(not8.data(), not8.size(), image, error),
			"a PCX that is not 8 bits per channel fails");
}

} // namespace

int main() {
	test_no_alternate_names();
	test_stage();
	test_plain_upper_pcx_mask();
	test_archive();
	test_file();
	test_hud();
	test_menu_and_cine();
	test_transforms();
	test_halving();
	test_pcx_reader();
	if (failures != 0) {
		std::fprintf(stderr, "texture_load_rules: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("texture_load_rules: ok\n");
	return 0;
}
