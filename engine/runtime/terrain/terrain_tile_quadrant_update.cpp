#include <runtime/terrain/terrain_tile_quadrant_update.h>

#include <runtime/renderer/device_texture.h>
#include <runtime/renderer/texture_dxt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace opennova::terrain {
namespace {

using Rect = TerrainTileQuadrantChange::Rect;

// The painted map as its texels lie: RGBA8, row-major, the top row first.
struct MapView {
	const uint8_t *rgba = nullptr;
	uint32_t width = 0, height = 0;
};

Rect clip(Rect r, int w, int h) {
	r.x0 = std::max(r.x0, 0);
	r.y0 = std::max(r.y0, 0);
	r.x1 = std::min(r.x1, w);
	r.y1 = std::min(r.y1, h);
	return r;
}

// The 4x4 blocks a rect falls in, cut at the level's edge (a DXT level's blocks are encoded alone:
// renderer::encode_dxt_surface).
Rect blocks_of(const Rect &r, int w, int h) {
	return clip(Rect{r.x0 & ~3, r.y0 & ~3, (r.x1 + 3) & ~3, (r.y1 + 3) & ~3}, w, h);
}

// The texels of the level below a `w` x `h` level whose box filter reads a texel of `r`: each reads the 2x2 at twice
// its place, a side of 1 its single texel (renderer::box_filter_half).
Rect parents_of(const Rect &r, int w, int h) {
	const Rect p{w == 1 ? 0 : r.x0 / 2, h == 1 ? 0 : r.y0 / 2, w == 1 ? 1 : (r.x1 + 1) / 2, h == 1 ? 1 : (r.y1 + 1) / 2};
	return clip(p, std::max(1, w / 2), std::max(1, h / 2));
}

// The texels of a `w` x `h` level the box filter reads for the texels `p` of the level below.
Rect children_of(const Rect &p, int w, int h) {
	return clip(Rect{w == 1 ? 0 : p.x0 * 2, h == 1 ? 0 : p.y0 * 2, w == 1 ? 1 : p.x1 * 2, h == 1 ? 1 : p.y1 * 2}, w, h);
}

std::vector<uint8_t> read_rect(const uint8_t *rgba, uint32_t width, const Rect &r, int origin_x = 0, int origin_y = 0) {
	const size_t row = size_t(r.x1 - r.x0) * 4;
	std::vector<uint8_t> out(row * size_t(r.y1 - r.y0));
	for (int y = r.y0; y < r.y1; ++y)
		std::memcpy(&out[size_t(y - r.y0) * row], &rgba[(size_t(y + origin_y) * width + size_t(r.x0 + origin_x)) * 4],
				row);
	return out;
}

void write_rect(Rgba8Image &image, const Rect &r, const std::vector<uint8_t> &texels) {
	const size_t row = size_t(r.x1 - r.x0) * 4;
	for (int y = r.y0; y < r.y1; ++y)
		std::memcpy(&image.pixels[(size_t(y) * image.width + size_t(r.x0)) * 4], &texels[size_t(y - r.y0) * row], row);
}

// The blocks of `surface` a block-aligned rect covers, as a surface of their own (its sides the rect's: a block past
// the level's edge decodes clipped, as the whole surface's does).
renderer::DxtSurface blocks_at(const renderer::DxtSurface &surface, const Rect &r) {
	const size_t bytes = renderer::dxt_block_bytes(surface.format);
	const uint32_t across = (surface.width + 3) / 4;
	renderer::DxtSurface out;
	out.format = surface.format;
	out.width = uint32_t(r.x1 - r.x0);
	out.height = uint32_t(r.y1 - r.y0);
	const uint32_t bx0 = uint32_t(r.x0) / 4, by0 = uint32_t(r.y0) / 4;
	const uint32_t bw = (out.width + 3) / 4, bh = (out.height + 3) / 4;
	out.blocks.resize(size_t(bw) * bh * bytes);
	for (uint32_t by = 0; by < bh; ++by)
		std::memcpy(&out.blocks[size_t(by) * bw * bytes], &surface.blocks[(size_t(by0 + by) * across + bx0) * bytes],
				size_t(bw) * bytes);
	return out;
}

void put_blocks(renderer::DxtSurface &surface, const Rect &r, const renderer::DxtSurface &blocks) {
	const size_t bytes = renderer::dxt_block_bytes(surface.format);
	const uint32_t across = (surface.width + 3) / 4;
	const uint32_t bx0 = uint32_t(r.x0) / 4, by0 = uint32_t(r.y0) / 4;
	const uint32_t bw = (blocks.width + 3) / 4, bh = (blocks.height + 3) / 4;
	for (uint32_t by = 0; by < bh; ++by)
		std::memcpy(&surface.blocks[(size_t(by0 + by) * across + bx0) * bytes], &blocks.blocks[size_t(by) * bw * bytes],
				size_t(bw) * bytes);
}

// A level's rect encoded from `colors` (its texels' decoded colours), its blocks put in the level and its texels read
// back as the device reads them (renderer::build_dxt_texture_levels, pixel_device_texture_levels) [orig:
// GTexture_CreateFromPixelData_0 @ 0x6878A0 (level 0), @ 0x6878B5..0x6878BE (each later level the box filter of the
// one before)].
void encode_rect(TerrainTileQuadrantChain &chain, size_t level, const Rect &r, const std::vector<renderer::DxtColor> &colors) {
	renderer::DxtSurface &surface = chain.dxt[level];
	const renderer::DxtSurface blocks =
			renderer::encode_dxt_surface(colors, uint32_t(r.x1 - r.x0), uint32_t(r.y1 - r.y0), surface.format);
	put_blocks(surface, r, blocks);
	write_rect(chain.levels[level], r, renderer::encode_rgba8(renderer::decode_dxt_surface(blocks)));
}

// The quadrant's base texels `base` (the new map's, the quadrant at (`origin_x`, `origin_y`) of `atlas`) carried
// through its chain over `r`, each level's changed texels into `changed`.
void update_chain(TerrainTileQuadrantChain &chain, const MapView &atlas, int origin_x, int origin_y, Rect r,
		std::vector<Rect> &changed) {
	changed.assign(chain.levels.size(), Rect{});
	const auto side = [&](size_t level, bool across) {
		return int(across ? chain.levels[level].width : chain.levels[level].height);
	};
	if (!chain.dxt.empty()) {
		r = blocks_of(r, side(0, true), side(0, false));
		encode_rect(chain, 0, r, renderer::decode_rgba8(read_rect(atlas.rgba, atlas.width, r, origin_x, origin_y).data(),
				uint32_t(r.x1 - r.x0), uint32_t(r.y1 - r.y0)));
		changed[0] = r;
		for (size_t level = 1; level < chain.levels.size(); ++level) {
			const int w = side(level - 1, true), h = side(level - 1, false);
			const Rect b = blocks_of(parents_of(changed[level - 1], w, h), side(level, true), side(level, false));
			const Rect c = children_of(b, w, h);
			const std::vector<renderer::DxtColor> filtered = renderer::box_filter_half(
					renderer::decode_dxt_surface(blocks_at(chain.dxt[level - 1], c)), uint32_t(c.x1 - c.x0),
					uint32_t(c.y1 - c.y0));
			encode_rect(chain, level, b, filtered);
			changed[level] = b;
		}
		return;
	}
	// A8R8G8B8: level 0 the base texels, every later level D3DX's box filter of the bytes the one before was stored as
	// (renderer::extend_box_chain).
	write_rect(chain.levels[0], r, read_rect(atlas.rgba, atlas.width, r, origin_x, origin_y));
	changed[0] = r;
	for (size_t level = 1; level < chain.levels.size(); ++level) {
		const int w = side(level - 1, true), h = side(level - 1, false);
		const Rect p = parents_of(changed[level - 1], w, h);
		const Rect c = children_of(p, w, h);
		write_rect(chain.levels[level], p,
				renderer::box_filter_half_rgba8(
						read_rect(chain.levels[level - 1].pixels.data(), chain.levels[level - 1].width, c).data(),
						uint32_t(c.x1 - c.x0),
						uint32_t(c.y1 - c.y0)));
		changed[level] = p;
	}
}

// The bilinear taps' texels (inclusive) a coordinate range [u0, u1] reaches on a side of `size` texels, as
// compose_terrain_tile_page's sample_bilinear_clamp takes them.
std::pair<int, int> taps(float u0, float u1, int size) {
	const auto first = [&](float u) {
		return int(std::floor(std::clamp(u * float(size) - 0.5f, 0.0f, float(size - 1))));
	};
	return {first(u0), std::min(first(u1) + 1, size - 1)};
}

} // namespace

unsigned update_terrain_tile_quadrants(TerrainTileQuadrantSource &source, const Rgba8Image &atlas, int x, int y, int w,
		int h, TerrainTileQuadrantChange *changed) {
	if (!atlas.is_valid()) {
		if (changed) *changed = TerrainTileQuadrantChange();
		return 0;
	}
	return update_terrain_tile_quadrants(source, atlas.pixels.data(), atlas.width, atlas.height, x, y, w, h, changed);
}

unsigned update_terrain_tile_quadrants(TerrainTileQuadrantSource &source, const uint8_t *rgba, uint32_t width,
		uint32_t height, int x, int y, int w, int h, TerrainTileQuadrantChange *changed) {
	if (changed) *changed = TerrainTileQuadrantChange();
	const MapView atlas{rgba, width, height};
	if (rgba == nullptr || width < 2 || height < 2 || w <= 0 || h <= 0) return 0;
	const int half_width = int(atlas.width / 2), half_height = int(atlas.height / 2);
	// The quadrant build's split (Terrain_SplitTileIntoQuadrants): a map of another size than the one the chains were
	// made from is none of this source's.
	for (size_t quadrant = 0; quadrant < 4; ++quadrant) {
		const std::shared_ptr<const TerrainTileQuadrantChain> &chain = source.quadrants[quadrant];
		if (!chain || chain->levels.empty()) return 0;
		const uint32_t halvings = renderer::pixel_texture_halvings(uint32_t(half_width), uint32_t(half_height),
				chain->creation_flags, renderer::kReferenceMaxTextureSide);
		if (chain->levels.front().width != std::max(1u, uint32_t(half_width) >> halvings) ||
				chain->levels.front().height != std::max(1u, uint32_t(half_height) >> halvings))
			return 0;
	}
	unsigned made = 0;
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		const int origin_x = (quadrant >> 1) * half_width, origin_y = (quadrant & 1) * half_height;
		const Rect r = clip(Rect{x - origin_x, y - origin_y, x + w - origin_x, y + h - origin_y}, half_width, half_height);
		if (r.empty()) continue;
		const TerrainTileQuadrantChain &old = *source.quadrants[size_t(quadrant)];
		std::shared_ptr<TerrainTileQuadrantChain> chain;
		std::vector<Rect> levels;
		if (old.levels.front().width != uint32_t(half_width) || old.levels.front().height != uint32_t(half_height)) {
			// A halved texture: its levels are no texel-for-texel image of the map; made again whole.
			Rgba8Image whole;
			whole.width = width;
			whole.height = height;
			whole.pixels.assign(rgba, rgba + size_t(width) * height * 4);
			chain = std::make_shared<TerrainTileQuadrantChain>(
					build_terrain_tile_quadrant_chain(whole, quadrant, old.creation_flags));
			for (const Rgba8Image &level : chain->levels) levels.push_back(Rect{0, 0, int(level.width), int(level.height)});
		} else {
			chain = std::make_shared<TerrainTileQuadrantChain>(old);
			update_chain(*chain, atlas, origin_x, origin_y, r, levels);
		}
		source.quadrants[size_t(quadrant)] = std::move(chain);
		if (changed) changed->levels[size_t(quadrant)] = std::move(levels);
		made |= 1u << quadrant;
	}
	if (changed) changed->quadrants = made;
	return made;
}

bool terrain_tile_page_samples(const TerrainTileQuadrantChange &change, const TerrainTileQuadrantSource &source,
		uint8_t page_lod_level, int32_t source_origin_x, int32_t source_origin_z, int dimension) {
	// The quadrant the page's origin routes to, and the level its footprint picks (compose_terrain_tile_page) [orig:
	// PolyTrn_RenderTile UV build @ 0x60DB62..0x60DC16; quadrant select @ 0x60DD1A..0x60DD28].
	const size_t quadrant = size_t((source_origin_x >= 512 ? 2 : 0) + (source_origin_z >= 512 ? 1 : 0));
	if (!((change.quadrants >> quadrant) & 1u) || dimension <= 0) return false;
	const std::vector<Rgba8Image> &levels = source.levels(quadrant);
	if (levels.empty()) return false;
	const bool flat = page_lod_level == 0;
	const int span = TerrainTileCompositionCache::page_world_span(page_lod_level);
	if (!flat && span <= 0) return false;
	const double uv_per_pixel = flat ? 0.0 : (double(span) / dimension) / 512.0;
	const size_t level =
			terrain_tile_nearest_level(static_cast<float>(uv_per_pixel * levels.front().width), levels.size());
	const std::vector<Rect> &made = change.levels[quadrant];
	if (level >= made.size() || made[level].empty()) return false;
	const Rect &r = made[level];
	// The first and the last pixel's coordinates over the span (the flat page: 0), and the taps they reach.
	const double local_x = flat ? 0.0 : double(source_origin_x & 511), local_z = flat ? 0.0 : double(source_origin_z & 511);
	const float u0 = static_cast<float>(local_x / 512.0), v0 = static_cast<float>(local_z / 512.0);
	const float u1 = static_cast<float>(local_x / 512.0 + (dimension - 1) * uv_per_pixel);
	const float v1 = static_cast<float>(local_z / 512.0 + (dimension - 1) * uv_per_pixel);
	const std::pair<int, int> across = taps(u0, u1, int(levels[level].width));
	const std::pair<int, int> down = taps(v0, v1, int(levels[level].height));
	return across.first < r.x1 && across.second >= r.x0 && down.first < r.y1 && down.second >= r.y0;
}

} // namespace opennova::terrain
