#include <editor/documents/texture_budget.h>

#include <algorithm>
#include <cstdio>

#include <runtime/renderer/texture_compression.h>
#include <runtime/renderer/texture_dxt.h>

namespace opennova::editor {

namespace {

using renderer::DeviceTexture;
using renderer::DeviceTextureFormat;

io::JsonValue device_json(const DeviceTexture &texture, int level) {
	using io::JsonValue;
	JsonValue out = JsonValue::make_object();
	if (level >= 0) out.set("level", JsonValue::make_number(level));
	out.set("width", JsonValue::make_number(double(texture.width)));
	out.set("height", JsonValue::make_number(double(texture.height)));
	out.set("format", JsonValue::make_string(renderer::device_texture_format_name(texture.format)));
	out.set("levels", JsonValue::make_number(double(texture.levels)));
	out.set("bytes", JsonValue::make_number(double(texture.bytes)));
	out.set("stat_bytes", JsonValue::make_number(double(texture.stat_bytes)));
	out.set("halvings", JsonValue::make_number(double(texture.halvings)));
	out.set("whole", JsonValue::make_bool(texture.whole));
	out.set("dxt5_as_dxt1", JsonValue::make_bool(texture.dxt5_as_dxt1));
	return out;
}

// A texture the game builds from `width` x `height` pixels under `flags` in `format` (`bits` a texel uncompressed):
// halved and levelled as renderer::pixel_device_texture rules it, its format the pixel format's or, for flags
// 0x300, the DXT select_texture_dxt_format picks on the reference card [orig: GTexture_CreateFromPixelData_0
// @ 0x6876FE (the pixel format's), @ 0x687717..0x687766 (the DXT)].
DeviceTexture made_texture(uint32_t width, uint32_t height, uint32_t flags,
		DeviceTextureFormat format = DeviceTextureFormat::A8R8G8B8, uint32_t bits = 32) {
	DeviceTexture out = renderer::pixel_device_texture(width, height, flags);
	switch (renderer::select_texture_dxt_format(flags, renderer::kReferenceTextureDxtCaps)) {
	case renderer::TextureDxtFormat::Dxt1: format = DeviceTextureFormat::Dxt1; break;
	case renderer::TextureDxtFormat::Dxt5: format = DeviceTextureFormat::Dxt5; break;
	case renderer::TextureDxtFormat::None: break;
	}
	out.format = format;
	out.bits = format == DeviceTextureFormat::Uncompressed ? bits : 32;
	out.bytes = 0;
	for (uint32_t level = 0; level < out.levels; ++level)
		out.bytes += renderer::device_level_bytes(std::max(1u, out.width >> level), std::max(1u, out.height >> level),
				out.format, out.bits);
	out.stat_bytes = renderer::texture_stat_bytes(out.width, out.height, out.format, out.bits);
	return out;
}

// `texture` taken `count` times: every level's bytes and the counter's of all of them.
DeviceTexture times(DeviceTexture texture, uint32_t count) {
	texture.bytes *= count;
	texture.stat_bytes *= count;
	return texture;
}

// The terrain texture detail's halving word at `level` (terrain_texdetail 0 to 3) [orig: sub_605D70
// @ 0x605DBF..0x605E0A].
uint32_t terrain_texdetail_flags(int level) {
	return level <= 0 ? 0x20000u : level <= 2 ? 0x10000u : 0u;
}

// The compression word the terrain's DXT maps take at the fresh profile's texcompression_level [orig:
// PolyTrn_InitTextures @ 0x60ABAD..0x60ABC6]: the splat layers' (0x400100 at 0, else 0x400200) and the colour and
// blend maps' (0x400200 at 1 or less, else none).
constexpr int kCompressionLevel = renderer::kTexCompressionLevelFreshProfile;
uint32_t splat_word() {
	return kCompressionLevel >= 1 ? 0x400200u : 0x400100u;
}
uint32_t map_word() {
	return kCompressionLevel >= 2 ? 0u : 0x400200u;
}

// The side GTexture_ComputeTileSize makes a tile of for an image side: its power of two, at most the card's
// largest [orig: sub_679DF0 @ 0x679DF0].
uint32_t tile_side(uint32_t side) {
	uint32_t out = 1;
	while (out < side && out < renderer::kReferenceMaxTextureSide) out <<= 1;
	return std::min(out, renderer::kReferenceMaxTextureSide);
}

} // namespace

bool texture_role_has_budget(renderer::TextureRoleId role) {
	using R = renderer::TextureRoleId;
	renderer::TextureLoader loader;
	if (texture_role_budget_loader(role, loader)) return true;
	switch (role) {
	case R::HudColour:
	case R::HudAlphaOnly:
	case R::MenuImage:
	case R::MenuCursor:
	case R::MenuFrameStencil:
	case R::MenuFrameBrush:
	case R::TerrainColourMap:
	case R::TerrainBlendMap:
	case R::TerrainSplatDetail:
	case R::TerrainSecondDetail:
	case R::TerrainFarDetail:
	case R::TerrainTileAtlas: return true;
	default: return false;
	}
}

TextureBudget texture_role_budget(const TextureHeader &header, const std::string &file, renderer::TextureRoleId role) {
	using R = renderer::TextureRoleId;
	TextureBudget out;
	if (!header.read || header.width == 0 || header.height == 0 || !texture_role_has_budget(role)) return out;
	renderer::TextureLoader loader;
	if (texture_role_budget_loader(role, loader)) return out; // a model row's: texture_budget
	out.known = true;
	out.role = role;
	out.file = file;
	out.setting.clear();
	const uint32_t w = header.width, h = header.height;
	const auto alike = [&](const DeviceTexture &texture) {
		for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level) out.detail[level] = texture;
	};
	// A stage loader's texture of the terrain's detail family at each terrain texture detail level: the file's DDS
	// through D3DX, else its pixels.
	const auto detail_family = [&](uint32_t word) {
		out.loader = renderer::TextureLoader::Stage;
		out.setting = "terrain_texdetail";
		const bool dds = header.reader == TextureReader::Dds;
		renderer::DdsSource source;
		source.width = w;
		source.height = h;
		if (dds) {
			source.levels = header.dds_levels;
			source.format = renderer::dds_device_format(header.dds_format);
			source.bits = header.dds_bits ? header.dds_bits : 32;
			source.dxt5_opaque = header.dds_dxt5_opaque;
		}
		for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level) {
			const uint32_t flags = word | 0x8u | terrain_texdetail_flags(level);
			out.detail[level] = dds ? renderer::dds_device_texture(source, flags) : made_texture(w, h, flags);
		}
	};
	switch (role) {
	case R::HudColour: alike(made_texture(w, h, 0x140000u)); break;
	case R::HudAlphaOnly: alike(made_texture(w, h, 0x140000u, DeviceTextureFormat::Uncompressed, 8)); break;
	case R::MenuImage:
	case R::MenuCursor: {
		const uint32_t tw = tile_side(w), th = tile_side(h);
		out.count = ((w + tw - 1) / tw) * ((h + th - 1) / th);
		alike(times(made_texture(tw, th, 0x140001u), out.count));
		break;
	}
	case R::MenuFrameStencil: alike(made_texture(w, h, 0x140000u)); break;
	case R::MenuFrameBrush: alike(made_texture(w, h, 0x40000u)); break;
	case R::TerrainColourMap:
	case R::TerrainBlendMap: {
		const uint32_t side = std::max(1u, w >> 1);
		out.count = 4;
		alike(times(made_texture(side, side, 0x100001u | map_word()), 4));
		break;
	}
	case R::TerrainSplatDetail: detail_family(splat_word()); break;
	case R::TerrainSecondDetail: detail_family(0); break;
	case R::TerrainFarDetail: out.count = 0; alike(DeviceTexture()); break;
	case R::TerrainTileAtlas: alike(made_texture(w, h, 0x100203u)); break;
	default: out = TextureBudget(); break;
	}
	return out;
}

bool texture_role_budget_loader(renderer::TextureRoleId role, renderer::TextureLoader &out) {
	switch (role) {
	case renderer::TextureRoleId::ModelDiffuse:
	case renderer::TextureRoleId::ModelDetail:
	case renderer::TextureRoleId::ModelFlipFrame: out = renderer::TextureLoader::Stage; return true;
	case renderer::TextureRoleId::ModelPlain: out = renderer::TextureLoader::Plain; return true;
	case renderer::TextureRoleId::ModelNormalMap:
	case renderer::TextureRoleId::ModelHeightNormal: out = renderer::TextureLoader::Normal; return true;
	default: return false;
	}
}

// Each detail level's device texture as the row's loader makes it (renderer::model_row_device_texture: the
// stage loader alone reads a DDS through D3DX).
TextureBudget texture_budget(const TextureHeader &header, const std::string &file, renderer::TextureLoader loader, uint8_t slot) {
	TextureBudget out;
	if (!header.read || header.width == 0 || header.height == 0) return out;
	out.known = true;
	out.loader = loader;
	out.slot = slot;
	out.file = file;
	const bool dds = header.reader == TextureReader::Dds;
	renderer::DdsSource source;
	source.width = header.width;
	source.height = header.height;
	if (dds) {
		source.levels = header.dds_levels;
		source.format = renderer::dds_device_format(header.dds_format);
		source.bits = header.dds_bits ? header.dds_bits : 32;
		source.dxt5_opaque = header.dds_dxt5_opaque;
	}
	for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level)
		out.detail[level] = renderer::model_row_device_texture(loader, slot, level, source, dds);
	if (loader == renderer::TextureLoader::Stage && header.reader != TextureReader::Dds) {
		renderer::DdsSource made;
		made.width = header.width;
		made.height = header.height;
		made.levels = renderer::full_chain_levels(header.width, header.height);
		made.format = header.alpha ? DeviceTextureFormat::Dxt5 : DeviceTextureFormat::Dxt1;
		out.offers_dds = true;
		out.as_dds = renderer::dds_device_texture(made, 0);
	}
	return out;
}

std::string texture_bytes_words(uint64_t bytes) {
	char text[48];
	if (bytes >= uint64_t(1024) * 1024) std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
	else if (bytes >= 1024) std::snprintf(text, sizeof(text), "%.0f KB", double(bytes) / 1024.0);
	else std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
	return text;
}

std::string device_texture_words(const DeviceTexture &texture) {
	std::string format = renderer::device_texture_format_name(texture.format);
	if (texture.format == DeviceTextureFormat::A8R8G8B8) format += " (uncompressed)";
	if (texture.dxt5_as_dxt1) format += " (a DXT5 the game stores as DXT1, every block opaque)";
	return std::to_string(texture.width) + " x " + std::to_string(texture.height) + ", " + format + ", " +
	       std::to_string(texture.levels) + (texture.levels == 1 ? " level" : " levels") + ": " + texture_bytes_words(texture.bytes);
}

std::string texture_budget_words(const TextureBudget &budget) {
	if (!budget.known) return "";
	const DeviceTexture &full = budget.full();
	if (budget.role != renderer::TextureRoleId::kCount && budget.count == 0)
		return "Drawn into its near map's levels: no texture of its own.";
	if (budget.count > 1) {
		DeviceTexture one = full;
		one.bytes /= budget.count;
		return texture_bytes_words(full.bytes) + " in the game (" + std::to_string(budget.count) + " textures of " +
		       device_texture_words(one) + ")";
	}
	std::string out = texture_bytes_words(full.bytes) + " in the game (" + device_texture_words(full) + ")";
	if (budget.offers_dds)
		out += "; " + texture_bytes_words(budget.as_dds.bytes) + " as a " +
		       renderer::device_texture_format_name(budget.as_dds.format) + " .dds";
	return out;
}

io::JsonValue texture_budget_json(const TextureBudget &budget) {
	using io::JsonValue;
	JsonValue out = JsonValue::make_object();
	out.set("loader", JsonValue::make_string(texture_loader_token(budget.loader)));
	out.set("slot", JsonValue::make_number(budget.slot));
	out.set("file", JsonValue::make_string(budget.file));
	JsonValue detail = JsonValue::make_array();
	for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level) detail.push(device_json(budget.detail[level], level));
	out.set("detail", std::move(detail));
	out.set("as_dds", budget.offers_dds ? device_json(budget.as_dds, -1) : JsonValue::make_null());
	out.set("role", JsonValue::make_string(budget.role == renderer::TextureRoleId::kCount ? ""
	                                                                                        : texture_role_row(budget.role).token));
	out.set("count", JsonValue::make_number(double(budget.count)));
	out.set("setting", JsonValue::make_string(budget.setting));
	out.set("words", JsonValue::make_string(texture_budget_words(budget)));
	return out;
}

} // namespace opennova::editor
