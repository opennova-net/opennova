#include "texture_load_rules.h"

#include <base/io/strutil.h>
#include <runtime/menu/menu_assets.h>

namespace opennova::renderer {

namespace {

bool holds(const std::string &upper, const char *what) {
	return upper.find(what) != std::string::npos;
}

// The plain dispatch the pixel loaders run on their upper-cased name: .TGA or .MDT
// through the TGA reader, .PCX through the PCX reader, anything else nothing.
TextureLoad plain_dispatch(std::string_view file) {
	const std::string upper = strutil::to_upper(file);
	TextureLoad load;
	if (holds(upper, ".TGA") || holds(upper, ".MDT"))
		load.reader = TextureReader::Tga;
	else if (holds(upper, ".PCX"))
		load.reader = TextureReader::Pcx;
	else
		return load;
	load.file = std::string(file);
	return load;
}

TextureLoad one_reader(std::string_view name, TextureReader reader) {
	TextureLoad load;
	load.file = std::string(name);
	load.reader = reader;
	return load;
}

bool asks(const std::function<bool(const std::string &)> &query, const std::string &name) {
	return query && query(name);
}

// [orig: File_HasExtension @ 0x53C640 — stricmp from the FIRST '.']
bool has_first_dot_extension(std::string_view name, const char *extension) {
	const size_t dot = name.find('.');
	return dot != std::string_view::npos && strutil::iequals(name.substr(dot), extension);
}

// [orig: CinematicFadeEvent_LoadTexture @ 0x570D00 — a name whose extension from
//  its first '.' is ".tga" @ 0x570D80: the name through the TGA reader when it
//  exists @ 0x570D91..0x570DA3, else the name with everything after its first '.'
//  replaced by "dds" through the DXT decode @ 0x570DD1..0x570DE3, and nothing
//  after either; any other name the PCX reader @ 0x570E6B, then the TGA reader
//  @ 0x570ED6 (both failing draw the solid 4x4 fill @ 0x570F1B, the cine's own)]
std::vector<TextureLoad> cine_fade_attempts(std::string_view name, const TextureFileQuery &files) {
	if (has_first_dot_extension(name, ".tga")) {
		if (asks(files.exists, std::string(name)))
			return {one_reader(name, TextureReader::Tga)};
		return {one_reader(menu::replace_or_append_extension(std::string(name), "dds"),
				TextureReader::Dds)};
	}
	return {one_reader(name, TextureReader::Pcx), one_reader(name, TextureReader::Tga)};
}

TextureReader menu_reader(menu::MenuTextureFormat format) {
	switch (format) {
		case menu::MenuTextureFormat::Tga: return TextureReader::Tga;
		case menu::MenuTextureFormat::Dds: return TextureReader::Dds;
		case menu::MenuTextureFormat::Pcx: return TextureReader::Pcx;
		case menu::MenuTextureFormat::Png: return TextureReader::Png;
		case menu::MenuTextureFormat::None: break;
	}
	return TextureReader::None;
}

} // namespace

std::vector<TextureLoad> texture_load_attempts(TextureLoader loader, std::string_view name,
		const TextureFileQuery &files) {
	const std::string whole(name);
	TextureLoad load;
	switch (loader) {
		case TextureLoader::Stage: {
			const std::string query = material_texture_query(name);
			load = stage_texture_load(query, asks(files.loose_first_hit, query),
					asks(files.exists, material_dds_sibling(query)));
			break;
		}
		case TextureLoader::Plain:
			load = plain_texture_load(name);
			break;
		case TextureLoader::Archive:
		case TextureLoader::ArchiveSelfAlpha:
			load = archive_texture_load(name,
					loader == TextureLoader::ArchiveSelfAlpha && asks(files.exists, whole),
					asks(files.loose_first_hit, whole), asks(files.exists, material_dds_sibling(name)));
			break;
		case TextureLoader::File:
			// No caller of the ported HUD and view effects sets flag 0x200000
			// [orig: HUD_LoadAllTextures @ 0x59E04C..0x59E0F8 (flags 0x100000 /
			//  0x140000); ViewFx_InitShadersAndTextures @ 0x5CFE36 (0x100002)].
			load = file_texture_load(name, false);
			break;
		case TextureLoader::Tga:
			load = one_reader(name, TextureReader::Tga);
			break;
		case TextureLoader::Pcx:
			load = one_reader(name, TextureReader::Pcx);
			break;
		case TextureLoader::HudColor:
		case TextureLoader::HudAlpha:
			load = hud_texture_load(name, loader == TextureLoader::HudAlpha);
			break;
		case TextureLoader::Menu: {
			const menu::MenuTextureSource source = menu::menu_texture_source(whole, files.exists);
			load = one_reader(source.file, menu_reader(source.format));
			break;
		}
		case TextureLoader::CineFade:
			return cine_fade_attempts(name, files);
	}
	if (load.reader == TextureReader::None || load.file.empty()) return {};
	return {load};
}

// [orig: Texture_LoadByNameWithChannel @ 0x58B470 — material_image_source]
TextureLoad stage_texture_load(std::string_view query, bool loose_first_hit, bool dds_exists) {
	const MaterialImageSource source = material_image_source(query, loose_first_hit, dds_exists);
	TextureLoad load;
	switch (source.decoder) {
		case MaterialImageDecoder::Dds: load.reader = TextureReader::Dds; break;
		case MaterialImageDecoder::Tga: load.reader = TextureReader::Tga; break;
		case MaterialImageDecoder::Pcx: load.reader = TextureReader::Pcx; break;
		case MaterialImageDecoder::None: return load;
	}
	load.file = source.file;
	return load;
}

// [orig: Texture_LoadAndRegister @ 0x58B790 — the upper-cased dispatch
//  @ 0x58B80E..0x58B881; the mask tests the name as written for ".PCX"
//  @ 0x58B8A5 and shifts every word left 24, ORing 0xFFFFFF @ 0x58B8D7..0x58B8DA]
TextureLoad plain_texture_load(std::string_view name) {
	TextureLoad load = plain_dispatch(name);
	if (load.reader == TextureReader::Pcx && name.find(".PCX") != std::string_view::npos)
		load.transform = TextureLoadTransform::WhiteAlphaFromBlue;
	return load;
}

// [orig: Texture_LoadFromArchive @ 0x58B980 — the loose-first test @ 0x58B9E6..0x58B9EF,
//  the .dds sibling (the whole name to its last '.') @ 0x58BA1B..0x58BA50 decoded as DDS
//  @ 0x58BA66; the upper-cased .TGA / .MDT / .PCX dispatch @ 0x58BB17..0x58BB85; the
//  alpha name's ".PCX" test @ 0x58BBEC and existence test @ 0x58BC07, its 8-bit read's
//  palette luminance as each pixel's alpha @ 0x58BC21..0x58BCFB]
TextureLoad archive_texture_load(std::string_view name, bool self_alpha, bool loose_first_hit,
		bool dds_exists) {
	if (!loose_first_hit && dds_exists)
		return one_reader(material_dds_sibling(name), TextureReader::Dds);
	TextureLoad load = plain_dispatch(name);
	if (load.reader == TextureReader::Pcx && self_alpha)
		load.transform = TextureLoadTransform::PaletteLuminanceAlpha;
	return load;
}

// [orig: Texture_LoadFromFile_0 @ 0x58FE00 — the upper-cased ".TGA" test @ 0x58FE4A,
//  every other name through Texture_LoadPCXFromPFF32 @ 0x58FEC7; flag 0x200000's
//  shift left 24 | 0xFFFFFF @ 0x58FED3..0x58FEFA]
TextureLoad file_texture_load(std::string_view name, bool white_alpha) {
	if (holds(strutil::to_upper(name), ".TGA")) return one_reader(name, TextureReader::Tga);
	TextureLoad load = one_reader(name, TextureReader::Pcx);
	if (white_alpha) load.transform = TextureLoadTransform::WhiteAlphaFromBlue;
	return load;
}

// [orig: sub_591750 @ 0x591750 — the upper-cased name's ".FULL" @ 0x59179A and
//  ".ALPHA" @ 0x5917B7..0x5917BD cut off and overriding the mode, the existence test
//  @ 0x5917DF; HUD_LoadImageAsTexture @ 0x591550 — ".TGA" @ 0x5915AB, ".PCX"
//  @ 0x5915CE, the PCX turned white with its blue as alpha @ 0x59160F..0x59163E, the
//  alpha-only A8 copy @ 0x5916AE..0x5916BE, colour kept @ 0x5916FB..0x59170B]
TextureLoad hud_texture_load(std::string_view name, bool alpha_mode) {
	std::string file(name);
	const std::string upper = strutil::to_upper(name);
	bool alpha = alpha_mode;
	if (const size_t full = upper.find(".FULL"); full != std::string::npos) {
		alpha = false;
		file.resize(full);
	} else if (const size_t cut = upper.find(".ALPHA"); cut != std::string::npos) {
		alpha = true;
		file.resize(cut);
	}
	const std::string file_upper = strutil::to_upper(file);
	TextureLoad load;
	if (holds(file_upper, ".TGA")) {
		load.reader = TextureReader::Tga;
	} else if (holds(file_upper, ".PCX")) {
		load.reader = TextureReader::Pcx;
		load.transform = TextureLoadTransform::WhiteAlphaFromBlue;
	} else {
		return load;
	}
	load.file = std::move(file);
	load.alpha_only = alpha;
	return load;
}

void apply_texture_load_transform(TextureLoadTransform transform, bool alpha_only, uint8_t *rgba,
		size_t pixels) {
	if (rgba == nullptr) return;
	for (size_t i = 0; i < pixels; ++i) {
		uint8_t *p = rgba + 4 * i;
		if (transform == TextureLoadTransform::WhiteAlphaFromBlue) {
			p[3] = p[2];
			p[0] = p[1] = p[2] = 0xFF;
		}
		if (alpha_only) p[0] = p[1] = p[2] = 0xFF;
	}
}

// [orig: GTexture_DownsampleToLimits @ 0x687170 — the cap (flag 0x1000 -> 512, 0x2000 ->
//  256, 0x4000 -> 128) halving both sides while either exceeds it; each halving
//  GTexture_Downsample2x2_RGBA8 @ 0x687000, every channel the truncated mean of a 2x2
//  block read at twice the new width's stride; a model normal map loads with 0x1000,
//  Material_LoadStageTexture @ 0x5B1782]
void halve_rgba_to_cap(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, uint32_t cap) {
	while ((width > cap || height > cap) && width > 0 && height > 0) {
		const uint32_t w = width >> 1, h = height >> 1;
		std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
		const size_t stride = static_cast<size_t>(2) * w; // the source row as the halving reads it
		const auto at = [&](size_t pixel, int channel) -> uint32_t {
			const size_t offset = pixel * 4 + static_cast<size_t>(channel);
			return offset < rgba.size() ? rgba[offset] : 0u;
		};
		for (uint32_t y = 0; y < h; ++y) {
			for (uint32_t x = 0; x < w; ++x) {
				const size_t top = static_cast<size_t>(y) * 2 * stride + static_cast<size_t>(x) * 2;
				for (int c = 0; c < 4; ++c) {
					const uint32_t sum = at(top, c) + at(top + 1, c) + at(top + stride, c) +
							at(top + stride + 1, c);
					out[(static_cast<size_t>(y) * w + x) * 4 + static_cast<size_t>(c)] =
							static_cast<uint8_t>(sum >> 2);
				}
			}
		}
		rgba = std::move(out);
		width = w;
		height = h;
	}
}

} // namespace opennova::renderer
