// opennova-3di texture: an image (a PNG, a TGA, an .mdt or a PCX) written as the texture file a model's row loads:
// a DXT `.dds` with its mip chain (the form of the game's own model textures: render-material-re, "The
// install"), or a 32-bit `.tga` or `.mdt`. The reading, the halving and the writing are the editor's image
// import's (editor/import/texture_import.h: decode_image_source, resize_image, encode_image; a DDS's blocks
// and chain editor/import/dxt_encode.h), so a texture the Blender add-on writes is the one the editor's
// import of the same image writes. Tooling, not a port.

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <editor/import/texture_import.h>
#include <formats/dds/dds.h>

#include "threedi_cli.h"

namespace opennova::threedi_cli {

namespace {

bool power_of_two(uint32_t side) { return side != 0 && (side & (side - 1)) == 0; }

std::string extension_of(const std::string &path) {
	const size_t slash = path.find_last_of("/\\");
	const size_t dot = path.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
	return strutil::to_lower(path.substr(dot));
}

std::string sides(uint32_t width, uint32_t height) { return std::to_string(width) + " x " + std::to_string(height); }

} // namespace

int cmd_texture(const TextureCommand &command) {
	const std::string in = command.input, out = command.output;
	const std::string in_extension = extension_of(in), out_extension = extension_of(out);
	if (out_extension != ".dds" && out_extension != ".tga" && out_extension != ".mdt") {
		std::fprintf(stderr, "opennova-3di: texture writes a .dds, a .tga or an .mdt, not %s\n", out.c_str());
		return 2;
	}
	if (out_extension != ".dds" && (!command.format.empty() || !command.mips.empty())) {
		std::fprintf(stderr, "opennova-3di: --format and --mips are a .dds's; %s is a 32-bit TGA\n", out.c_str());
		return 2;
	}
	std::ifstream file(in, std::ios::binary);
	if (!file) {
		std::fprintf(stderr, "opennova-3di: cannot open %s\n", in.c_str());
		return 1;
	}
	const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	editor::ImageSource source;
	std::string error;
	// The source read as an image program reads it (its origin honoured, every depth), by its extension: an
	// .mdt is a TGA under another name.
	const std::string read_as = in_extension == ".mdt" ? in.substr(0, in.size() - 4) + ".tga" : in;
	if (!editor::decode_image_source(read_as, bytes, source, error)) {
		std::fprintf(stderr, "opennova-3di: %s: %s\n", in.c_str(), error.c_str());
		return 1;
	}
	RgbaImage &image = source.image;
	const uint32_t source_width = uint32_t(image.width), source_height = uint32_t(image.height);
	// Halved while a side exceeds the cap: each texel the 2 x 2 box's sum shifted down by two, as the game
	// halves a texture past its cap (renderer::halve_rgba_to_cap [orig: GTexture_Downsample2x2_RGBA8 @
	// 0x687000]), which editor::resize_image makes of an exact half.
	if (command.max_size > 0)
		while (uint32_t(image.width) > command.max_size || uint32_t(image.height) > command.max_size)
			image = editor::resize_image(image, std::max(1u, uint32_t(image.width) / 2), std::max(1u, uint32_t(image.height) / 2));
	const uint32_t width = uint32_t(image.width), height = uint32_t(image.height);
	// Its alpha as the import makes it (an image whose stray alpha no shader reads written opaque: DXT1).
	if (!command.alpha.empty() && !editor::apply_image_alpha(image, strutil::to_lower(command.alpha), error)) {
		std::fprintf(stderr, "opennova-3di: --alpha: %s\n", error.c_str());
		return 2;
	}
	editor::ImageImportSettings settings = editor::image_import_settings({});
	std::string form;
	if (out_extension == ".dds") {
		// A DDS-reader image is created at D3DX_DEFAULT sides, which D3DX rounds up to powers of two, the
		// texels placed top left with transparent black past them, so a model's UVs reach the padding
		// [orig: GTexture_InitFromMemory @ 0x68830E..0x688310; D3DXCreateTextureFromFileInMemoryEx_Internal @
		// 0x6914D9..0x6914EE, @ 0x69150B..0x691520; CBlt::BltNone @ 0x6E0D57] (render-material-re D-RMAT-18).
		if (!power_of_two(width) || !power_of_two(height)) {
			std::fprintf(stderr,
			             "opennova-3di: %s would be %s: the game pads a .dds whose sides are not powers of two up to the next "
			             "ones (D3DX_DEFAULT sides), so a model's UVs reach the padding; make each side a power of two, "
			             "or write a .tga, which the game takes at any size\n",
			             out.c_str(), sides(width, height).c_str());
			return 1;
		}
		std::string format = command.format.empty() ? "auto" : command.format;
		if (format == "auto") {
			// DXT1 when every texel is opaque, DXT5 (its graded alpha) otherwise.
			bool opaque = true;
			for (size_t i = 3; i < image.pixels.size() && opaque; i += 4) opaque = image.pixels[i] == 255;
			format = opaque ? "dxt1" : "dxt5";
		}
		if (format != "dxt1" && format != "dxt5" && format != "argb") {
			std::fprintf(stderr, "opennova-3di: --format takes auto, dxt1, dxt5 or argb, not %s\n", format.c_str());
			return 2;
		}
		const std::string mips = command.mips.empty() ? "full" : command.mips;
		if (mips != "full" && mips != "none") {
			std::fprintf(stderr, "opennova-3di: --mips takes full or none, not %s\n", mips.c_str());
			return 2;
		}
		if (format == "argb" && !command.mips.empty()) {
			std::fprintf(stderr, "opennova-3di: an argb .dds is one uncompressed level; --mips is a DXT's\n");
			return 2;
		}
		settings.format = "dds";
		settings.dds = format;
		settings.mips = mips;
		form = format == "argb" ? "A8R8G8B8" : strutil::to_upper(format);
	} else {
		settings.format = out_extension == ".mdt" ? "mdt" : "tga";
		form = out_extension == ".mdt" ? "32-bit TGA under .mdt" : "32-bit TGA";
	}
	std::vector<uint8_t> written;
	std::string note;
	if (!editor::encode_image(image, settings, written, error, note)) {
		std::fprintf(stderr, "opennova-3di: cannot write %s: %s\n", out.c_str(), error.c_str());
		return 1;
	}
	// A normal map is halved until it fits 512 a side when the game loads it [orig: Material_LoadStageTexture @
	// 0x5B1782, flag 0x1000; GTexture_DownsampleToLimits @ 0x687170].
	if (out_extension == ".mdt" && (width > 512 || height > 512))
		std::fprintf(stderr, "opennova-3di: note: %s is %s: the game halves a normal map until it fits 512 a side\n", out.c_str(),
		             sides(width, height).c_str());
	if (!write_output(out.c_str(), written.data(), written.size())) return 1;
	std::string levels;
	if (settings.format == "dds" && settings.dds != "argb") {
		dds::DdsImage read;
		std::string ignored;
		if (dds::dds_read(written.data(), written.size(), read, ignored))
			levels = " of " + std::to_string(read.levels.size()) + (read.levels.size() == 1 ? " level" : " levels");
	}
	const std::string halved = width != source_width || height != source_height ? " halved from " + sides(source_width, source_height) : "";
	std::printf("wrote %s (%s%s, %s%s, %zu bytes)\n", out.c_str(), form.c_str(), levels.c_str(), sides(width, height).c_str(),
	            halved.c_str(), written.size());
	return 0;
}

} // namespace opennova::threedi_cli
