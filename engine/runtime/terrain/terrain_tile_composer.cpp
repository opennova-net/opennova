#include <runtime/terrain/terrain_tile_composer.h>

// [orig: PolyTrn_RenderTile @ 0x60DA70; tile overlay submission
// PolyTrn_DrawTileOverlayQuad @ 0x604700; docs/tiles/til-re.md]

#include <runtime/renderer/texture_dxt.h>
#include <runtime/terrain/row_stripes.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace opennova::terrain {
namespace {

struct RgbaF {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 0.0f;
};

float clamp01(float value) {
	return std::clamp(value, 0.0f, 1.0f);
}

RgbaF pixel(const Rgba8Image &image, int x, int y) {
	x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
	y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
	const size_t offset = 4u * (static_cast<size_t>(y) * image.width + x);
	constexpr float inverse_byte = 1.0f / 255.0f;
	return {
		image.pixels[offset] * inverse_byte,
		image.pixels[offset + 1] * inverse_byte,
		image.pixels[offset + 2] * inverse_byte,
		image.pixels[offset + 3] * inverse_byte,
	};
}

RgbaF lerp(RgbaF a, RgbaF b, float t) {
	return {
		a.r + (b.r - a.r) * t,
		a.g + (b.g - a.g) * t,
		a.b + (b.b - a.b) * t,
		a.a + (b.a - a.a) * t,
	};
}

// MIPFILTER POINT: the level nearest the footprint's log2 texel density.
// A magnified or zero footprint stays on level 0.
std::size_t nearest_mip_level(float texels_per_pixel, std::size_t levels) {
	if (levels == 0 || !(texels_per_pixel > 1.0f)) return 0;
	const int level = static_cast<int>(
			std::floor(std::log2(texels_per_pixel) + 0.5f));
	return static_cast<std::size_t>(
			std::clamp(level, 0, static_cast<int>(levels) - 1));
}

// LINEAR min/mag filter under CLAMP addressing: the 2x2 footprint around
// (u*width - 0.5, v*height - 0.5), edge texels repeated.
RgbaF sample_bilinear_clamp(const Rgba8Image &image, float u, float v) {
	const float texel_x = std::clamp(
			u * static_cast<float>(image.width) - 0.5f, 0.0f,
			static_cast<float>(image.width - 1));
	const float texel_y = std::clamp(
			v * static_cast<float>(image.height) - 0.5f, 0.0f,
			static_cast<float>(image.height - 1));
	const int x0 = static_cast<int>(std::floor(texel_x));
	const int y0 = static_cast<int>(std::floor(texel_y));
	const float tx = texel_x - x0;
	const float ty = texel_y - y0;
	return lerp(lerp(pixel(image, x0, y0), pixel(image, x0 + 1, y0), tx),
			lerp(pixel(image, x0, y0 + 1), pixel(image, x0 + 1, y0 + 1), tx), ty);
}

// POINT min/mag filter under CLAMP addressing: the texel containing (u, v).
RgbaF sample_point_clamp(const Rgba8Image &image, float u, float v) {
	return pixel(image,
			static_cast<int>(std::floor(u * static_cast<float>(image.width))),
			static_cast<int>(std::floor(v * static_cast<float>(image.height))));
}

TilUv interpolate_uv(const TilUvQuad &quad, float x, float z) {
	const TilUv &tl = quad.corners[0];
	const TilUv &tr = quad.corners[1];
	const TilUv &bl = quad.corners[2];
	const TilUv &br = quad.corners[3];
	const TilUv top{tl.u + (tr.u - tl.u) * x, tl.v + (tr.v - tl.v) * x};
	const TilUv bottom{bl.u + (br.u - bl.u) * x, bl.v + (br.v - bl.v) * x};
	return {top.u + (bottom.u - top.u) * z,
			top.v + (bottom.v - top.v) * z};
}

uint8_t byte(float value) {
	return static_cast<uint8_t>(std::clamp(
			static_cast<int>(std::lround(clamp01(value) * 255.0f)), 0, 255));
}

// The first pixel position at or after `edge` on one raster axis.
int first_covered(double edge, int dimension) {
	return std::clamp(static_cast<int>(std::ceil(edge)), 0, dimension);
}

void compose_overlay_rgba(uint8_t *destination, RgbaF source,
		const std::array<float, 3> &tint) {
	const float alpha = clamp01(source.a);
	if (alpha <= 0.0f) return;
	constexpr float inverse_byte = 1.0f / 255.0f;
	const float destination_rgb[3] = {
		destination[0] * inverse_byte,
		destination[1] * inverse_byte,
		destination[2] * inverse_byte,
	};
	const float source_rgb[3] = {source.r, source.g, source.b};
	for (int channel = 0; channel < 3; ++channel) {
		// The stage colour MODULATE2X(TEXTURE, TintHalf) saturates before
		// the SRCALPHA/INVSRCALPHA blend.
		destination[channel] = byte(
				destination_rgb[channel] * (1.0f - alpha) +
				clamp01(source_rgb[channel] * tint[channel]) * alpha);
	}
	// Overlays never touch the page alpha: retail masks the alpha channel
	// off for the base pass and both ordered overlay loops
	// (SetRenderState(D3DRS_COLORWRITEENABLE, 7)) and restores RGBA writes
	// only for the DOT3/static alpha passes, so the blend's SRCALPHA math
	// lands on RGB alone and the final page alpha is purely the terrain
	// light term.
	// [orig: PolyTrn_RenderTile SetRenderState(0xA8, 7) @ 0x60DD04..0x60DD12
	// (pre-base) and @ 0x60DD6B..0x60DD73 (pre-overlay loops); 0xF restore
	// @ 0x60E0EA..0x60E0F2 / 0x60E1B6..0x60E1BE; overlay view mode 0x631 ->
	// SRCALPHA/INVSRCALPHA @ 0x680F2C..0x680F3A]
}

void add_dot3_alpha(Rgba8Image &output,
		const std::vector<uint8_t> &dot3_alpha) {
	const size_t pixel_count = static_cast<size_t>(output.width) * output.height;
	for (size_t index = 0; index < pixel_count; ++index) {
		const size_t alpha_offset = index * 4u + 3u;
		output.pixels[alpha_offset] = static_cast<uint8_t>(std::min(
				255, static_cast<int>(output.pixels[alpha_offset]) +
						static_cast<int>(dot3_alpha[index])));
	}
}

bool valid_chain(const std::vector<Rgba8Image> &chain) {
	return !chain.empty() && std::all_of(chain.begin(), chain.end(),
			[](const Rgba8Image &level) { return level.is_valid(); });
}

// Retail's level set for a power-of-two texture; a synthetic texture the box
// chain cannot halve keeps its single base level.
std::vector<Rgba8Image> retail_box_levels(const Rgba8Image &base) {
	std::vector<Rgba8Image> levels = build_box_mip_chain_to_4x4(base);
	if (levels.empty() && base.is_valid()) levels.push_back(base);
	return levels;
}

} // namespace

bool TerrainTileQuadrantSource::is_valid() const noexcept {
	return std::all_of(quadrants.begin(), quadrants.end(), valid_chain);
}

TerrainTileQuadrantSource build_terrain_tile_quadrant_source(
		const Rgba8Image &atlas) {
	TerrainTileQuadrantSource result;
	if (!atlas.is_valid() || atlas.width < 2 || atlas.height < 2) return result;
	const uint32_t half_width = atlas.width / 2;
	const uint32_t half_height = atlas.height / 2;
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		const uint32_t origin_x = (quadrant >> 1) * half_width;
		const uint32_t origin_y = (quadrant & 1) * half_height;
		Rgba8Image block;
		block.width = half_width;
		block.height = half_height;
		block.pixels.resize(static_cast<size_t>(half_width) * half_height * 4u);
		for (uint32_t row = 0; row < half_height; ++row) {
			const auto source = atlas.pixels.begin() +
					4u * (static_cast<size_t>(origin_y + row) * atlas.width + origin_x);
			std::copy(source, source + 4u * half_width,
					block.pixels.begin() + 4u * static_cast<size_t>(row) * half_width);
		}
		result.quadrants[static_cast<size_t>(quadrant)] = retail_box_levels(block);
	}
	return result;
}

std::vector<Rgba8Image> build_terrain_tile_set_mips(const Rgba8Image &atlas) {
	// [orig: Terrain_LoadTileSetAtlas @ 0x604B24 (flags 0x100203)]
	constexpr uint32_t kTileSetAtlasFlags = 0x100203u;
	const renderer::TextureDxtFormat format = renderer::select_texture_dxt_format(
			kTileSetAtlasFlags, renderer::kReferenceTextureDxtCaps);
	if (!atlas.is_valid() || format == renderer::TextureDxtFormat::None) {
		return retail_box_levels(atlas);
	}
	const std::vector<renderer::DxtSurface> levels = renderer::build_dxt_texture_levels(
			atlas.pixels.data(), atlas.width, atlas.height, format,
			renderer::texture_level_count(atlas.width, atlas.height, kTileSetAtlasFlags));
	std::vector<Rgba8Image> result;
	result.reserve(levels.size());
	for (const renderer::DxtSurface &level : levels) {
		Rgba8Image image;
		image.width = level.width;
		image.height = level.height;
		image.pixels = renderer::encode_rgba8(renderer::decode_dxt_surface(level));
		result.push_back(std::move(image));
	}
	return result;
}

Rgba8Image compose_terrain_tile_page(
		const TerrainTileCompositionJob &job,
		const TerrainTilePageSourceView &sources,
		std::size_t threads) {
	Rgba8Image output;
	if (job.layout.texture_dimension <= 0 || job.layout.world_span <= 0 ||
			sources.colormap == nullptr || !sources.colormap->is_valid() ||
			sources.heightfield_normal == nullptr ||
			!sources.heightfield_normal->is_valid()) {
		return output;
	}

	// The flat page zeroes every base/DOT3 UV, so every pixel samples the
	// source texel at UV (0,0); its .til loop is suppressed, scorches remain
	// ordered. [orig: PolyTrn_RenderTile @ 0x60DC08..0x60DC16, .til gate
	// @ 0x60DD9A..0x60DD9E]
	const bool flat_page = job.target.page.page_lod_level == 0;
	const int dimension = job.layout.texture_dimension;
	const double world_per_pixel =
			static_cast<double>(job.layout.world_span) / dimension;
	const int32_t world_origin_x =
			job.target.page.sector_origin_x + job.target.page.page_local_x;
	const int32_t world_origin_z =
			job.target.page.sector_origin_z + job.target.page.page_local_z;
	output.width = static_cast<uint32_t>(dimension);
	output.height = static_cast<uint32_t>(dimension);
	output.pixels.resize(static_cast<size_t>(dimension) * dimension * 4u);
	std::vector<uint8_t> dot3_alpha(
			static_cast<size_t>(dimension) * dimension, 0);

	// Base and DOT3 draw the same page-filling quad: positions (0,0)-(dim,dim),
	// UV (local, local + span) / 512 in the selected 512-unit quadrant texture.
	// Both quadrant textures carry flags 0x100001 (CLAMP, LINEAR, MIPFILTER
	// POINT) and the passes add stage-0 CLAMP (0x1700000), so each pixel
	// bilinearly samples the level nearest its texel footprint.
	// [orig: PolyTrn_RenderTile UV build @ 0x60DB62..0x60DC16 (1/512 =
	// flt_7D83A4), quad @ 0x60DC1A..0x60DC7E drawn @ 0x60DD5A / 0x60E39E;
	// apply_texture_stages flag decode @ 0x68084C..0x680870;
	// CGfxDevice_ApplyRenderStates per-stage filters @ 0x67E463..0x67E4A7;
	// levels GTexture_CreateFromPixelData_0 @ 0x6877BA..0x6877D8, box filter
	// @ 0x6878B7..0x6878BE]
	const int quadrant = (job.source_origin_x >= 512 ? 2 : 0) +
			(job.source_origin_z >= 512 ? 1 : 0);
	const std::vector<Rgba8Image> &color_levels =
			sources.colormap->quadrants[static_cast<size_t>(quadrant)];
	const std::vector<Rgba8Image> &normal_levels =
			sources.heightfield_normal->quadrants[static_cast<size_t>(quadrant)];
	const double local_x = flat_page ? 0.0 : job.source_origin_x & 511;
	const double local_z = flat_page ? 0.0 : job.source_origin_z & 511;
	const double uv_per_pixel = flat_page ? 0.0 : world_per_pixel / 512.0;
	const Rgba8Image &color_level = color_levels[nearest_mip_level(
			static_cast<float>(uv_per_pixel * color_levels.front().width),
			color_levels.size())];
	const Rgba8Image &normal_level = normal_levels[nearest_mip_level(
			static_cast<float>(uv_per_pixel * normal_levels.front().width),
			normal_levels.size())];

	const auto base_rows = [&](int row_begin, int row_end) {
	for (int y = row_begin; y < row_end; ++y) {
		const float v = static_cast<float>(local_z / 512.0 + y * uv_per_pixel);
		for (int x = 0; x < dimension; ++x) {
			const float u = static_cast<float>(local_x / 512.0 + x * uv_per_pixel);
			const RgbaF colormap = sample_bilinear_clamp(color_level, u, v);
			const RgbaF normal = sample_bilinear_clamp(normal_level, u, v);

			const float dot =
					(normal.r - 0.5f) * (sources.light_bytes[0] / 255.0f - 0.5f) +
					(normal.g - 0.5f) * (sources.light_bytes[1] / 255.0f - 0.5f) +
					(normal.b - 0.5f) * (sources.light_bytes[2] / 255.0f - 0.5f);
			const size_t offset = 4u * (static_cast<size_t>(y) * dimension + x);
			// The base pass draws MODULATE2X(TEXTURE=colormap, DIFFUSE=
			// 0x808080): c * (128/255) * 2 = c * 256/255; DIFFUSE alpha 0
			// zeroes the RT alpha. [orig: PolyTrn_RenderTile base quad @
			// 0x60DCE5..0x60DCF9, pass PolyTrn_TileBakeBasePass]
			output.pixels[offset] = byte(colormap.r * (256.0f / 255.0f));
			output.pixels[offset + 1] = byte(colormap.g * (256.0f / 255.0f));
			output.pixels[offset + 2] = byte(colormap.b * (256.0f / 255.0f));
			output.pixels[offset + 3] = 0;
			// D3D DOTPRODUCT3 semantics: 4 * sum((a-0.5)(b-0.5)), saturated,
			// blended ONE/ONE into A. [orig: PolyTrn_TileBakeDot3LightPass @
			// 0x60E38A; light dir packed (d+1)*127.5 @ 0x60E231..0x60E331]
			dot3_alpha[static_cast<size_t>(y) * dimension + x] =
					byte(4.0f * dot);
		}
	}
	};

	// Each .til entry is one 16-unit quad placed at (entry - page origin) *
	// dim / span in the page, its atlas corners flipped/rotated and shifted
	// by a half texel. The tile-set atlas carries flags 0x100203 (CLAMP,
	// POINT min/mag, MIPFILTER POINT), so a covered pixel takes the single
	// texel under its interpolated UV on the level nearest its footprint.
	// [orig: PolyTrn_RenderTile ordered entry loop @ 0x60DDD4..0x60DF1B
	// (tile-set bind, then one PolyTrn_DrawTileOverlayQuad per entry), entry AABB test
	// @ 0x60DE20..0x60DE60, quad positions @ 0x60DE66..0x60DEBB, atlas cell
	// UV @ 0x60DEBF..0x60DF05;
	// PolyTrn_DrawTileOverlayQuad flips/rotate @ 0x604772..0x604806, half-texel
	// @ 0x604808..0x6048FD; atlas flags @ 0x604B24]
	const int64_t page_span_q16 = static_cast<int64_t>(job.layout.world_span) << 16;
	const int64_t origin_x_q16 = static_cast<int64_t>(world_origin_x) << 16;
	const int64_t origin_z_q16 = static_cast<int64_t>(world_origin_z) << 16;
	const double pixels_per_q16 =
			static_cast<double>(dimension) / static_cast<double>(page_span_q16);
	const bool draw_tiles = !flat_page && sources.tile_info != nullptr &&
			sources.tilestrip != nullptr && valid_chain(*sources.tilestrip);
	const auto tile_rows = [&](std::size_t lane, std::size_t lanes) {
		const std::vector<Rgba8Image> &atlas_levels = *sources.tilestrip;
		const Rgba8Image &atlas = atlas_levels.front();
		for (const TilOverlayEntry &entry : sources.tile_info->entries) {
			// The entry's page-space rectangle: x raw, z negated on read; the
			// inclusive overlap test admits an entry touching the page edge.
			const int64_t entry_x_q16 = entry.x_fixed;
			const int64_t entry_z_q16 = -static_cast<int64_t>(entry.z_fixed);
			if (entry_x_q16 > origin_x_q16 + page_span_q16 ||
					entry_z_q16 > origin_z_q16 + page_span_q16 ||
					entry_x_q16 + TIL_FIXED_CELL_UNITS < origin_x_q16 ||
					entry_z_q16 + TIL_FIXED_CELL_UNITS < origin_z_q16) {
				continue;
			}
			const TilUvQuad uv_quad = til_build_entry_render_uv_quad(
					entry.tile_index, entry.flags,
					static_cast<int>(atlas.width), static_cast<int>(atlas.height));
			if (!uv_quad.valid) continue;

			const double x0 = static_cast<double>(entry_x_q16 - origin_x_q16) *
					pixels_per_q16;
			const double z0 = static_cast<double>(entry_z_q16 - origin_z_q16) *
					pixels_per_q16;
			const double extent = TIL_FIXED_CELL_UNITS * pixels_per_q16;
			// The footprint of one page pixel in atlas texels, across and
			// down the quad (rotation moves U onto the page's Z axis).
			const TilUv &tl = uv_quad.corners[0];
			const TilUv &tr = uv_quad.corners[1];
			const TilUv &bl = uv_quad.corners[2];
			const float across = std::hypot((tr.u - tl.u) * atlas.width,
					(tr.v - tl.v) * atlas.height) / static_cast<float>(extent);
			const float down = std::hypot((bl.u - tl.u) * atlas.width,
					(bl.v - tl.v) * atlas.height) / static_cast<float>(extent);
			const Rgba8Image &level = atlas_levels[nearest_mip_level(
					std::max(across, down), atlas_levels.size())];

			const int x_begin = first_covered(x0, dimension);
			const int x_end = first_covered(x0 + extent, dimension);
			const int z_begin = first_covered(z0, dimension);
			const int z_end = first_covered(z0 + extent, dimension);
			for_lane_stripes(lane, lanes, z_begin, z_end, [&](int row_begin, int row_end) {
			for (int y = row_begin; y < row_end; ++y) {
				const float local_z = static_cast<float>((y - z0) / extent);
				for (int x = x_begin; x < x_end; ++x) {
					const float local_x = static_cast<float>((x - x0) / extent);
					const TilUv uv = interpolate_uv(uv_quad, local_x, local_z);
					const size_t offset = 4u * (static_cast<size_t>(y) * dimension + x);
					compose_overlay_rgba(output.pixels.data() + offset,
							sample_point_clamp(level, uv.u, uv.v),
							sources.tile_overlay_tint);
				}
			}
			});
		}
	};
	// Each lane owns whole rows, so its base pass lands before its .til
	// writes exactly as the serial order has it.
	const std::size_t lanes = std::max<std::size_t>(threads, 1);
	run_row_stripe_lanes(lanes, [&](std::size_t lane) noexcept {
		for_lane_stripes(lane, lanes, 0, dimension, base_rows);
		if (draw_tiles) tile_rows(lane, lanes);
	});
	// Permanent terrain scorch quads are the second ordered overlay loop:
	// after every mission .til entry and before the DOT3/static-model alpha
	// contribution. Fail closed if a declared plan cannot be composed.
	// [orig: PolyTrn_RenderTile @0x60DF39..0x60E0AF]
	if (sources.scorch_plan != nullptr) {
		if (sources.scorch_textures == nullptr ||
				!compose_terrain_scorches(job, *sources.scorch_plan,
						*sources.scorch_textures, output)) {
			return {};
		}
	}
	add_dot3_alpha(output, dot3_alpha);
	return output;
}

} // namespace opennova::terrain
