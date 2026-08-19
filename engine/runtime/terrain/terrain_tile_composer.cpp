#include <terrain/terrain_tile_composer.h>

// [orig: PolyTrn_RenderTile @ 0x60DA70; tile overlay submission
// Terrain_DrawTileOverlays2D @ 0x5C79C0; docs/tiles/til-re.md]

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

RgbaF sample_bilinear_texel(
		const Rgba8Image &image, float texel_x, float texel_y,
		int minimum_x, int minimum_y, int maximum_x, int maximum_y) {
	texel_x = std::clamp(texel_x, static_cast<float>(minimum_x),
			static_cast<float>(maximum_x));
	texel_y = std::clamp(texel_y, static_cast<float>(minimum_y),
			static_cast<float>(maximum_y));
	const int x0 = static_cast<int>(std::floor(texel_x));
	const int y0 = static_cast<int>(std::floor(texel_y));
	const int x1 = std::min(x0 + 1, maximum_x);
	const int y1 = std::min(y0 + 1, maximum_y);
	const float tx = texel_x - x0;
	const float ty = texel_y - y0;
	return lerp(lerp(pixel(image, x0, y0), pixel(image, x1, y0), tx),
			lerp(pixel(image, x0, y1), pixel(image, x1, y1), tx), ty);
}

RgbaF sample_source_quadrant(
		const Rgba8Image &image, float source_x, float source_z,
		int source_origin_x, int source_origin_z) {
	const int quadrant_x = source_origin_x >= 512 ? 1 : 0;
	const int quadrant_z = source_origin_z >= 512 ? 1 : 0;
	const int half_width = static_cast<int>(image.width) / 2;
	const int half_height = static_cast<int>(image.height) / 2;
	const int minimum_x = quadrant_x * half_width;
	const int minimum_y = quadrant_z * half_height;
	const int maximum_x = minimum_x + half_width - 1;
	const int maximum_y = minimum_y + half_height - 1;
	const float texel_x = source_x * static_cast<float>(image.width) / 1024.0f - 0.5f;
	const float texel_y = source_z * static_cast<float>(image.height) / 1024.0f - 0.5f;
	return sample_bilinear_texel(
			image, texel_x, texel_y, minimum_x, minimum_y, maximum_x, maximum_y);
}

RgbaF sample_tilestrip(const Rgba8Image &image, TilUv uv) {
	const float texel_x = uv.u * static_cast<float>(image.width) - 0.5f;
	const float texel_y = uv.v * static_cast<float>(image.height) - 0.5f;
	return sample_bilinear_texel(image, texel_x, texel_y, 0, 0,
			static_cast<int>(image.width) - 1,
			static_cast<int>(image.height) - 1);
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
		destination[channel] = byte(
				destination_rgb[channel] * (1.0f - alpha) +
				source_rgb[channel] * tint[channel] * alpha);
	}
	// The fixed-function SRCALPHA/INVSRCALPHA state applies to every render-
	// target channel. Stage alpha selects the texture, so an overlay drawn into
	// the base pass's zero alpha writes src.a * src.a (and attenuates any prior
	// overlay alpha). The later DOT3 pass adds terrain light into A.
	// [orig: overlay view mode 0x631 -> SRCALPHA/INVSRCALPHA @
	// 0x680F2C..0x680F3A; no separate-alpha override, GfxBlend_ApplyToDevice
	// @ 0x6817D0]
	destination[3] = byte(
			alpha * alpha +
			(destination[3] * inverse_byte) * (1.0f - alpha));
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

} // namespace

Rgba8Image compose_terrain_tile_page(
		const TerrainTileCompositionJob &job,
		const TerrainTilePageSourceView &sources) {
	Rgba8Image output;
	if (job.layout.texture_dimension <= 0 || job.layout.world_span <= 0 ||
			sources.colormap == nullptr || !sources.colormap->is_valid() ||
			sources.colormap->width < 2 || sources.colormap->height < 2 ||
			sources.heightfield_normal == nullptr ||
			!sources.heightfield_normal->is_valid() ||
			sources.heightfield_normal->width < 2 ||
			sources.heightfield_normal->height < 2) {
		return output;
	}

	const int dimension = job.layout.texture_dimension;
	const float world_per_texel =
			static_cast<float>(job.layout.world_span) / dimension;
	const float world_origin_x = static_cast<float>(
			job.target.page.sector_origin_x + job.target.page.page_local_x);
	const float world_origin_z = static_cast<float>(
			job.target.page.sector_origin_z + job.target.page.page_local_z);
	output.width = static_cast<uint32_t>(dimension);
	output.height = static_cast<uint32_t>(dimension);
	output.pixels.resize(static_cast<size_t>(dimension) * dimension * 4u);
	std::vector<uint8_t> dot3_alpha(
			static_cast<size_t>(dimension) * dimension, 0);

	for (int y = 0; y < dimension; ++y) {
		for (int x = 0; x < dimension; ++x) {
			const float page_x = (static_cast<float>(x) + 0.5f) * world_per_texel;
			const float page_z = (static_cast<float>(y) + 0.5f) * world_per_texel;
			const RgbaF colormap = sample_source_quadrant(*sources.colormap,
					static_cast<float>(job.source_origin_x) + page_x,
					static_cast<float>(job.source_origin_z) + page_z,
					job.source_origin_x, job.source_origin_z);
			const RgbaF normal = sample_source_quadrant(*sources.heightfield_normal,
					static_cast<float>(job.source_origin_x) + page_x,
					static_cast<float>(job.source_origin_z) + page_z,
					job.source_origin_x, job.source_origin_z);

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

	if (sources.tile_info == nullptr || sources.tilestrip == nullptr ||
			!sources.tilestrip->is_valid()) {
		add_dot3_alpha(output, dot3_alpha);
		return output;
	}

	const float page_max_x = world_origin_x + job.layout.world_span;
	const float page_max_z = world_origin_z + job.layout.world_span;
	for (const TilOverlayEntry &entry : sources.tile_info->entries) {
		const float entry_x = til_world_x_from_fixed(entry.x_fixed);
		const float entry_z = til_world_z_from_fixed(entry.z_fixed);
		const float entry_max_x = entry_x + TIL_CELL_WORLD_UNITS;
		const float entry_max_z = entry_z + TIL_CELL_WORLD_UNITS;
		if (entry_max_x <= world_origin_x || entry_x >= page_max_x ||
				entry_max_z <= world_origin_z || entry_z >= page_max_z) {
			continue;
		}
		const TilUvQuad uv_quad = til_build_entry_render_uv_quad(
				entry.tile_index, entry.flags,
				static_cast<int>(sources.tilestrip->width),
				static_cast<int>(sources.tilestrip->height));
		if (!uv_quad.valid) continue;

		const int x0 = std::clamp(static_cast<int>(std::floor(
				(entry_x - world_origin_x) / world_per_texel)), 0, dimension);
		const int z0 = std::clamp(static_cast<int>(std::floor(
				(entry_z - world_origin_z) / world_per_texel)), 0, dimension);
		const int x1 = std::clamp(static_cast<int>(std::ceil(
				(entry_max_x - world_origin_x) / world_per_texel)), 0, dimension);
		const int z1 = std::clamp(static_cast<int>(std::ceil(
				(entry_max_z - world_origin_z) / world_per_texel)), 0, dimension);

		for (int y = z0; y < z1; ++y) {
			const float world_z = world_origin_z +
					(static_cast<float>(y) + 0.5f) * world_per_texel;
			const float local_z = (world_z - entry_z) / TIL_CELL_WORLD_UNITS;
			if (local_z < 0.0f || local_z >= 1.0f) continue;
			for (int x = x0; x < x1; ++x) {
				const float world_x = world_origin_x +
						(static_cast<float>(x) + 0.5f) * world_per_texel;
				const float local_x = (world_x - entry_x) / TIL_CELL_WORLD_UNITS;
				if (local_x < 0.0f || local_x >= 1.0f) continue;
				const RgbaF tile = sample_tilestrip(*sources.tilestrip,
						interpolate_uv(uv_quad, local_x, local_z));
				const size_t offset = 4u * (static_cast<size_t>(y) * dimension + x);
				compose_overlay_rgba(output.pixels.data() + offset, tile,
						sources.tile_overlay_tint);
			}
		}
	}
	add_dot3_alpha(output, dot3_alpha);
	return output;
}

} // namespace opennova::terrain
