#include <editor/documents/texture_load_rules.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <formats/trn/trn.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

namespace {

bool holds(const std::string &upper, const char *what) { return upper.find(what) != std::string::npos; }

TextureFileReader reader_of(renderer::MaterialTextureReader reader) {
	switch (reader) {
	case renderer::MaterialTextureReader::Dds: return TextureFileReader::Dds;
	case renderer::MaterialTextureReader::Tga: return TextureFileReader::Tga;
	case renderer::MaterialTextureReader::Pcx: return TextureFileReader::Pcx;
	case renderer::MaterialTextureReader::Chunk: return TextureFileReader::Chunk;
	case renderer::MaterialTextureReader::None: break;
	}
	return TextureFileReader::None;
}

// The name up to its last '.', plus ".dds" (a loader's DDS sibling).
std::string dds_sibling(const std::string &name) {
	const size_t dot = name.find_last_of('.');
	return (dot == std::string::npos ? name : name.substr(0, dot)) + ".dds";
}

// ARCHIVE [orig: Texture_LoadFromArchive @ 0x58B980]: the .dds sibling (the name cut at its last '.',
// @ 0x58BA1B..0x58BA46) when the files hold it (@ 0x58BA50), through the DDS reader; else the name
// upper-cased holding .TGA or .MDT through the TGA reader, else holding .PCX through the PCX reader
// (@ 0x58BB0B..0x58BB75), whose alpha is the luminance of the second name's palette when that is a .PCX
// the files hold (@ 0x58BBD9..0x58BCEE); else nothing.
TextureLoad archive_load(const std::string &name, const TextureNameTest &exists) {
	TextureLoad out;
	const std::string sibling = dds_sibling(name);
	if (exists && exists(sibling)) {
		out.file = sibling;
		out.reader = TextureFileReader::Dds;
		return out;
	}
	const std::string upper = strutil::to_upper(name);
	if (holds(upper, ".TGA") || holds(upper, ".MDT")) {
		out.file = name;
		out.reader = TextureFileReader::Tga;
	} else if (holds(upper, ".PCX")) {
		out.file = name;
		out.reader = TextureFileReader::Pcx;
		if (exists && exists(name)) {
			out.transform = TextureLoadTransform::LuminanceAlpha;
			out.alpha_source = name;
		}
	}
	return out;
}

// HUD [orig: sub_591750 @ 0x591750, HUD_LoadImageAsTexture @ 0x591550]: the name upper-cased, a .FULL
// suffix cut (colour) or else an .ALPHA one (alpha only) overriding the caller's mode (@ 0x59179A..
// 0x5917CE); nothing unless the files hold what is left (@ 0x5917DF); a .TGA through the TGA reader, a
// .PCX through the PCX reader turned white with alpha from blue (@ 0x591615..0x591644); alpha mode
// keeps the alpha alone (an A8 texture @ 0x591674..0x5916C4).
TextureLoad hud_load(const std::string &name, const TextureNameTest &exists, int alpha_mode) {
	TextureLoad out;
	// The name as written cut where its upper-cased copy is (the archive's lookup takes no case).
	std::string upper = strutil::to_upper(name);
	std::string file = name;
	bool alpha = alpha_mode != 0;
	if (const size_t full = upper.find(".FULL"); full != std::string::npos) {
		upper.resize(full);
		file.resize(full);
		alpha = false;
	} else if (const size_t only = upper.find(".ALPHA"); only != std::string::npos) {
		upper.resize(only);
		file.resize(only);
		alpha = true;
	}
	if (!exists || !exists(file)) return out;
	if (holds(upper, ".TGA")) {
		out.reader = TextureFileReader::Tga;
	} else if (holds(upper, ".PCX")) {
		out.reader = TextureFileReader::Pcx;
		out.transform = TextureLoadTransform::WhiteAlphaFromBlue;
	} else {
		return out;
	}
	out.file = file;
	if (alpha) out.transform = TextureLoadTransform::AlphaOnly;
	return out;
}

} // namespace

const char *texture_file_reader_token(TextureFileReader reader) {
	switch (reader) {
	case TextureFileReader::Tga: return "tga";
	case TextureFileReader::Pcx: return "pcx";
	case TextureFileReader::Dds: return "dds";
	case TextureFileReader::Png: return "png";
	case TextureFileReader::Pcx8: return "pcx8";
	case TextureFileReader::Chunk: return "chunk";
	case TextureFileReader::None: break;
	}
	return "none";
}

const char *texture_load_transform_token(TextureLoadTransform transform) {
	switch (transform) {
	case TextureLoadTransform::None: return "none";
	case TextureLoadTransform::LuminanceAlpha: return "luminance_alpha";
	case TextureLoadTransform::WhiteAlphaFromBlue: return "white_alpha_from_blue";
	case TextureLoadTransform::AlphaOnly: return "alpha_only";
	case TextureLoadTransform::NormalFromHeight: return "normal_from_height";
	case TextureLoadTransform::kCount: break;
	}
	return "none";
}

const char *texture_load_transform_words(TextureLoadTransform transform) {
	switch (transform) {
	case TextureLoadTransform::LuminanceAlpha: return "its alpha the brightness of its palette's colours";
	case TextureLoadTransform::WhiteAlphaFromBlue: return "white, its alpha its blue";
	case TextureLoadTransform::AlphaOnly: return "its alpha alone, tinted by the HUD colour";
	case TextureLoadTransform::NormalFromHeight: return "a normal map made from the height in its alpha";
	case TextureLoadTransform::None:
	case TextureLoadTransform::kCount: break;
	}
	return "";
}

TextureLoad texture_load(TextureLoader loader, std::string_view written, const TextureNameTest &exists, uint8_t row_type,
                         int alpha_mode) {
	const std::string name(written);
	TextureLoad out;
	if (name.empty()) return out;
	const std::string upper = strutil::to_upper(name);
	switch (loader) {
	case TextureLoader::Stage:
	case TextureLoader::Plain:
	case TextureLoader::Normal:
	case TextureLoader::Producer:
	case TextureLoader::Chunk: {
		// The model row's dispatcher (renderer::material_texture_source) by the row's runtime type; a
		// role named without a row (a terrain's detail, a scar) passes the STAGE type 0.
		uint8_t type = row_type;
		if (loader == TextureLoader::Plain) type = 1;
		const renderer::MaterialTextureSource source =
		        renderer::material_texture_source(name, renderer::material_texture_runtime_type(type), exists);
		out.file = source.file;
		out.reader = reader_of(source.reader);
		const uint8_t runtime = renderer::material_texture_runtime_type(type);
		// PLAIN's upper-case .PCX as written: white, alpha from blue [orig: Texture_LoadAndRegister @
		// 0x58B8A5..0x58B8F7].
		if (runtime == 1 && name.find(".PCX") != std::string::npos && out.reader == TextureFileReader::Pcx)
			out.transform = TextureLoadTransform::WhiteAlphaFromBlue;
		// A normal map's .tga (not its .mdt) is converted from its height [orig: Texture_LoadAsNormalMap
		// @ 0x58C480] (renderer::material_texture_transform).
		if (renderer::material_texture_transform(runtime, name, true) ==
		    renderer::MaterialTextureTransform::NormalFromAlpha)
			out.transform = TextureLoadTransform::NormalFromHeight;
		return out;
	}
	case TextureLoader::Archive: return archive_load(name, exists);
	case TextureLoader::Hud: return hud_load(name, exists, alpha_mode);
	case TextureLoader::File:
		// FILE [orig: Texture_LoadFromFile_0 @ 0x58FE00]: .TGA through the TGA reader, any other name
		// through the PCX reader (@ 0x58FE5F..0x58FEC7).
		out.file = name;
		out.reader = holds(upper, ".TGA") ? TextureFileReader::Tga : TextureFileReader::Pcx;
		return out;
	case TextureLoader::Menu: {
		const menu::MenuTextureSource source = menu::menu_texture_source(name, exists);
		out.file = source.file;
		switch (source.format) {
		case menu::MenuTextureFormat::Tga: out.reader = TextureFileReader::Tga; break;
		case menu::MenuTextureFormat::Dds: out.reader = TextureFileReader::Dds; break;
		case menu::MenuTextureFormat::Pcx: out.reader = TextureFileReader::Pcx; break;
		case menu::MenuTextureFormat::Png: out.reader = TextureFileReader::Png; break;
		case menu::MenuTextureFormat::None: out.file.clear(); break;
		}
		return out;
	}
	case TextureLoader::Ptl:
	case TextureLoader::Tga:
		out.file = name;
		out.reader = TextureFileReader::Tga;
		return out;
	case TextureLoader::Pcx:
		out.file = name;
		out.reader = TextureFileReader::Pcx;
		return out;
	case TextureLoader::Pcx8:
		out.file = name;
		out.reader = TextureFileReader::Pcx8;
		return out;
	case TextureLoader::Cube:
		out.file = name;
		out.reader = TextureFileReader::Dds;
		return out;
	case TextureLoader::kCount: break;
	}
	return out;
}

TextureLoad texture_reference_load(std::string_view written, int32_t loader_arg, const TextureNameTest &exists) {
	if (texture_arg_is_row_type(loader_arg))
		return texture_load(TextureLoader::Stage, written, exists, static_cast<uint8_t>(loader_arg));
	TextureRoleId role = TextureRoleId::kCount;
	if (!texture_arg_role(loader_arg, role)) {
		TextureLoad out;
		out.file = std::string(written);
		return out;
	}
	std::string name(written);
	// A mission's tile set: its name with its extension from the first '.' replaced by, or else
	// appended as, TGA [orig: Terrain_LoadEnvironmentConfig @ 0x6109EE, Path_ReplaceOrAppendExtension @
	// 0x53C780] (formats/trn).
	if (loader_arg & kTextureArgTileSet) name = trn_mission_tilestrip(TrnConfig{}, name);
	const TextureRoleRow &row = texture_role_row(role);
	return texture_load(row.loader, name, exists, 0, role == TextureRoleId::HudAlphaOnly ? 1 : 0);
}

std::shared_ptr<const TextureImage> apply_load_transform(const TextureImage &image, TextureLoadTransform transform,
                                                         const TextureImage *alpha_source) {
	auto out = std::make_shared<TextureImage>(image);
	switch (transform) {
	case TextureLoadTransform::None:
	case TextureLoadTransform::kCount: break;
	case TextureLoadTransform::LuminanceAlpha: {
		// [orig: Texture_LoadFromArchive @ 0x58BC35..0x58BCEE]: per palette entry A = (85 x (r + g + b))
		// >> 8 of the second PCX's palette, each texel's by its index there.
		const TextureImage &source = alpha_source ? *alpha_source : image;
		if (source.indices.empty() || source.palette.size() < 768 || out->levels.empty()) break;
		uint8_t luminance[256];
		for (size_t i = 0; i < 256; ++i)
			luminance[i] = uint8_t(uint16_t(85u * (source.palette[i * 3] + source.palette[i * 3 + 1] + source.palette[i * 3 + 2])) >> 8);
		std::vector<uint8_t> &rgba = out->levels.front().rgba;
		for (size_t i = 0; i < rgba.size() / 4 && i < source.indices.size(); ++i) rgba[i * 4 + 3] = luminance[source.indices[i]];
		break;
	}
	case TextureLoadTransform::WhiteAlphaFromBlue:
		// [orig: HUD_LoadImageAsTexture @ 0x591615..0x591644]: each A8R8G8B8 texel shifted up 24 and or'd
		// with 0xFFFFFF: white, its alpha the blue byte.
		for (TextureLevel &level : out->levels)
			for (size_t i = 0; i < level.rgba.size() / 4; ++i) {
				const uint8_t blue = level.rgba[i * 4 + 2];
				level.rgba[i * 4] = level.rgba[i * 4 + 1] = level.rgba[i * 4 + 2] = 0xFF;
				level.rgba[i * 4 + 3] = blue;
			}
		break;
	case TextureLoadTransform::AlphaOnly:
		// [orig: HUD_LoadImageAsTexture @ 0x591674..0x5916C4]: the alpha alone (an A8 texture, which
		// samples as black with that alpha).
		for (TextureLevel &level : out->levels)
			for (size_t i = 0; i < level.rgba.size() / 4; ++i) level.rgba[i * 4] = level.rgba[i * 4 + 1] = level.rgba[i * 4 + 2] = 0;
		break;
	case TextureLoadTransform::NormalFromHeight:
		// [orig: Texture_LoadAsNormalMap @ 0x58C985..0x58CAED] (renderer::normal_map_from_height_rgba,
		// the height in A, B the output alpha, at the device's scale).
		for (TextureLevel &level : out->levels)
			if (level.width && level.height)
				level.rgba = renderer::normal_map_from_height_rgba(level.rgba.data(), level.width, level.height, 1.0f / 64.0f, 3, 2);
		break;
	}
	return out;
}

} // namespace opennova::editor
