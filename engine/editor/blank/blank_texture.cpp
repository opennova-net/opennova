#include "blank_makers.h"

#include <base/resource_index/resource_kind.h>
#include <editor/import/quantize.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
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

} // namespace opennova::editor
