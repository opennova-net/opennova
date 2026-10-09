// The game's texture loaders as rules (renderer/texture_load_rules.h): the file each
// loader opens and the reader it names, no alternate name ever, the transforms the
// loaders make, the normal-map cap's halving, and the PCX reader every loader uses.

#include <formats/pcx/pcx_io.h>
#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_texture_materials.h>
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
	// A count of halvings, each the truncated 2x2 mean (101 and 102 make 101), while both sides exceed 1.
	std::vector<uint8_t> odd(4 * 4 * 4, 255);
	for (size_t i = 0; i < odd.size(); i += 4) odd[i] = static_cast<uint8_t>((i / 4) % 2 ? 102 : 101);
	std::vector<uint8_t> once = odd;
	uint32_t ow = 4, oh = 4;
	halve_rgba_times(once, ow, oh, 1);
	CHECK(ow == 2 && oh == 2 && once.size() == 16 && once[0] == 101, "one halving, the truncated mean");
	std::vector<uint8_t> twice = odd;
	uint32_t tw = 4, th = 4;
	halve_rgba_times(twice, tw, th, 5);
	CHECK(tw == 1 && th == 1 && twice.size() == 4, "halvings stop at a side of 1");
	std::vector<uint8_t> strip(8 * 2 * 4, 9);
	uint32_t pw = 8, ph = 2;
	halve_rgba_times(strip, pw, ph, 3);
	CHECK(pw == 4 && ph == 1, "a side of 1 stops the halvings, the other side halved once");
}

void test_loaders_past_particle() {
	// The model row's, map and cube loaders name no file of their own.
	const TextureFileQuery files = mounted({"skin.tga", "skin.dds", "fol.pcx", "HwmCube.dds"});
	for (TextureLoader loader : {TextureLoader::Normal, TextureLoader::Producer, TextureLoader::Chunk,
				 TextureLoader::Pcx8, TextureLoader::Cube}) {
		CHECK(!texture_loader_has_attempts(loader), "no attempts past Particle");
		CHECK(texture_load_attempts(loader, "skin.tga", files).empty(), "no file tried");
	}
	CHECK(texture_loader_has_attempts(TextureLoader::Stage) && texture_loader_has_attempts(TextureLoader::Particle),
			"Stage to Particle name files");
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

void test_masks_follow_the_name() {
	// Texture_LoadAndRegister and the HUD loader mask by the name, after either reader.
	const TextureLoad plain = only(TextureLoader::Plain, "SKIN.PCX.TGA", mounted({}));
	CHECK(plain.reader == TextureReader::Tga && plain.transform == TextureLoadTransform::WhiteAlphaFromBlue,
			"an upper-case .PCX in a TGA's name masks it");
	const TextureLoad hud = only(TextureLoader::HudColor, "logo.pcx.tga", mounted({}));
	CHECK(hud.reader == TextureReader::Tga && hud.transform == TextureLoadTransform::WhiteAlphaFromBlue,
			"the HUD masks a TGA whose name holds .PCX");
	CHECK(only(TextureLoader::HudColor, "logo.tga", mounted({})).transform == TextureLoadTransform::None,
			"a plain HUD TGA keeps its colour");
}

void test_particle_attempts() {
	const std::vector<TextureLoad> attempts =
			texture_load_attempts(TextureLoader::Particle, "fx\\spark.tga", mounted({}));
	CHECK(attempts.size() == 2, "the loose folder, then the mounted name");
	if (attempts.size() == 2) {
		CHECK(attempts[0].source == TextureFileSource::ParticleTextureDir &&
						attempts[0].reader == TextureReader::TgaParticleLoose && attempts[0].file == "fx\\spark.tga",
				"the loose leg reads the whole name from the particle folder");
		CHECK(attempts[1].source == TextureFileSource::Mounted && attempts[1].reader == TextureReader::Tga &&
						attempts[1].file == "spark.tga",
				"the archive leg reads the part after the last backslash");
	}
}

void test_hud_alpha_material() {
	CHECK(hud_alpha_material_argb(0xFFA0A0A0u) == 0xFFFFFFFFu, "0xA0 doubles and saturates to white");
	CHECK(hud_alpha_material_argb(0x80102030u) == 0x80204060u, "twice each channel, alpha kept");
}

// The colour material 0x651: MODULATE2X(TEXTURE, DIFFUSE) colour, MODULATE alpha
// (D-HUD-49) [orig: RenderState_DecodeModeColorStage @ 0x6814BE..0x6814CA].
void test_hud_color_material() {
	CHECK(hud_color_material_argb(0xFFFFFFFFu, 0xFF7F7F7Fu) == 0xFFFEFEFEu,
			"a half-bright diffuse over a white texel lands at full brightness");
	CHECK(hud_color_material_argb(0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFFu, "white stays white");
	CHECK(hud_color_material_argb(0xFF808080u, 0xFFFFFFFFu) == 0xFFFFFFFFu,
			"a mid texel under a white diffuse doubles and saturates");
	CHECK(hud_color_material_argb(0x80404040u, 0x80FFFFFFu) == 0x40808080u,
			"twice texel x diffuse per colour channel; the alpha only modulates");
	CHECK(hud_color_material_argb(0xFF102030u, 0xFF000000u) == 0xFF000000u, "a black diffuse draws black");
}

// The stage a material word selects is its colour family, whatever loader made
// the texture: the file-loader textures whose words are family 0x600 double
// like the HUD loader's colour mode (D-HUD-49) [orig: RenderState_DecodeModeColorStage
// @ 0x681080, `opcode & 0x3F00` @ 0x68113a].
void test_material_color_stage() {
	using opennova::hud::kBoxMaterialWord;
	using opennova::hud::kLfpIconMaterialWord;
	using opennova::hud::kNetIconMaterialWord;
	using opennova::hud::kTipIconMaterialWord;
	using opennova::hud::kTsdIconMaterialWord;
	using opennova::hud::kWpIndicatorMaterialWord;
	CHECK(hud_loader_material_word(false) == 0x651u && hud_loader_material_word(true) == 0xA51u,
			"the HUD loader's colour and alpha words");
	CHECK(material_color_stage(hud_loader_material_word(false)) == MaterialColorStage::Modulate2x,
			"the HUD loader's colour mode doubles");
	CHECK(material_color_stage(hud_loader_material_word(true)) == MaterialColorStage::AddDiffuse,
			"the HUD loader's alpha mode adds the diffuse to itself");
	CHECK(material_color_stage(kLfpIconMaterialWord) == MaterialColorStage::Modulate2x,
			"the capture-point icons' 0x300631 doubles");
	CHECK(material_color_stage(kWpIndicatorMaterialWord) == MaterialColorStage::Modulate2x,
			"the waypoint indicator's 0x300631 doubles");
	CHECK(material_color_stage(kTsdIconMaterialWord) == MaterialColorStage::Modulate2x &&
					material_color_stage(kBoxMaterialWord) == MaterialColorStage::Modulate2x &&
					material_color_stage(kTipIconMaterialWord) == MaterialColorStage::Modulate2x,
			"the icon strip, the box styles and the tip icons double");
	CHECK(material_color_stage(kNetIconMaterialWord) == MaterialColorStage::Other,
			"the network icons' 0x300451 is SELECTARG1(TEXTURE)");
	CHECK(material_color_stage(0x300402u) == MaterialColorStage::Other,
			"the binocular digits' 0x300402 is family 0x400");
	CHECK(material_color_stage(0u) == MaterialColorStage::Other, "no material draws plain");
	// Every font page draws through 0x651 whatever loader made the font, so every
	// glyph doubles (D-HUD-51) [orig: GameFont_LoadFromBlob @0x674825;
	// CGameFont_Create @0x674b9c / @0x674d1e].
	CHECK(material_color_stage(opennova::hud::kFontPageMaterialWord) == MaterialColorStage::Modulate2x,
			"a font page's material runs MODULATE2X");
}

void test_side_caps() {
	CHECK(material_texture_side_cap(4) == 512 && material_texture_side_cap(5) == 512, "normal maps");
	CHECK(material_texture_side_cap(7) == 512, "the occlusion producer");
	CHECK(material_texture_side_cap(6) == 0, "the horizon volume is never downsampled");
	CHECK(material_texture_side_cap(0) == 0 && material_texture_side_cap(1) == 0, "diffuse rows");
}

void test_dds_codec_order() {
	const std::vector<DdsCodec> &order = dds_reader_codec_order();
	CHECK(order.size() == 9 && order[0] == DdsCodec::Bmp && order[2] == DdsCodec::Dds &&
					order[3] == DdsCodec::Jpeg && order[4] == DdsCodec::Png && order[7] == DdsCodec::Tga,
			"BMP, PPM, DDS, JPEG, PNG, PFM, HDR, TGA, DIB");
}

// A minimal 24-bit (NPlanes 3) PCX, raw scanlines of 3 * BytesPerLine bytes.
std::vector<uint8_t> pcx24(int width, int height, int bpl, const std::vector<uint8_t> &rows) {
	std::vector<uint8_t> bytes(128, 0);
	bytes[0] = 0x0A;
	bytes[1] = 5;
	bytes[2] = 1;
	bytes[3] = 8;
	bytes[8] = static_cast<uint8_t>(width - 1);
	bytes[10] = static_cast<uint8_t>(height - 1);
	bytes[0x41] = 3;
	bytes[0x42] = static_cast<uint8_t>(bpl);
	for (uint8_t b : rows) bytes.push_back(b);
	return bytes;
}

void test_pcx_more() {
	// 24-bit: the red, green and blue planes `width` apart in each scanline.
	const std::vector<uint8_t> rgb = pcx24(2, 1, 2, {10, 20, 30, 40, 50, 60});
	opennova::RgbaImage image;
	std::string error;
	CHECK(opennova::decode_pcx_menu_rgba(rgb.data(), rgb.size(), image, error), "24-bit decodes");
	CHECK(image.pixels.size() == 8 && image.pixels[0] == 10 && image.pixels[1] == 30 &&
					image.pixels[2] == 50 && image.pixels[4] == 20,
			"pixel 0 is (plane0[0], plane1[0], plane2[0])");
	// A header naming 30000 x 30000 over a few bytes fails before the buffer.
	std::vector<uint8_t> huge = pcx8(2, 1, 2, {1, 2});
	huge[8] = 0x2F;
	huge[9] = 0x75;
	huge[10] = 0x2F;
	huge[11] = 0x75;
	CHECK(!opennova::decode_pcx_menu_rgba(huge.data(), huge.size(), image, error), "the colour reader refuses");
	CHECK(!opennova::decode_pcx_luminance_alpha(huge.data(), huge.size(), image, error),
			"the 8-bit reader refuses");
	// The 8-bit alpha read takes the last 768 bytes without the 0x0C marker, and an
	// odd width's pad index spills onto the next row and, from the last row, onto the
	// palette [orig: Texture_LoadPCXFromPFF8Bit @ 0x56E0A0].
	std::vector<uint8_t> unmarked = pcx8(3, 2, 4, {1, 2, 3, 9, 0, 5, 6, 7});
	unmarked[128 + 8] = 0x00; // the marker byte, now a zero
	CHECK(opennova::decode_pcx_luminance_alpha(unmarked.data(), unmarked.size(), image, error),
			"no marker needed");
	CHECK(image.width == 3 && image.height == 2 && image.pixels.size() == 24, "3 x 2 out");
	// Palette entry 1 is (1, 2, 3): luminance (85 * 6) >> 8 = 1. Pixel 3 is row 1's own
	// index 0 (row 0's pad 9 overwritten); row 1's pad 7 landed on entry 0's blue, so
	// entry 0 reads (0, 0, 7), luminance (85 * 7) >> 8 = 2.
	if (image.pixels.size() == 24) {
		CHECK(image.pixels[3] == 1, "pixel 0's alpha is entry 1's luminance");
		CHECK(image.pixels[3 * 4 + 3] == 2 && image.pixels[3 * 4 + 2] == 7,
				"the last row's pad index clobbered the palette the luminance reads");
	}
}

} // namespace

int main() {
	test_masks_follow_the_name();
	test_particle_attempts();
	test_hud_alpha_material();
	test_hud_color_material();
	test_material_color_stage();
	test_side_caps();
	test_dds_codec_order();
	test_pcx_more();
	test_no_alternate_names();
	test_stage();
	test_plain_upper_pcx_mask();
	test_archive();
	test_file();
	test_hud();
	test_menu_and_cine();
	test_transforms();
	test_halving();
	test_loaders_past_particle();
	test_pcx_reader();
	if (failures != 0) {
		std::fprintf(stderr, "texture_load_rules: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("texture_load_rules: ok\n");
	return 0;
}
