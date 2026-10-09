// Terrain tile-page composer: portable pixel-oracle coverage for the 128-page
// device cache. Synthetic cases pin page density, quadrant-locked source
// sampling, DOT3 byte order, .til atlas orientation, tint/alpha, clipping, and
// painter order. The optional CP12 leg follows docs/asset-gated-tests.md.
#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/renderer/texture_dxt.h>

#include <formats/til/til.h>
#include <formats/til/til_io.h>
#include <base/vfs/vfs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "common/retail_paths.h"

namespace {

using opennova::terrain::Rgba8Image;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

Rgba8Image solid_image(
		uint32_t width, uint32_t height, std::array<uint8_t, 4> rgba) {
	Rgba8Image image;
	image.width = width;
	image.height = height;
	image.pixels.resize(static_cast<size_t>(width) * height * 4u);
	for (size_t i = 0; i < image.pixels.size(); i += 4) {
		std::copy(rgba.begin(), rgba.end(), image.pixels.begin() + i);
	}
	return image;
}

void set_pixel(Rgba8Image &image, int x, int y,
		std::array<uint8_t, 4> rgba) {
	const size_t offset = 4u *
			(static_cast<size_t>(y) * image.width + static_cast<size_t>(x));
	std::copy(rgba.begin(), rgba.end(), image.pixels.begin() + offset);
}

void fill_rect(Rgba8Image &image, int x0, int y0, int x1, int y1,
		std::array<uint8_t, 4> rgba) {
	for (int y = y0; y < y1; ++y) {
		for (int x = x0; x < x1; ++x) {
			set_pixel(image, x, y, rgba);
		}
	}
}

std::array<uint8_t, 4> pixel(const Rgba8Image &image, int x, int y) {
	const size_t offset = 4u *
			(static_cast<size_t>(y) * image.width + static_cast<size_t>(x));
	return {image.pixels[offset], image.pixels[offset + 1],
			image.pixels[offset + 2], image.pixels[offset + 3]};
}

bool expect_pixel(const Rgba8Image &image, int x, int y,
		std::array<uint8_t, 4> expected, const char *message) {
	const std::array<uint8_t, 4> actual = pixel(image, x, y);
	if (actual == expected) return true;
	std::fprintf(stderr,
			"FAIL: %s at (%d,%d): actual=(%u,%u,%u,%u) "
			"expected=(%u,%u,%u,%u)\n",
			message, x, y,
			static_cast<unsigned>(actual[0]),
			static_cast<unsigned>(actual[1]),
			static_cast<unsigned>(actual[2]),
			static_cast<unsigned>(actual[3]),
			static_cast<unsigned>(expected[0]),
			static_cast<unsigned>(expected[1]),
			static_cast<unsigned>(expected[2]),
			static_cast<unsigned>(expected[3]));
	return false;
}

uint8_t quantized_byte(float value) {
	return static_cast<uint8_t>(std::clamp(
			static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) *
					255.0f)), 0, 255));
}

uint8_t retail_colormap_byte(uint8_t source) {
	return quantized_byte(
			(source / 255.0f) * (256.0f / 255.0f));
}

uint8_t retail_dot3_alpha(std::array<uint8_t, 3> normal,
		std::array<uint8_t, 3> light) {
	float dot = 0.0f;
	for (int channel = 0; channel < 3; ++channel) {
		dot += (normal[channel] / 255.0f - 0.5f) *
				(light[channel] / 255.0f - 0.5f);
	}
	return quantized_byte(4.0f * dot);
}

std::array<uint8_t, 4> base_pixel(std::array<uint8_t, 3> color,
		std::array<uint8_t, 3> normal,
		std::array<uint8_t, 3> light) {
	return {retail_colormap_byte(color[0]),
			retail_colormap_byte(color[1]),
			retail_colormap_byte(color[2]),
			retail_dot3_alpha(normal, light)};
}

// A solid atlas cell as the DXT5 tile-set texture reads it back.
std::array<uint8_t, 4> dxt5_texel(std::array<uint8_t, 4> color) {
	std::vector<uint8_t> rgba;
	for (int texel = 0; texel < 16; ++texel) rgba.insert(rgba.end(), color.begin(), color.end());
	const std::vector<opennova::renderer::DxtSurface> levels =
			opennova::renderer::build_dxt_texture_levels(rgba.data(), 4, 4,
					opennova::renderer::TextureDxtFormat::Dxt5, 1);
	const std::vector<uint8_t> bytes = opennova::renderer::encode_rgba8(
			opennova::renderer::decode_dxt_surface(levels.front()));
	return {bytes[0], bytes[1], bytes[2], bytes[3]};
}

std::array<uint8_t, 4> overlay_render_target_pixel(
		std::array<uint8_t, 4> destination,
		std::array<uint8_t, 4> source,
		std::array<float, 3> tint) {
	const float alpha = source[3] / 255.0f;
	for (int channel = 0; channel < 3; ++channel) {
		destination[channel] = quantized_byte(
				(destination[channel] / 255.0f) * (1.0f - alpha) +
				(source[channel] / 255.0f) * tint[channel] * alpha);
	}
	// Alpha stays untouched: the overlay loops run with
	// COLORWRITEENABLE = 7, so the SRCALPHA blend lands on RGB only.
	// [orig: PolyTrn_RenderTile SetRenderState(0xA8, 7) @ 0x60DD6B..0x60DD73]
	return destination;
}

std::array<uint8_t, 4> add_dot3_alpha(
		std::array<uint8_t, 4> destination, uint8_t dot3_alpha) {
	destination[3] = static_cast<uint8_t>(std::min(
			255, static_cast<int>(destination[3]) + dot3_alpha));
	return destination;
}

std::array<uint8_t, 4> overlay_pixel(
		std::array<uint8_t, 4> base,
		std::array<uint8_t, 4> source,
		std::array<float, 3> tint) {
	std::array<uint8_t, 4> target = base;
	target[3] = 0;
	return add_dot3_alpha(
			overlay_render_target_pixel(target, source, tint), base[3]);
}

opennova::TerrainTileCompositionJob cold_job(uint8_t page_lod,
		opennova::TerrainTilePageKey page = {},
		int source_origin_x = 0, int source_origin_z = 0) {
	page.page_lod_level = page_lod;
	opennova::TerrainTileCompositionJob job;
	job.target.page = page;
	job.source_origin_x = source_origin_x;
	job.source_origin_z = source_origin_z;
	const int span = opennova::TerrainTileCompositionCache::page_world_span(page_lod);
	job.layout.texture_dimension = opennova::TerrainTileCompositionCache::kDimension;
	job.layout.world_span = span;
	job.layout.texels_per_world_unit =
			static_cast<float>(job.layout.texture_dimension) / static_cast<float>(span);
	return job;
}

opennova::terrain::TerrainTileQuadrantSource quadrants(const Rgba8Image &atlas) {
	return opennova::terrain::build_terrain_tile_quadrant_source(atlas);
}

std::vector<Rgba8Image> tile_levels(const Rgba8Image &atlas) {
	return opennova::terrain::build_terrain_tile_set_mips(atlas);
}

int bright_pixels(const Rgba8Image &image) {
	int count = 0;
	for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
		if (image.pixels[i] > 240 && image.pixels[i + 1] > 240 &&
				image.pixels[i + 2] > 240) {
			++count;
		}
	}
	return count;
}

bool test_level_density() {
	const auto colormap = quadrants(solid_image(2, 2, {0, 0, 0, 255}));
	const auto normal = quadrants(solid_image(2, 2, {128, 128, 255, 128}));
	const auto tilestrip = tile_levels(solid_image(64, 64, {255, 255, 255, 255}));
	opennova::TilFile tiles;
	tiles.entries.push_back(opennova::make_til_overlay_entry(0, 0, 0, 0));

	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.tile_info = &tiles;
	sources.tilestrip = &tilestrip;
	sources.light_bytes = {128, 128, 255};

	const Rgba8Image coarse = opennova::terrain::compose_terrain_tile_page(
			cold_job(1), sources);
	if (!expect(coarse.width == 256 && coarse.height == 256 && coarse.is_valid(),
			"coarse page is one complete 256x256 cache layer")) return false;
	if (!expect(bright_pixels(coarse) == 8 * 8,
			"a 16-unit tile occupies 8x8 pixels on the 512-unit page")) return false;

	const Rgba8Image fine = opennova::terrain::compose_terrain_tile_page(
			cold_job(4), sources);
	if (!expect(fine.width == 256 && fine.height == 256 && fine.is_valid(),
			"fine page is one complete 256x256 cache layer")) return false;
	return expect(bright_pixels(fine) == 64 * 64,
			"a 16-unit tile occupies 64x64 pixels on the 64-unit page");
}

bool test_flat_page_source_and_overlay_gate() {
	Rgba8Image colormap = solid_image(8, 8, {220, 180, 120, 255});
	Rgba8Image normal = solid_image(8, 8, {255, 128, 128, 128});
	set_pixel(colormap, 0, 0, {31, 63, 95, 255});
	set_pixel(normal, 0, 0, {128, 128, 255, 128});
	const auto colormap_quadrants = quadrants(colormap);
	const auto normal_quadrants = quadrants(normal);
	const auto tilestrip = tile_levels(solid_image(64, 64, {255, 0, 255, 255}));
	opennova::TilFile tiles;
	tiles.entries.push_back(opennova::make_til_overlay_entry(0, 0, 0, 0));
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap_quadrants;
	sources.heightfield_normal = &normal_quadrants;
	sources.tile_info = &tiles;
	sources.tilestrip = &tilestrip;
	sources.light_bytes = {128, 128, 255};
	const Rgba8Image flat = opennova::terrain::compose_terrain_tile_page(cold_job(0), sources);
	if (!expect(flat.is_valid(), "flat LOD-0 cache page composes")) return false;
	const auto expected = base_pixel({31, 63, 95}, {128, 128, 255}, sources.light_bytes);
	for (int y = 0; y < 256; ++y) {
		for (int x = 0; x < 256; ++x) {
			if (!expect_pixel(flat, x, y, expected,
					"flat base and DOT3 use source UV zero and reject .til overlays")) return false;
		}
	}
	return true;
}

bool test_rejects_source_atlases_without_a_complete_quadrant() {
	const Rgba8Image valid = solid_image(2, 2, {128, 128, 255, 255});
	const std::array<Rgba8Image, 3> tiny = {
			solid_image(1, 2, {128, 128, 255, 255}),
			solid_image(2, 1, {128, 128, 255, 255}),
			solid_image(1, 1, {128, 128, 255, 255}),
	};
	const auto valid_quadrants = quadrants(valid);
	for (const Rgba8Image &source : tiny) {
		const auto source_quadrants = quadrants(source);
		opennova::terrain::TerrainTilePageSourceView sources;
		sources.colormap = &source_quadrants;
		sources.heightfield_normal = &valid_quadrants;
		if (!expect(!opennova::terrain::compose_terrain_tile_page(
					cold_job(4), sources).is_valid(),
				"a 1xN, Nx1, or 1x1 colormap cannot provide four quadrants")) {
			return false;
		}
	}
	const auto tiny_quadrants = quadrants(tiny[0]);
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &valid_quadrants;
	sources.heightfield_normal = &tiny_quadrants;
	return expect(!opennova::terrain::compose_terrain_tile_page(
				cold_job(4), sources).is_valid(),
			"the mandatory height-normal atlas needs complete quadrants too");
}

bool test_source_orientation_quadrant_clamp_and_dot3() {
	// The requested page begins on both 512-source boundaries. Values outside
	// the bottom-right source quadrant are deliberately hostile: the first
	// bilinear footprint must clamp to (512,512), never bleed from row/column
	// 511. Four asymmetric blocks inside the quadrant pin +X/+Z row direction.
	Rgba8Image colormap = solid_image(1024, 1024, {251, 3, 197, 255});
	Rgba8Image normal = solid_image(1024, 1024, {255, 0, 0, 255});
	const std::array<std::array<uint8_t, 3>, 4> colors = {{
			{40, 80, 120}, {18, 140, 73},
			{200, 35, 90}, {15, 220, 160}}};
	const std::array<std::array<uint8_t, 3>, 4> normals = {{
			{220, 110, 180}, {180, 220, 100},
			{90, 200, 240}, {140, 70, 250}}};
	for (int block_z = 0; block_z < 2; ++block_z) {
		for (int block_x = 0; block_x < 2; ++block_x) {
			const int index = block_z * 2 + block_x;
			fill_rect(colormap, 512 + block_x * 32, 512 + block_z * 32,
					544 + block_x * 32, 544 + block_z * 32,
					{colors[index][0], colors[index][1], colors[index][2], 255});
			fill_rect(normal, 512 + block_x * 32, 512 + block_z * 32,
					544 + block_x * 32, 544 + block_z * 32,
					{normals[index][0], normals[index][1], normals[index][2], 255});
		}
	}

	const std::array<uint8_t, 3> light{231, 83, 187};
	const auto colormap_quadrants = quadrants(colormap);
	const auto normal_quadrants = quadrants(normal);
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap_quadrants;
	sources.heightfield_normal = &normal_quadrants;
	sources.light_bytes = light;
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(4, {}, 512, 512), sources);
	if (!expect(page.is_valid(),
			"quadrant-clamped source page composes")) return false;
	if (!expect_pixel(page, 0, 0, base_pixel(colors[0], normals[0], light),
			"source-boundary sample clamps within the selected quadrant")) return false;
	if (!expect_pixel(page, 64, 64, base_pixel(colors[0], normals[0], light),
			"top-left source block keeps X/Z orientation")) return false;
	if (!expect_pixel(page, 192, 64, base_pixel(colors[1], normals[1], light),
			"increasing output X samples the source-right block")) return false;
	if (!expect_pixel(page, 64, 192, base_pixel(colors[2], normals[2], light),
			"increasing output row samples increasing source Z")) return false;
	return expect_pixel(page, 192, 192,
			base_pixel(colors[3], normals[3], light),
			"asymmetric light bytes preserve the documented DOT3 basis");
}

bool test_overlay_atlas_flags_tint_clipping_and_order() {
	const std::array<uint8_t, 3> base_color{40, 80, 120};
	const std::array<uint8_t, 3> base_normal{220, 110, 180};
	const std::array<uint8_t, 3> light{231, 83, 187};
	const std::array<float, 3> tint{0.5f, 0.25f, 0.75f};
	const std::array<uint8_t, 4> base =
			base_pixel(base_color, base_normal, light);
	const Rgba8Image colormap = solid_image(
			2, 2, {base_color[0], base_color[1], base_color[2], 255});
	const Rgba8Image normal = solid_image(
			2, 2, {base_normal[0], base_normal[1], base_normal[2], 255});

	// Four 64px cells in one row. Cell 1 has asymmetric color/alpha
	// quadrants; cells 2 then 3 overlap to pin painter order.
	const std::array<uint8_t, 4> edge{250, 90, 10, 200};
	const std::array<uint8_t, 4> tile_tl{200, 10, 20, 64};
	const std::array<uint8_t, 4> tile_tr{30, 210, 40, 128};
	const std::array<uint8_t, 4> tile_bl{50, 60, 220, 192};
	const std::array<uint8_t, 4> tile_br{230, 180, 70, 255};
	const std::array<uint8_t, 4> overlap_first{20, 200, 80, 255};
	const std::array<uint8_t, 4> overlap_last{220, 40, 180, 128};
	Rgba8Image tilestrip = solid_image(256, 64, edge);
	// The atlas is point-sampled on a 1:1 LOD-4 page, so every cell's texels
	// stay inside that cell; it is DXT5, so each solid cell reads back as its
	// block's decode. [orig: atlas flags 0x100203 @ 0x604B24]
	fill_rect(tilestrip, 64, 0, 96, 32, tile_tl);
	fill_rect(tilestrip, 96, 0, 128, 32, tile_tr);
	fill_rect(tilestrip, 64, 32, 96, 64, tile_bl);
	fill_rect(tilestrip, 96, 32, 128, 64, tile_br);
	fill_rect(tilestrip, 128, 0, 192, 64, overlap_first);
	fill_rect(tilestrip, 192, 0, 256, 64, overlap_last);

	opennova::TilFile tiles;
	opennova::TilOverlayEntry clipped;
	clipped.x_fixed = opennova::til_x_fixed_from_world(-8.0);
	clipped.z_fixed = opennova::til_z_fixed_from_world(48.0);
	clipped.tile_index = 0;
	tiles.entries.push_back(clipped);
	tiles.entries.push_back(opennova::make_til_overlay_entry(
			1, 1, 1, static_cast<uint8_t>(
				opennova::TIL_FLAG_FLIP_X | opennova::TIL_FLAG_ROTATE_90)));
	tiles.entries.push_back(opennova::make_til_overlay_entry(2, 0, 2, 0));
	tiles.entries.push_back(opennova::make_til_overlay_entry(2, 0, 3, 0));

	const auto colormap_quadrants = quadrants(colormap);
	const auto normal_quadrants = quadrants(normal);
	const auto tilestrip_levels = tile_levels(tilestrip);
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap_quadrants;
	sources.heightfield_normal = &normal_quadrants;
	sources.tile_info = &tiles;
	sources.tilestrip = &tilestrip_levels;
	sources.tile_overlay_tint = tint;
	sources.light_bytes = light;
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(4), sources);
	if (!expect(page.is_valid(), "asymmetric multi-tile page composes")) return false;

	const std::array<uint8_t, 4> edge_result =
			overlay_pixel(base, dxt5_texel(edge), tint);
	if (!expect_pixel(page, 0, 220, edge_result,
			"negative-X entry clips onto the page's left edge")) return false;
	if (!expect_pixel(page, 31, 220, edge_result,
			"partial entry fills its last in-page column")) return false;
	if (!expect_pixel(page, 32, 220, base,
			"partial entry does not leak past its clipped 8-unit span")) return false;
	if (!expect_pixel(page, 16, 191, base,
			"entry begins on the exact page-row boundary")) return false;
	if (!expect_pixel(page, 16, 192, edge_result,
			"page-edge clipping keeps the first covered row")) return false;

	// FLIP_X|ROTATE_90 (0x05) under retail's mirror-then-corner-cycle order is
	// T(x,z) = (z, x) — a transpose: destination TL/TR/BL/BR sample source
	// TL/BL/TR/BR (D-TIL-4). Interior probes avoid interpolation across
	// quadrant seams. [orig: PolyTrn_DrawTileOverlayQuad @ 0x604700 — mirrors
	// @ 0x604782/0x6047a9, rotate cycle @ 0x6047d4..0x604806]
	if (!expect_pixel(page, 80, 80, overlay_pixel(base, dxt5_texel(tile_tl), tint),
			"combined flags map destination TL to source TL")) return false;
	if (!expect_pixel(page, 112, 80, overlay_pixel(base, dxt5_texel(tile_bl), tint),
			"combined flags map destination TR to source BL")) return false;
	if (!expect_pixel(page, 80, 112, overlay_pixel(base, dxt5_texel(tile_tr), tint),
			"output rows follow world +Z through the transposed atlas")) return false;
	if (!expect_pixel(page, 112, 112, overlay_pixel(base, dxt5_texel(tile_br), tint),
			"combined flags map destination BR to source BR")) return false;
	if (!expect(pixel(page, 80, 80)[3] ==
			overlay_pixel(base, dxt5_texel(tile_tl), tint)[3],
			"overlay draws leave the page alpha to the DOT3 pass")) return false;

	std::array<uint8_t, 4> overlay_target = base;
	overlay_target[3] = 0;
	const std::array<uint8_t, 4> after_first =
			overlay_render_target_pixel(overlay_target, dxt5_texel(overlap_first), tint);
	const std::array<uint8_t, 4> after_last = add_dot3_alpha(
			overlay_render_target_pixel(after_first, dxt5_texel(overlap_last), tint),
			base[3]);
	if (!expect_pixel(page, 160, 32, after_last,
			"later .til entries source-over earlier entries in file order")) return false;
	// Row stripes (runtime/terrain/row_stripes.h) keep every pixel's base
	// then .til writes in file order, so any thread count reproduces the
	// serial page byte for byte.
	for (const std::size_t threads : {std::size_t{3}, std::size_t{8}}) {
		const Rgba8Image striped = opennova::terrain::compose_terrain_tile_page(
				cold_job(4), sources, threads);
		if (!expect(striped.pixels == page.pixels,
				"row-striped composition reproduces the serial page bytes")) return false;
	}
	return true;
}

// The page passes are XYZRHW quads whose positions are copied unbiased, so
// page pixel x samples at position x: a 1:1 LOD-2 page takes base texel
// coordinate x - 0.5 in its quadrant texture, blending texels x-1 and x.
// [orig: GDynamicVB_FillFullscreenQuadVertices @ 0x678DFE..0x678E4B (positions
// unbiased); PolyTrn_RenderTile quad (0,0)-(dim,dim) @ 0x60DC1A..0x60DC7E,
// UV local/512 @ 0x60DB86..0x60DC02]
bool test_base_pass_samples_integer_pixel_positions() {
	Rgba8Image colormap = solid_image(1024, 1024, {0, 0, 0, 255});
	fill_rect(colormap, 5, 0, 6, 512, {255, 255, 255, 255});
	const auto colormap_quadrants = quadrants(colormap);
	const auto normal = quadrants(solid_image(2, 2, {128, 128, 255, 128}));
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap_quadrants;
	sources.heightfield_normal = &normal;
	sources.light_bytes = {128, 128, 255};
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(2), sources);
	if (!expect(page.is_valid(), "1:1 base page composes")) return false;
	const uint8_t half = quantized_byte(0.5f * (256.0f / 255.0f));
	const std::array<uint8_t, 4> blended{half, half, half, 255};
	const std::array<uint8_t, 4> black{0, 0, 0, 255};
	return expect_pixel(page, 4, 40, black,
			"pixel 4 blends texels 3 and 4 (both dark)") &&
			expect_pixel(page, 5, 40, blended,
					"pixel 5 blends texels 4 and 5 at one half each") &&
			expect_pixel(page, 6, 40, blended,
					"pixel 6 blends texels 5 and 6 at one half each") &&
			expect_pixel(page, 7, 40, black,
					"pixel 7 no longer reaches the stripe");
}

// A LOD-1 page draws 512 units over 256 pixels: two colormap texels per
// pixel, so the quadrant texture's MIPFILTER POINT selects box level 1.
// [orig: Colormap0..3 flags 0x100001 @ 0x60B51C (bit 3 clear: MIPFILTER
// POINT, bit 1 clear: LINEAR) decoded by CGfxShader_ApplyTextureStages
// @ 0x68084C..0x680870 into CGfxDevice_ApplyRenderStates @ 0x67E463..0x67E4A7;
// box levels GTexture_CreateFromPixelData_0 @ 0x6877BA..0x6878BE]
bool test_base_pass_coarse_page_samples_box_level_one() {
	Rgba8Image colormap = solid_image(1024, 1024, {0, 0, 0, 255});
	for (int x = 0; x < 512; ++x) {
		if (((x / 2) & 1) != 0) fill_rect(colormap, x, 0, x + 1, 512, {255, 255, 255, 255});
	}
	const auto colormap_quadrants = quadrants(colormap);
	const auto normal = quadrants(solid_image(2, 2, {128, 128, 255, 128}));
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap_quadrants;
	sources.heightfield_normal = &normal;
	sources.light_bytes = {128, 128, 255};
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(1), sources);
	if (!expect(page.is_valid(), "coarse base page composes")) return false;
	// Level 1 alternates 0/255 per texel; pixel x samples it at x - 0.5.
	const uint8_t half = quantized_byte(0.5f * (256.0f / 255.0f));
	for (int x = 1; x < 64; ++x) {
		if (!expect_pixel(page, x, 17, {half, half, half, 255},
				"each coarse pixel blends two level-1 texels")) return false;
	}
	return expect_pixel(page, 0, 17, {0, 0, 0, 255},
			"the clamped first pixel takes level-1 texel 0");
}

// A one-texel line on a 1:1 LOD-4 page: the tile-set atlas is POINT-sampled
// and the half-texel bias puts every pixel on a texel centre, so the line
// lands on exactly one page column at full strength.
// [orig: Terrain_LoadTileSetAtlas flags 0x100203 @ 0x604B24 (bit 1: POINT);
// PolyTrn_DrawTileOverlayQuad half-texel @ 0x604808..0x6048FD; quad positions
// PolyTrn_RenderTile @ 0x60DE66..0x60DEBB]
bool test_til_line_is_point_sampled_one_to_one() {
	const std::array<uint8_t, 3> base_color{40, 80, 120};
	const std::array<uint8_t, 3> base_normal{128, 128, 255};
	const std::array<uint8_t, 3> light{128, 128, 255};
	const auto colormap = quadrants(solid_image(
			2, 2, {base_color[0], base_color[1], base_color[2], 255}));
	const auto normal = quadrants(solid_image(
			2, 2, {base_normal[0], base_normal[1], base_normal[2], 255}));
	Rgba8Image atlas = solid_image(64, 64, {0, 0, 0, 0});
	fill_rect(atlas, 10, 0, 11, 64, {255, 255, 255, 255});
	const auto atlas_levels = tile_levels(atlas);
	opennova::TilFile tiles;
	tiles.entries.push_back(opennova::make_til_overlay_entry(0, 0, 0, 0));
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.tile_info = &tiles;
	sources.tilestrip = &atlas_levels;
	sources.light_bytes = light;
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(4), sources);
	if (!expect(page.is_valid(), "1:1 tile page composes")) return false;
	const std::array<uint8_t, 4> base = base_pixel(base_color, base_normal, light);
	const std::array<uint8_t, 4> line = overlay_pixel(
			base, {255, 255, 255, 255}, {1.0f, 1.0f, 1.0f});
	return expect_pixel(page, 10, 20, line,
			"the one-texel line fills exactly its page column") &&
			expect_pixel(page, 9, 20, base, "no half-strength bleed on the left") &&
			expect_pixel(page, 11, 20, base, "no half-strength bleed on the right");
}

// A LOD-3 page draws a 16-unit tile over 32 pixels: two atlas texels per
// pixel, so the point sample takes the tile-set atlas box level 1.
// [orig: atlas levels GTexture_CreateFromPixelData_0 @ 0x6877BA..0x6878BE;
// point min/mag/mip @ 0x604B24 via CGfxShader_ApplyTextureStages @ 0x68084C..0x680870]
bool test_til_coarse_page_takes_box_level() {
	const auto colormap = quadrants(solid_image(2, 2, {0, 0, 0, 255}));
	const auto normal = quadrants(solid_image(2, 2, {128, 128, 255, 128}));
	Rgba8Image atlas = solid_image(64, 64, {0, 0, 200, 255});
	for (int x = 0; x < 64; x += 2) fill_rect(atlas, x, 0, x + 1, 64, {200, 0, 0, 255});
	const auto atlas_levels = tile_levels(atlas);
	opennova::TilFile tiles;
	tiles.entries.push_back(opennova::make_til_overlay_entry(0, 0, 0, 0));
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.tile_info = &tiles;
	sources.tilestrip = &atlas_levels;
	sources.light_bytes = {128, 128, 255};
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(3), sources);
	if (!expect(page.is_valid(), "LOD-3 tile page composes")) return false;
	const std::array<uint8_t, 4> bare =
			base_pixel({0, 0, 0}, {128, 128, 255}, {128, 128, 255});
	// The DXT5 atlas decodes the stripes to their 5:6:5 values (197, 0, 0)
	// and (0, 0, 197); level 1 is D3DXFilterTexture's box average of that
	// decode, re-encoded: (99, 0, 99).
	const std::array<uint8_t, 4> purple = overlay_pixel(
			bare, {99, 0, 99, 255}, {1.0f, 1.0f, 1.0f});
	for (int y = 0; y < 32; ++y) {
		for (int x = 0; x < 32; ++x) {
			if (!expect_pixel(page, x, y, purple,
					"every covered pixel is the level-1 box average")) return false;
		}
	}
	return expect_pixel(page, 32, 0, bare,
			"the 16-unit tile covers exactly 32 LOD-3 pixels");
}

uint16_t little_u16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0]) |
			(static_cast<uint16_t>(p[1]) << 8);
}

bool decode_uncompressed_tga_rgba(
		const std::vector<uint8_t> &bytes, Rgba8Image &output) {
	output = {};
	if (bytes.size() < 18 || bytes[1] != 0 || bytes[2] != 2 ||
			(bytes[16] != 24 && bytes[16] != 32)) {
		return false;
	}
	const int width = little_u16(bytes.data() + 12);
	const int height = little_u16(bytes.data() + 14);
	const size_t stride = bytes[16] / 8u;
	const size_t start = 18u + bytes[0];
	if (width <= 0 || height <= 0 ||
			start + static_cast<size_t>(width) * height * stride > bytes.size()) {
		return false;
	}
	output.width = static_cast<uint32_t>(width);
	output.height = static_cast<uint32_t>(height);
	output.pixels.resize(static_cast<size_t>(width) * height * 4u);
	const bool top_origin = (bytes[17] & 0x20u) != 0;
	const bool right_origin = (bytes[17] & 0x10u) != 0;
	for (int source_y = 0; source_y < height; ++source_y) {
		for (int source_x = 0; source_x < width; ++source_x) {
			const int x = right_origin ? width - 1 - source_x : source_x;
			const int y = top_origin ? source_y : height - 1 - source_y;
			const size_t source = start + stride *
					(static_cast<size_t>(source_y) * width + source_x);
			const size_t target = 4u *
					(static_cast<size_t>(y) * width + x);
			output.pixels[target] = bytes[source + 2];
			output.pixels[target + 1] = bytes[source + 1];
			output.pixels[target + 2] = bytes[source];
			output.pixels[target + 3] = stride == 4 ? bytes[source + 3] : 255;
		}
	}
	return output.is_valid();
}

uint64_t fnv_byte(uint64_t hash, uint8_t value) {
	return (hash ^ value) * UINT64_C(1099511628211);
}

bool til_entry_oracle(const opennova::TilOverlayEntry &entry,
		const Rgba8Image &tilestrip, int entry_index,
		const opennova::TerrainTilePageKey &page_key,
		uint64_t expected_rgb_hash, uint64_t expected_alpha_hash) {
	opennova::TilFile single;
	single.entries.push_back(entry);
	const auto colormap = quadrants(solid_image(2, 2, {0, 0, 0, 255}));
	// A neutral normal/light pair quantizes the later DOT3 pass to zero. The
	// overlay draw itself must contribute NO alpha: retail runs the .til loop
	// under COLORWRITEENABLE = 7, so the page alpha is exclusively the DOT3
	// term the terrain lighting pass consumes.
	// [orig: PolyTrn_RenderTile SetRenderState(0xA8, 7) @ 0x60DD6B..0x60DD73]
	const auto normal = quadrants(solid_image(2, 2, {128, 128, 128, 128}));
	const std::vector<Rgba8Image> levels = tile_levels(tilestrip);
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.tile_info = &single;
	sources.tilestrip = &levels;
	sources.light_bytes = {128, 128, 128};
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(2, page_key), sources);
	if (!expect(page.is_valid(), "til entry page composes")) return false;
	if (!expect(levels.size() > 2, "the tile-set atlas carries its box mip 2")) return false;

	// A lod-2 page spans 256 world units over 256 pixels: one page pixel per
	// world unit, so the 16-unit entry covers 16x16 pixels and each pixel's
	// footprint is 4 atlas texels — the point sample lands on mip 2, at the
	// texel under the pixel's (flipped/rotated) cell position.
	// [orig: PolyTrn_DrawTileOverlayQuad half-texel @ 0x604808..0x6048FD; atlas flags
	// 0x100203 @ 0x604B24 (POINT min/mag/mip, CLAMP)]
	const Rgba8Image &mip = levels[2];
	const opennova::TilAtlasLayout layout = opennova::til_make_atlas_layout(
			static_cast<int>(tilestrip.width), static_cast<int>(tilestrip.height));
	const int cell_texels = opennova::TIL_ATLAS_TILE_PIXELS / 4;
	const int cell_x = (entry.tile_index % layout.tiles_x) * cell_texels;
	const int cell_y = (entry.tile_index / layout.tiles_x) * cell_texels;
	const int origin_x = static_cast<int>(std::lround(
			opennova::til_world_x_from_fixed(entry.x_fixed)));
	const int origin_z = static_cast<int>(std::lround(
			opennova::til_world_z_from_fixed(entry.z_fixed)));
	const int page_x = origin_x -
			(page_key.sector_origin_x + page_key.page_local_x);
	const int page_y = origin_z -
			(page_key.sector_origin_z + page_key.page_local_z);
	uint64_t hash = UINT64_C(1469598103934665603);
	uint64_t actual_alpha_hash = UINT64_C(1469598103934665603);
	uint64_t retail_alpha_hash = UINT64_C(1469598103934665603);
	int alpha_mismatches = 0;
	for (int z = 0; z < opennova::TIL_CELL_WORLD_UNITS; ++z) {
		for (int x = 0; x < opennova::TIL_CELL_WORLD_UNITS; ++x) {
			const size_t actual_offset = 4u *
					(static_cast<size_t>(page_y + z) * page.width + page_x + x);
			const opennova::TilUv local = opennova::til_transform_local_uv(
					opennova::TilUv{(x + 0.5f) / opennova::TIL_CELL_WORLD_UNITS,
							(z + 0.5f) / opennova::TIL_CELL_WORLD_UNITS},
					entry.flags);
			const int texel_x = cell_x + static_cast<int>(
					std::floor(local.u * cell_texels));
			const int texel_y = cell_y + static_cast<int>(
					std::floor(local.v * cell_texels));
			const std::array<uint8_t, 4> texel = pixel(mip, texel_x, texel_y);
			const std::array<uint8_t, 4> expected = overlay_render_target_pixel(
					{0, 0, 0, 0}, texel, {1.0f, 1.0f, 1.0f});
			for (int channel = 0; channel < 3; ++channel) {
				const uint8_t actual = page.pixels[actual_offset + channel];
				if (!expect(actual == expected[channel],
						"til page is the point-sampled mip-2 texel under each pixel")) {
					std::fprintf(stderr, "  entry=%d pixel=(%d,%d) channel=%d actual=%u expected=%u\n",
							entry_index, x, z, channel, actual, expected[channel]);
					return false;
				}
				hash = fnv_byte(hash, actual);
			}
			const uint8_t actual_alpha = page.pixels[actual_offset + 3];
			const uint8_t retail_alpha = 0;  // alpha write is masked off
			actual_alpha_hash = fnv_byte(actual_alpha_hash, actual_alpha);
			retail_alpha_hash = fnv_byte(retail_alpha_hash, retail_alpha);
			if (actual_alpha != retail_alpha) ++alpha_mismatches;
		}
	}
	if (!expect(hash == expected_rgb_hash,
			"til entry RGB hash pins atlas cell, flags, and TGA row orientation")) {
		std::fprintf(stderr, "  entry=%d actual_hash=%016llx expected_hash=%016llx\n",
				entry_index, static_cast<unsigned long long>(hash),
				static_cast<unsigned long long>(expected_rgb_hash));
		return false;
	}
	if (!expect(retail_alpha_hash == expected_alpha_hash,
			"til source alpha hash pins the retail TGA channel and blend equation")) {
		std::fprintf(stderr,
				"  entry=%d retail_hash=%016llx expected_hash=%016llx\n",
				entry_index,
				static_cast<unsigned long long>(retail_alpha_hash),
				static_cast<unsigned long long>(expected_alpha_hash));
		return false;
	}
	if (!expect(alpha_mismatches == 0 &&
			actual_alpha_hash == expected_alpha_hash,
			"til page preserves retail overlay alpha before the DOT3 add")) {
		std::fprintf(stderr,
				"  entry=%d alpha_mismatches=%d actual_hash=%016llx "
				"retail_hash=%016llx\n",
				entry_index, alpha_mismatches,
				static_cast<unsigned long long>(actual_alpha_hash),
				static_cast<unsigned long long>(retail_alpha_hash));
		return false;
	}
	std::printf("PASS: til entry %d RGB/alpha hashes %016llx/%016llx\n",
			entry_index, static_cast<unsigned long long>(hash),
			static_cast<unsigned long long>(actual_alpha_hash));
	return true;
}

// Mount the retail install (OPENNOVA_JO_DIR) — base, then every expansion it
// ships — and read the two oracle files from the first mount carrying both.
// Which expansion carries a retail SKU's tile set is machine-specific (the
// JO:CA unified install ships jox01 without the CP12 files; JOTAC ships
// revx02 with them), so an install without them is an environment condition,
// not a regression: the leg reports SKIP-LEG and the synthetic legs stand.
bool read_retail_tile_pair(const char *til_name, const char *tga_name,
		std::vector<uint8_t> &til_bytes, std::vector<uint8_t> &tga_bytes,
		const char *leg) {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg((std::string("OPENNOVA_JO_DIR (") + leg + ")").c_str());
		return false;
	}
	std::vector<std::string> mounts{std::string()};
	for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
	for (const std::string &expansion : mounts) {
		opennova::Vfs vfs;
		if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) continue;
		til_bytes.clear();
		tga_bytes.clear();
		if (vfs.read_file(til_name, til_bytes) && vfs.read_file(tga_name, tga_bytes))
			return true;
	}
	retail::skip_leg((std::string("an install carrying ") + til_name + "/" + tga_name +
			" (" + leg + ")").c_str());
	return false;
}

bool test_optional_cp12_assets() {
	std::vector<uint8_t> til_bytes;
	std::vector<uint8_t> tga_bytes;
	if (!read_retail_tile_pair("CP12.TIL", "TRNTILEA1.TGA", til_bytes, tga_bytes,
			"CP12 tile composer oracle")) return true;
	opennova::TilFile tiles;
	std::string error;
	if (!expect(opennova::load_til(
				til_bytes.data(), til_bytes.size(), tiles, error),
			"CP12.TIL parses")) return false;
	Rgba8Image tilestrip;
	if (!expect(decode_uncompressed_tga_rgba(tga_bytes, tilestrip),
			"TRNTILEA1.TGA decodes into normalized top-origin RGBA rows")) return false;
	if (!expect(tiles.entries.size() > 1013,
			"CP12 contains witnessed entries 53 and 1013")) return false;

	const opennova::TilOverlayEntry &entry_53 = tiles.entries[53];
	const opennova::TilOverlayEntry &entry_1013 = tiles.entries[1013];
	if (!expect(static_cast<int>(std::lround(
				opennova::til_world_x_from_fixed(entry_53.x_fixed))) == -426 &&
			static_cast<int>(std::lround(
				opennova::til_world_z_from_fixed(entry_53.z_fixed))) == -1560 &&
			entry_53.tile_index == 42 && entry_53.flags == 7,
			"CP12 entry 53 matches the witnessed tile/flags/placement")) return false;
	if (!expect(static_cast<int>(std::lround(
				opennova::til_world_x_from_fixed(entry_1013.x_fixed))) == -426 &&
			static_cast<int>(std::lround(
				opennova::til_world_z_from_fixed(entry_1013.z_fixed))) == -1576 &&
			entry_1013.tile_index == 41 && entry_1013.flags == 7,
			"CP12 entry 1013 matches the witnessed tile/flags/placement")) return false;
	const opennova::TerrainTilePageKey cp12_key{-512, -2048, 0, 256, 2};
	return til_entry_oracle(entry_53, tilestrip, 53, cp12_key,
			UINT64_C(0xd99b29b978eb8576),
			UINT64_C(0xdce53c1df8560f83)) &&
			til_entry_oracle(entry_1013, tilestrip, 1013, cp12_key,
					UINT64_C(0x6e24c8ae86b9ad45),
					UINT64_C(0xdce53c1df8560f83));
}

// D-TIL-4 oracle: the 00TRa driving-course fork retail draws through the
// 00tra-tire-marks-retail fixture camera is built from ROTATE_90-plus-single-
// flip entries (0x05/0x06) — exactly the combos where retail's mirror-then-
// corner-cycle order diverges from flip-then-rotate. The CP12 oracles are
// flags-7 and blind to that order, so this leg pins one entry of each combo.
// Gated like the CP12 leg (docs/asset-gated-tests.md).
bool test_optional_00tra_fork_oracle() {
	std::vector<uint8_t> til_bytes;
	std::vector<uint8_t> tga_bytes;
	if (!read_retail_tile_pair("00TRA.TIL", "TRNTILE10.TGA", til_bytes, tga_bytes,
			"00TRa fork oracle")) return true;
	opennova::TilFile tiles;
	std::string error;
	if (!expect(opennova::load_til(
				til_bytes.data(), til_bytes.size(), tiles, error),
			"00TRA.TIL parses for the fork oracle")) return false;
	Rgba8Image tilestrip;
	if (!expect(decode_uncompressed_tga_rgba(tga_bytes, tilestrip),
			"TRNTILE10.TGA decodes for the fork oracle")) return false;
	if (!expect(tiles.entries.size() > 781,
			"00TRa contains witnessed fork entries 761 and 781")) return false;

	const opennova::TilOverlayEntry &entry_761 = tiles.entries[761];
	const opennova::TilOverlayEntry &entry_781 = tiles.entries[781];
	if (!expect(static_cast<int>(std::lround(
				opennova::til_world_x_from_fixed(entry_761.x_fixed))) == 317 &&
			static_cast<int>(std::lround(
				opennova::til_world_z_from_fixed(entry_761.z_fixed))) == 350 &&
			entry_761.tile_index == 42 && entry_761.flags == 0x05,
			"00TRa entry 761 matches the witnessed tile/flags/placement")) return false;
	if (!expect(static_cast<int>(std::lround(
				opennova::til_world_x_from_fixed(entry_781.x_fixed))) == 308 &&
			static_cast<int>(std::lround(
				opennova::til_world_z_from_fixed(entry_781.z_fixed))) == 363 &&
			entry_781.tile_index == 42 && entry_781.flags == 0x06,
			"00TRa entry 781 matches the witnessed tile/flags/placement")) return false;
	const opennova::TerrainTilePageKey fork_key{0, 0, 256, 256, 2};
	// These are regression pins for the retail archive payload
	// TRNTILE10.TGA (SHA-256 eb3b25ca50f66f2006668198919c8e25374d093c0290e9aceb613ee37d8bc490).
	// The transform itself is independently witnessed in PolyTrn_DrawTileOverlayQuad
	// and pinned synthetically by til_render_uv_test; the per-pixel mip-2
	// comparison above shares til_transform_local_uv and therefore proves
	// placement/channel/level parity, not transform independence. On that
	// payload, under retail's point-sampled box level 2 on a 1:1 LOD-2 page,
	// 0x05 -> T(x,z)=(z,x) hashes d0eea35d8f11850b and
	// 0x06 -> T(x,z)=(1-z,1-x) hashes 1cb6a4f7cec6660f. Alpha pins are the
	// all-zero page hash: the overlay loops run under COLORWRITEENABLE = 7
	// and never write A.
	return til_entry_oracle(entry_761, tilestrip, 761, fork_key,
			UINT64_C(0x5242a373347a260e),
			UINT64_C(0xdce53c1df8560f83)) &&
			til_entry_oracle(entry_781, tilestrip, 781, fork_key,
					UINT64_C(0x5d281e2b51b47088),
					UINT64_C(0xdce53c1df8560f83));
}

} // namespace

// The tile-set atlas is created DXT5 (flags 0x100203): the composer's level
// set is D3DX's level chain read back as bytes, a solid cell comes back as
// its 5:6:5 colour with its alpha, and the chain ends at 4x4.
// [orig: Terrain_LoadTileSetAtlas @ 0x604B24; GTexture_CreateFromPixelData_0
// @ 0x687717..0x687727, @ 0x6877BA..0x687801, @ 0x6878A0, @ 0x6878BE]
bool test_tile_set_levels_are_the_dxt5_decode() {
	Rgba8Image atlas = solid_image(64, 64, {200, 80, 40, 64});
	fill_rect(atlas, 0, 0, 32, 32, {30, 210, 40, 128});
	for (int x = 32; x < 64; x += 2) fill_rect(atlas, x, 32, x + 1, 64, {250, 90, 10, 200});
	const std::vector<Rgba8Image> levels =
			opennova::terrain::build_terrain_tile_set_mips(atlas);
	if (!expect(levels.size() == 5 && levels.back().width == 4 &&
			levels.back().height == 4, "a 64x64 atlas carries five levels down to 4x4")) {
		return false;
	}
	if (!expect(pixel(levels[0], 40, 8) == std::array<uint8_t, 4>{197, 81, 41, 64} &&
			pixel(levels[0], 8, 8) == std::array<uint8_t, 4>{33, 210, 41, 128},
			"solid cells read back as their 5:6:5 colours with exact alpha")) {
		return false;
	}
	const std::vector<opennova::renderer::DxtSurface> chain =
			opennova::renderer::build_dxt_texture_levels(atlas.pixels.data(), 64, 64,
					opennova::renderer::TextureDxtFormat::Dxt5, 5);
	for (size_t level = 0; level < levels.size(); ++level) {
		if (!expect(chain.size() == levels.size() &&
				levels[level].pixels == opennova::renderer::encode_rgba8(
						opennova::renderer::decode_dxt_surface(chain[level])),
				"every atlas level is its DXT5 blocks decoded")) {
			return false;
		}
	}
	return expect(levels[0].pixels != atlas.pixels,
			"the composer no longer samples the raw TGA texels");
}

// The colormap quadrants follow the session's texcompression_level: 0x100001
// with the DXT5 request and its DXT1 fallback below 2, so DXT1 on the reference
// card, their levels the blocks decoded; 0x100001 alone from 2, D3DX's box chain
// of the stored bytes. The map's colormap is the quadrants' first levels put back
// together (D-RMAT-24).
// [orig: PolyTrn_InitTextures @ 0x60ABAD..0x60ABC6, @ 0x60B515..0x60B51C;
// GTexture_CreateFromPixelData_0 @ 0x687717..0x687766, @ 0x6878A0..0x6878BE]
bool test_colormap_quadrants_follow_texcompression() {
	if (!expect(opennova::terrain::terrain_colormap_quadrant_flags(0) == 0x500201u &&
			opennova::terrain::terrain_colormap_quadrant_flags(1) == 0x500201u &&
			opennova::terrain::terrain_colormap_quadrant_flags(2) == 0x100001u &&
			opennova::terrain::terrain_colormap_quadrant_flags(5) == 0x100001u,
			"the colormap quadrants' flags follow the compression word")) {
		return false;
	}
	Rgba8Image atlas = solid_image(32, 32, {200, 80, 40, 255});
	for (uint32_t i = 0; i < atlas.pixels.size(); i += 4) {
		atlas.pixels[i] = static_cast<uint8_t>((i * 13) % 256);
		atlas.pixels[i + 1] = static_cast<uint8_t>((i * 7) % 256);
	}
	const auto compressed = opennova::terrain::build_terrain_tile_quadrant_source(atlas,
			opennova::terrain::terrain_colormap_quadrant_flags(1));
	const auto plain = opennova::terrain::build_terrain_tile_quadrant_source(atlas,
			opennova::terrain::terrain_colormap_quadrant_flags(2));
	// Quadrant 2 (x half 1, z half 0) is the atlas' top-right 16x16 block.
	Rgba8Image block = solid_image(16, 16, {0, 0, 0, 0});
	for (uint32_t y = 0; y < 16; ++y) {
		std::copy(atlas.pixels.begin() + 4u * (y * 32u + 16u),
				atlas.pixels.begin() + 4u * (y * 32u + 32u),
				block.pixels.begin() + 4u * y * 16u);
	}
	const std::vector<opennova::renderer::DxtSurface> chain =
			opennova::renderer::build_dxt_texture_levels(block.pixels.data(), 16, 16,
					opennova::renderer::TextureDxtFormat::Dxt1, 3);
	if (!expect(compressed.is_valid() && compressed.quadrants[2].size() == 3 &&
			compressed.quadrants[2][0].pixels == opennova::renderer::encode_rgba8(
					opennova::renderer::decode_dxt_surface(chain[0])) &&
			compressed.quadrants[2][2].pixels == opennova::renderer::encode_rgba8(
					opennova::renderer::decode_dxt_surface(chain[2])),
			"a compressed quadrant is its DXT1 chain decoded")) {
		return false;
	}
	if (!expect(plain.is_valid() && plain.quadrants[2].size() == 3 &&
			plain.quadrants[2][0].pixels == block.pixels &&
			plain.quadrants[2][1].pixels == opennova::renderer::encode_rgba8(
					opennova::renderer::box_filter_half(opennova::renderer::decode_rgba8(
							block.pixels.data(), 16, 16), 16, 16)),
			"an uncompressed quadrant is D3DX's box chain of its bytes")) {
		return false;
	}
	const Rgba8Image map_plain = opennova::terrain::terrain_colormap_device_image(atlas, 2);
	const Rgba8Image map_compressed = opennova::terrain::terrain_colormap_device_image(atlas, 1);
	return expect(map_plain.pixels == atlas.pixels &&
			pixel(map_compressed, 20, 3) == pixel(compressed.quadrants[2][0], 4, 3) &&
			map_compressed.pixels != atlas.pixels,
			"the map's colormap is the quadrants' first levels put back together");
}

// The colour map's fixed side and the quadrant split's overrun: the split takes the map's width as its side,
// so a map shorter than its even width is read and written past.
bool test_map_split_rules() {
	using opennova::terrain::kTerrainColourMapSide;
	using opennova::terrain::terrain_map_split_overruns;
	return expect(kTerrainColourMapSide == 1024 && !terrain_map_split_overruns(1024, 1024) &&
			!terrain_map_split_overruns(512, 1024) && terrain_map_split_overruns(1024, 512) &&
			!terrain_map_split_overruns(3, 2) && terrain_map_split_overruns(4, 3),
			"a map shorter than its even width overruns the split");
}

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
	if (!test_map_split_rules()) return 1;
	if (!test_colormap_quadrants_follow_texcompression()) return 1;
	if (!test_tile_set_levels_are_the_dxt5_decode()) return 1;
	if (!test_level_density()) return 1;
	if (!test_flat_page_source_and_overlay_gate()) return 1;
	if (!test_rejects_source_atlases_without_a_complete_quadrant()) return 1;
	if (!test_source_orientation_quadrant_clamp_and_dot3()) return 1;
	if (!test_overlay_atlas_flags_tint_clipping_and_order()) return 1;
	if (!test_base_pass_samples_integer_pixel_positions()) return 1;
	if (!test_base_pass_coarse_page_samples_box_level_one()) return 1;
	if (!test_til_line_is_point_sampled_one_to_one()) return 1;
	if (!test_til_coarse_page_takes_box_level()) return 1;
	if (!test_optional_cp12_assets()) return 1;
	if (!test_optional_00tra_fork_oracle()) return 1;
	std::printf("OK: terrain tile page composition\n");
	return 0;
}
