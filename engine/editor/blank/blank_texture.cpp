#include "blank_makers.h"

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

// The checkerboard the game draws for a texture it cannot load, its own pixels as they are
// (renderer::missing_material_texture_rgba: 128 by 128 opaque gray squares), written in the
// format the name asks for: a TGA or a DDS whole, a PCX as the two grays' palette image.
bool make_blank_texture(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::string reason;
	if (!can_make_blank_texture(request.logical_name, reason)) {
		error = make_finding(CoreFinding::BlankTexture, DiagnosticSeverity::Error, reason, request.logical_name);
		return false;
	}
	const std::vector<uint8_t> pixels = renderer::missing_material_texture_rgba();
	const uint32_t side = renderer::kMissingMaterialTextureSide;
	bool written = false;
	switch (blank_texture_reader(request.logical_name)) {
	case MaterialTextureReader::Tga: written = tga::tga_write_rgba32(pixels.data(), side, side, out, reason); break;
	case MaterialTextureReader::Dds: written = dds::dds_write_a8r8g8b8(pixels.data(), side, side, out, reason); break;
	case MaterialTextureReader::Pcx: {
		RgbaImage image;
		image.width = image.height = int(side);
		image.pixels = pixels;
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
