#include <editor/preview/texture_role_view.h>

#include <cstdio>

#include <formats/foliage/foliage.h>
#include <formats/particle/particle.h>
#include <runtime/renderer/particle_atlas.h>
#include <runtime/terrain/texture_preprocess.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

constexpr int kParticleModes = particle::kBlendModeCount; // Blend to Distort

std::string percent_words(double share) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.1f%%", share * 100.0);
	return text;
}

bool level_whole(const TextureLevel &level) {
	return level.width && level.height && level.rgba.size() == size_t(level.width) * level.height * 4;
}

// A blend map's weights as the terrain makes them: each texel's red, green and blue scaled to sum to 255 by the
// game's integer path, a texel of none made red, its alpha kept [orig: PolyTrn_InitTextures @ 0x60B1F0..0x60B24D;
// terrain::normalize_detail_blend_map].
std::shared_ptr<const TextureImage> blend_weights(const std::shared_ptr<const TextureImage> &image) {
	const TextureLevel &first = image->levels.front();
	terrain::Rgba8Image map;
	map.width = first.width;
	map.height = first.height;
	map.pixels = first.rgba;
	const terrain::Rgba8Image weights = terrain::normalize_detail_blend_map(map);
	if (!weights.is_valid()) return image;
	auto out = std::make_shared<TextureImage>(*image);
	TextureLevel level;
	level.width = weights.width;
	level.height = weights.height;
	level.rgba = weights.pixels;
	// The game builds its chain from these (the viewport's level builds it again from the first).
	out->levels.assign(1, std::move(level));
	out->indices.clear();
	return out;
}

// A particle graphic as its atlas page holds it (renderer::particle_atlas_paged_frame: an additive one's alpha
// cleared, a bump's or a distortion's page made a normal map of its blue); the image itself where no page holds it.
std::shared_ptr<const TextureImage> particle_page_texels(const std::shared_ptr<const TextureImage> &image, int mode) {
	const TextureLevel &first = image->levels.front();
	renderer::ParticleRgbaImage frame;
	frame.width = int(first.width);
	frame.height = int(first.height);
	frame.rgba = first.rgba;
	const renderer::ParticleRgbaImage paged = renderer::particle_atlas_paged_frame(frame, uint8_t(mode));
	if (!paged.valid()) return image;
	TextureLevel level;
	level.width = uint32_t(paged.width);
	level.height = uint32_t(paged.height);
	level.rgba = paged.rgba;
	auto out = std::make_shared<TextureImage>(*image);
	out->levels.assign(1, std::move(level));
	out->indices.clear();
	return out;
}

// The blend map's legend: each channel's mean weight and the splat detail the terrain names for it. The ps.1.4
// splat weighs the detail of stage 1 by the map's red, stage 4's by its green and stage 5's by its blue [orig:
// g_PolyTrnPS14Splat @ 0x7DEE18: mul r1, r1, r2.r; mad r1, r4, r2.g; mad r1, r5, r2.b], the three splat layers
// in their order [orig: PolyTrn_BindStageTextures @ 0x604330]; the splat is taken only with _c1 named [orig:
// PolyTrn_InitTextures @ 0x60ABDF].
TextureRoleView blend_legend(const TextureImage &used, const TrnConfig *terrain) {
	TextureRoleView out;
	out.title = "Its weights as the terrain's splat reads them: each texel's red, green and blue scaled to sum to 255 (a "
	            "texel of none is red)";
	const TextureLevel &first = used.levels.front();
	double sums[3] = {};
	const size_t texels = size_t(first.width) * first.height;
	for (size_t i = 0; i < texels; ++i)
		for (int c = 0; c < 3; ++c) sums[c] += first.rgba[i * 4 + size_t(c)];
	static const char *const kKeys[] = {"red", "green", "blue"};
	static const char *const kFields[] = {"polytrn_detailmap_c1", "polytrn_detailmap_c2", "polytrn_detailmap_c3"};
	const std::string *names[] = {terrain ? &terrain->detailmap_c1 : nullptr, terrain ? &terrain->detailmap_c2 : nullptr,
	                              terrain ? &terrain->detailmap_c3 : nullptr};
	for (int c = 0; c < 3; ++c) {
		TextureLegendRow row;
		row.key = kKeys[c];
		row.rgb[c] = 255;
		row.share = texels ? sums[c] / (255.0 * double(texels)) : 0.0;
		if (!terrain) row.words = std::string("weighs ") + kFields[c] + " (its terrain did not read)";
		else if (names[c]->empty()) row.words = std::string("weighs ") + kFields[c] + ", which the terrain does not name";
		else row.words = std::string("weighs ") + kFields[c] + ", " + *names[c];
		out.legend.push_back(std::move(row));
	}
	if (terrain && terrain->detailmap_c1.empty())
		out.words = "The terrain names no polytrn_detailmap_c1: the game draws no splat, and these weights go unread.";
	return out;
}

// The foliage map's legend: each code its texels hold (a palette index), its share, and the definitions of the
// terrain it selects: every one of whose four codes it is, code 0 none [orig: Foliage_RemapPixelToDefMask @
// 0x5FF4E0; foliage_remap_pixel_to_def_mask].
TextureRoleView foliage_legend(const TextureImage &source, const TrnConfig *terrain) {
	TextureRoleView out;
	out.title = "Its codes (each texel's palette index) and what grows on them";
	size_t counts[256] = {};
	for (const uint8_t index : source.indices) ++counts[index];
	const double texels = double(source.indices.size());
	for (int code = 0; code < 256; ++code) {
		if (!counts[code]) continue;
		TextureLegendRow row;
		row.key = std::to_string(code);
		if (size_t(code) * 3 + 2 < source.palette.size())
			for (int c = 0; c < 3; ++c) row.rgb[c] = source.palette[size_t(code) * 3 + size_t(c)];
		row.share = texels > 0.0 ? double(counts[code]) / texels : 0.0;
		if (code == 0) {
			row.words = "nothing grows: code 0 matches no definition";
		} else if (!terrain) {
			row.words = "its terrain did not read";
		} else {
			const uint32_t mask = foliage_remap_pixel_to_def_mask(terrain->foliage_defs, code);
			std::string grows;
			for (size_t slot = 0; slot < terrain->foliage_defs.size() && slot < 32; ++slot) {
				if (!(mask & (1u << slot))) continue;
				if (!grows.empty()) grows += ", ";
				grows += "foliage " + std::to_string(slot + 1) + " (" + terrain->foliage_defs[slot].graphic + ")";
			}
			row.words = grows.empty() ? "nothing grows: no definition of the terrain matches it" : "grows " + grows;
		}
		out.legend.push_back(std::move(row));
	}
	return out;
}

// A particle graphic's atlas page: the page side of its mode, and its share of an empty one [orig:
// CParticleManager_BuildTextureAtlases @ 0x5E8F19..0x5E8F20; renderer::particle_atlas_page_side, particle_atlas_fits].
TextureRoleView particle_page(const TextureImage &source, int mode) {
	TextureRoleView out;
	const int side = renderer::particle_atlas_page_side(uint8_t(mode));
	const std::string page = std::to_string(side) + " x " + std::to_string(side);
	const std::string mode_name = particle::blend_mode_name(particle::BlendMode(mode));
	if (!renderer::particle_atlas_fits(uint8_t(mode), int(source.width()), int(source.height()))) {
		out.words = "No " + page + " atlas page of its mode (" + mode_name +
		            ") holds it: it must be narrower than the page and no taller, and the game's atlas build never ends.";
		return out;
	}
	const double share = double(source.width()) * double(source.height()) / (double(side) * double(side));
	out.words = "Alone on a " + page + " atlas page of its mode (" + mode_name + ") it takes " + percent_words(share) +
	            " of the page";
	if (mode == int(particle::BlendMode::Additive)) out.words += "; the page holds it with its alpha cleared";
	else if (mode == int(particle::BlendMode::Bump) || mode == int(particle::BlendMode::Bumpadd))
		out.words += "; the page is made a normal map of its blue, read as a height";
	else if (mode == int(particle::BlendMode::Distort))
		out.words += "; the page is made a distortion map of its blue, read as a height";
	out.words += ".";
	return out;
}

} // namespace

bool TextureRoleView::operator==(const TextureRoleView &other) const {
	if (title != other.title || words != other.words || legend.size() != other.legend.size()) return false;
	for (size_t i = 0; i < legend.size(); ++i) {
		const TextureLegendRow &a = legend[i], &b = other.legend[i];
		if (a.key != b.key || a.words != b.words || a.share != b.share || a.rgb[0] != b.rgb[0] || a.rgb[1] != b.rgb[1] ||
		    a.rgb[2] != b.rgb[2])
			return false;
	}
	return true;
}

std::shared_ptr<const TextureImage> texture_role_texels(const std::shared_ptr<const TextureImage> &image, renderer::TextureRoleId role,
                                                        int blend_mode) {
	if (!image || !image->decoded || image->levels.empty() || !level_whole(image->levels.front())) return image;
	if (role == renderer::TextureRoleId::TerrainBlendMap) return blend_weights(image);
	if (role == renderer::TextureRoleId::ParticleGraphic && blend_mode >= 0 && blend_mode < kParticleModes)
		return particle_page_texels(image, blend_mode);
	return image;
}

TextureRoleView texture_role_view(const TextureImage &source, const TextureImage &used, renderer::TextureRoleId role, int blend_mode,
                                  const TrnConfig *terrain) {
	if (!source.decoded || source.levels.empty() || used.levels.empty() || !level_whole(used.levels.front())) return {};
	if (role == renderer::TextureRoleId::TerrainBlendMap) return blend_legend(used, terrain);
	if (role == renderer::TextureRoleId::TerrainFoliageMap && !source.indices.empty()) return foliage_legend(source, terrain);
	if (role == renderer::TextureRoleId::ParticleGraphic && blend_mode >= 0 && blend_mode < kParticleModes)
		return particle_page(source, blend_mode);
	return {};
}

io::JsonValue texture_role_view_json(const TextureRoleView &view) {
	JsonValue out = JsonValue::make_object();
	out.set("title", json_string(view.title));
	JsonValue legend = JsonValue::make_array();
	for (const TextureLegendRow &row : view.legend) {
		JsonValue item = JsonValue::make_object();
		item.set("key", json_string(row.key));
		JsonValue rgb = JsonValue::make_array();
		for (const uint8_t channel : row.rgb) rgb.push(json_number(channel));
		item.set("rgb", std::move(rgb));
		item.set("share", json_number(row.share));
		item.set("words", json_string(row.words));
		legend.push(std::move(item));
	}
	out.set("legend", std::move(legend));
	out.set("words", json_string(view.words));
	return out;
}

} // namespace opennova::editor
