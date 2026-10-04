#include <editor/documents/texture_load_rules.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <formats/trn/trn.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_load_rules.h>

namespace opennova::editor {

namespace {

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

// A load of the game's rules in the editor's words: the reader, and what the loader makes of the texels
// (a PCX's palette luminance its alpha, the loader decoding it so; the HUD's alpha alone).
TextureLoad from_renderer(const renderer::TextureLoad &load) {
	TextureLoad out;
	out.file = load.file;
	switch (load.reader) {
	case renderer::TextureReader::Dds: out.reader = TextureFileReader::Dds; break;
	case renderer::TextureReader::Tga:
	case renderer::TextureReader::TgaParticleLoose: out.reader = TextureFileReader::Tga; break;
	case renderer::TextureReader::Pcx: out.reader = TextureFileReader::Pcx; break;
	case renderer::TextureReader::Png: out.reader = TextureFileReader::Png; break;
	case renderer::TextureReader::None: break;
	}
	switch (load.transform) {
	case renderer::TextureLoadTransform::WhiteAlphaFromBlue: out.transform = TextureLoadTransform::WhiteAlphaFromBlue; break;
	case renderer::TextureLoadTransform::PaletteLuminanceAlpha:
		out.transform = TextureLoadTransform::LuminanceAlpha;
		out.alpha_source = load.file;
		break;
	case renderer::TextureLoadTransform::None: break;
	}
	// The HUD's alpha mode keeps the alpha alone, after the colour transform: a PCX's alpha is its blue
	// [orig: HUD_LoadImageAsTexture @ 0x59160F..0x59163E, then the A8 copy @ 0x5916AE..0x5916BE].
	if (load.alpha_only)
		out.transform = out.transform == TextureLoadTransform::WhiteAlphaFromBlue ? TextureLoadTransform::BlueAlphaOnly
		                                                                         : TextureLoadTransform::AlphaOnly;
	return out;
}

// The game's loader's tries for `name` (renderer::texture_load_attempts) over the project's files: the
// first the files hold, else the first, each of the mounted set (the particle manager's loose tga\ leg is
// no project file).
TextureLoad attempts_load(renderer::TextureLoader loader, const std::string &name, const TextureNameTest &exists) {
	renderer::TextureFileQuery files;
	files.exists = [&exists](const std::string &file) { return exists && exists(file); };
	files.loose_first_hit = [](const std::string &) { return false; };
	const std::vector<renderer::TextureLoad> attempts = renderer::texture_load_attempts(loader, name, files);
	const renderer::TextureLoad *first = nullptr;
	for (const renderer::TextureLoad &attempt : attempts) {
		if (attempt.source != renderer::TextureFileSource::Mounted || attempt.file.empty()) continue;
		if (!first) first = &attempt;
		if (files.exists(attempt.file)) return from_renderer(attempt);
	}
	return first ? from_renderer(*first) : TextureLoad();
}

// A model texture row's load by its authored type: the file and reader the dispatcher's loader picks
// (renderer::material_texture_source), a type-1 row's upper-case .PCX white with its blue as alpha
// (renderer::material_texture_load), a normal map's .tga (not its .mdt) converted from its height
// [orig: Texture_LoadAsNormalMap @ 0x58C480] (renderer::material_texture_transform).
TextureLoad row_load(const std::string &name, uint8_t row_type, const TextureNameTest &exists) {
	const uint8_t runtime = renderer::material_texture_runtime_type(row_type);
	const renderer::MaterialTextureSource source = renderer::material_texture_source(name, runtime, exists);
	TextureLoad out;
	out.file = source.file;
	out.reader = reader_of(source.reader);
	if (renderer::material_texture_load(source, runtime).transform == renderer::TextureLoadTransform::WhiteAlphaFromBlue)
		out.transform = TextureLoadTransform::WhiteAlphaFromBlue;
	if (renderer::material_texture_transform(runtime, name, true) == renderer::MaterialTextureTransform::NormalFromAlpha)
		out.transform = TextureLoadTransform::NormalFromHeight;
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
	case TextureLoadTransform::BlueAlphaOnly: return "blue_alpha_only";
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
	case TextureLoadTransform::BlueAlphaOnly: return "its blue alone as alpha, tinted by the HUD colour";
	case TextureLoadTransform::None:
	case TextureLoadTransform::kCount: break;
	}
	return "";
}

TextureLoad texture_load(TextureLoader loader, std::string_view written, const TextureNameTest &exists, uint8_t row_type,
                         int alpha_mode, TextureRoleId role) {
	const std::string name(written);
	TextureLoad out;
	if (name.empty()) return out;
	switch (loader) {
	case TextureLoader::Stage:
	case TextureLoader::Plain:
	case TextureLoader::Normal:
	case TextureLoader::Producer:
	case TextureLoader::Chunk:
		// The model row's dispatcher by the row's runtime type; a role named without a row (a terrain's
		// detail, a weather drop) passes the STAGE type 0, PLAIN its type 1.
		return row_load(name, loader == TextureLoader::Plain ? uint8_t(1) : row_type, exists);
	case TextureLoader::Pcx8:
		// Read by its own name as 8-bit indices (a foliage or char map): no texture loader.
		out.file = name;
		out.reader = TextureFileReader::Pcx8;
		return out;
	case TextureLoader::Cube:
		out.file = name;
		out.reader = TextureFileReader::Dds;
		return out;
	case TextureLoader::kCount: return out;
	default: break;
	}
	renderer::TextureLoader by;
	if (!texture_role_renderer_loader(role, by, loader, alpha_mode)) return out;
	return attempts_load(by, name, exists);
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
	// A sky map: its extension made PCX first, the archive loader then trying its .dds and the .pcx
	// (kTextureArgPcx).
	if (loader_arg & kTextureArgPcx) name = menu::replace_or_append_extension(name, "pcx");
	return texture_load(texture_role_row(role).loader, name, exists, 0, -1, role);
}

std::shared_ptr<const TextureImage> apply_load_transform(const TextureImage &image, TextureLoadTransform transform,
                                                         const TextureImage *alpha_source) {
	auto out = std::make_shared<TextureImage>(image);
	switch (transform) {
	case TextureLoadTransform::None:
	case TextureLoadTransform::kCount: break;
	case TextureLoadTransform::LuminanceAlpha: {
		// [orig: Texture_LoadFromArchive @ 0x58BC35..0x58BCEE]: per palette entry A = (85 x (r + g + b))
		// >> 8 of the second PCX's palette, each texel's by its index there, as the game's 8-bit read of it
		// gives them (formats/pcx decode_pcx_luminance_alpha: TextureImage::luminance).
		const TextureImage &source = alpha_source ? *alpha_source : image;
		if (source.luminance.empty() || out->levels.empty()) break;
		std::vector<uint8_t> &rgba = out->levels.front().rgba;
		for (size_t i = 0; i < rgba.size() / 4 && i < source.luminance.size(); ++i) rgba[i * 4 + 3] = source.luminance[i];
		break;
	}
	case TextureLoadTransform::WhiteAlphaFromBlue:
	case TextureLoadTransform::AlphaOnly:
	case TextureLoadTransform::BlueAlphaOnly: {
		// As the game's loaders do it (renderer::apply_texture_load_transform): white with the blue as alpha;
		// the alpha alone, white, the form an A8 texture takes under the HUD's alpha material; both for a
		// PCX in the HUD's alpha mode.
		const bool blue = transform != TextureLoadTransform::AlphaOnly;
		const bool alone = transform != TextureLoadTransform::WhiteAlphaFromBlue;
		for (TextureLevel &level : out->levels)
			renderer::apply_texture_load_transform(blue ? renderer::TextureLoadTransform::WhiteAlphaFromBlue
			                                            : renderer::TextureLoadTransform::None,
			                                       alone, level.rgba.data(), level.rgba.size() / 4);
		break;
	}
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
