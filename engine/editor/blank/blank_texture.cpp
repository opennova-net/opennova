#include "blank_makers.h"

#include <utility>

#include <base/resource_index/resource_kind.h>
#include <editor/import/quantize.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
#include <runtime/hud/loading_screen.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

using renderer::MaterialTextureReader;

// The format a placeholder of this name is written in, as the reader it is for: the name's
// extension decides what the file is (the scan's rule), a .mdt being a TGA the game reads
// through its TGA reader (renderer::material_texture_source).
MaterialTextureReader blank_texture_reader(const std::string &logical_name) {
	const std::string extension = resource_extension_for_name(logical_name);
	if (extension == ".tga" || extension == ".mdt") return MaterialTextureReader::Tga;
	if (extension == ".pcx") return MaterialTextureReader::Pcx;
	if (extension == ".dds") return MaterialTextureReader::Dds;
	return MaterialTextureReader::None;
}

bool can_make_blank_texture(const std::string &logical_name, std::string &reason) {
	if (blank_texture_reader(logical_name) != MaterialTextureReader::None) return true;
	reason = "A placeholder texture is a .tga, .mdt, .pcx or .dds file; " + logical_name + " is none of them.";
	return false;
}

namespace {

// The checkerboard the game draws for a texture it cannot load, its own pixels as they are
// (renderer::missing_material_texture_rgba: 128 by 128 opaque gray squares), repeated over
// `width` by `height` (a side of 128 is that image whole), written in the format the name asks
// for: a TGA or a DDS whole, a PCX as the two grays' palette image.
bool make_placeholder(const BlankRequest &request, uint32_t width, uint32_t height, std::vector<uint8_t> &out,
                      Diagnostic &error) {
	std::string reason;
	if (!can_make_blank_texture(request.logical_name, reason)) {
		error = make_finding(CoreFinding::BlankTexture, DiagnosticSeverity::Error, reason, request.logical_name);
		return false;
	}
	const std::vector<uint8_t> tile = renderer::missing_material_texture_rgba();
	const uint32_t side = renderer::kMissingMaterialTextureSide;
	std::vector<uint8_t> pixels(size_t(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x)
			for (int c = 0; c < 4; ++c)
				pixels[(size_t(y) * width + x) * 4 + c] = tile[(size_t(y % side) * side + x % side) * 4 + c];
	bool written = false;
	switch (blank_texture_reader(request.logical_name)) {
	case MaterialTextureReader::Tga: written = tga::tga_write_rgba32(pixels.data(), width, height, out, reason); break;
	case MaterialTextureReader::Dds: written = dds::dds_write_a8r8g8b8(pixels.data(), width, height, out, reason); break;
	case MaterialTextureReader::Pcx: {
		RgbaImage image;
		image.width = int(width);
		image.height = int(height);
		image.pixels = std::move(pixels);
		written = encode_pcx_indexed(quantize_to_256(image), out, reason);
		break;
	}
	case MaterialTextureReader::Chunk:
	case MaterialTextureReader::None: break;
	}
	if (!written) {
		out.clear();
		error = make_finding(CoreFinding::BlankTexture, DiagnosticSeverity::Error, "The placeholder texture could not be written: " + reason,
		                     request.logical_name);
	}
	return written;
}

} // namespace

bool make_blank_texture(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return make_placeholder(request, renderer::kMissingMaterialTextureSide, renderer::kMissingMaterialTextureSide, out,
	                        error);
}

// The mission's fixed textures, each the placeholder at the size the game's own file has, which the
// screens they fill are drawn for (border.tga is the 128 by 128 one above). loadscrn.pcx: the
// loading screen's fallback image, stretched over the screen [orig: Render_LoadingScreen @ 0x521d10,
// the fallback @ 0x521e20; LoadingScreen_DrawEffectFullscreen @ 0x586ba0], 800 by 600.
bool make_blank_loading_screen(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return make_placeholder(request, 800, 600, out, error);
}

// The boards' box (the scoreboard's, the tips'): border.tga its pieces, boxtile.tga its tiled fill,
// 256 by 256, monogram.tga its watermark, 512 by 256 [orig: Game_StartMission @ 0x525aa3-0x525aad
// -> BoxTexture_LoadAndSetupUVRegions @ 0x56acd0].
bool make_blank_monogram(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return make_placeholder(request, 512, 256, out, error);
}

bool make_blank_boxtile(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return make_placeholder(request, 256, 256, out, error);
}

namespace {

// The pointer's art, authored here (no retail byte): an arrow whose tip is the image's top-left
// pixel, the point the game draws the pointer's texture from [orig: CUIScene_DrawScreensAndCursor
// @ 0x63bfda, the rect's top left at the mouse; Game_ShowStartMissionSplash @ 0x520820's quad at
// the live cursor alike], '#' its black outline, 'o' its white body, the rest clear. It sits in a
// 32 by 32 image, the size of the shipped pointer's, drawn at the texture's own size, unscaled,
// in the menus [orig: @ 0x63bfb9..0x63c046, scale 1.0].
constexpr uint32_t kPointerSide = 32;
constexpr const char *kPointerArt[] = {
	"#",
	"##",
	"#o#",
	"#oo#",
	"#ooo#",
	"#oooo#",
	"#ooooo#",
	"#oooooo#",
	"#ooooooo#",
	"#oooooooo#",
	"#ooooo#####",
	"#oo#oo#",
	"#o# #oo#",
	"##  #oo#",
	"#    #oo#",
	"     #oo#",
	"      ##",
};

} // namespace

const char *blank_pointer_name() {
	return hud::kSplashArrowImage;
}

// The pointer, a TGA as the menus' and the splash's TGA readers take it [orig: CUIImage_LoadTGA
// @ 0x6647D0; CTerrainTileData_LoadTGAFromArchive @ 0x56E570]: 32-bit with its alpha, the clear
// pixels clear, which the shipped screens' STANDARD_TRANSPARENT blends away
// (docs/mnu/menu-re.md, "Cursor material flags").
bool make_blank_pointer(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	if (blank_texture_reader(request.logical_name) != MaterialTextureReader::Tga) {
		error = make_finding(CoreFinding::BlankTexture, DiagnosticSeverity::Error,
		                     "The mouse pointer is a .tga file; " + request.logical_name + " is not.", request.logical_name);
		return false;
	}
	std::vector<uint8_t> pixels(size_t(kPointerSide) * kPointerSide * 4, 0);
	uint32_t y = 0;
	for (const char *row : kPointerArt) {
		for (uint32_t x = 0; row[x] != '\0' && x < kPointerSide; ++x) {
			if (row[x] == ' ') continue;
			uint8_t *p = &pixels[(size_t(y) * kPointerSide + x) * 4];
			const uint8_t shade = row[x] == 'o' ? 0xFF : 0x00;
			p[0] = p[1] = p[2] = shade;
			p[3] = 0xFF;
		}
		++y;
	}
	std::string reason;
	if (!tga::tga_write_rgba32(pixels.data(), kPointerSide, kPointerSide, out, reason)) {
		out.clear();
		error = make_finding(CoreFinding::BlankTexture, DiagnosticSeverity::Error, "The mouse pointer could not be written: " + reason,
		                     request.logical_name);
		return false;
	}
	return true;
}

} // namespace opennova::editor
