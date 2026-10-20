// Terrain tile-page composer: portable pixel-oracle coverage for the 128-page
// device cache. Synthetic cases pin page density, quadrant-locked source
// sampling, DOT3 byte order, .til atlas orientation, tint/alpha, clipping, and
// painter order; the witnessed CP12 and 00TRa entries over a minted atlas.
#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>
#include <runtime/terrain/terrain_tile_quadrant_update.h>
#include <runtime/renderer/texture_dxt.h>

#include <formats/tga/tga.h>
#include <formats/til/til.h>
#include <formats/til/til_io.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
	job.layout.texture_dimension = opennova::TerrainTileCompositionCache::kDimensionHigh;
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

// An index past the atlas's cells: the render divides by the set's columns with no bound on the row
// [orig: PolyTrn_RenderTile @ 0x60DEBF..0x60DF05], so the quad lies below the atlas and its CLAMP
// addressing draws the last texel row across the column's span (D-TIL-7) [orig: Terrain_LoadTileSetAtlas
// @ 0x604B4E, flags 0x100203]. A 3 x 2 set: index 7 is column 1, row 2 (past the 2 rows); a 4 x 1 set: index
// 4 is column 0, row 1. A set under 64 wide is one column as wide as itself [orig: @ 0x604B97].
bool test_til_index_past_the_atlas_draws_the_clamped_edge() {
	const std::array<uint8_t, 3> base_color{40, 80, 120};
	const std::array<uint8_t, 3> base_normal{128, 128, 255};
	const std::array<uint8_t, 3> light{128, 128, 255};
	const auto colormap = quadrants(solid_image(
			2, 2, {base_color[0], base_color[1], base_color[2], 255}));
	const auto normal = quadrants(solid_image(
			2, 2, {base_normal[0], base_normal[1], base_normal[2], 255}));
	const std::array<uint8_t, 4> base = base_pixel(base_color, base_normal, light);
	const std::array<float, 3> white{1.0f, 1.0f, 1.0f};
	const std::array<uint8_t, 4> body{10, 20, 30, 255};
	const std::array<uint8_t, 4> left{200, 40, 40, 255};
	const std::array<uint8_t, 4> middle{40, 200, 40, 255};
	const std::array<uint8_t, 4> right{40, 40, 200, 255};
	const auto page_of = [&](const Rgba8Image &atlas, uint8_t index) {
		const auto levels = tile_levels(atlas);
		opennova::TilFile tiles;
		tiles.entries.push_back(opennova::make_til_overlay_entry(0, 0, index, 0));
		opennova::terrain::TerrainTilePageSourceView sources;
		sources.colormap = &colormap;
		sources.heightfield_normal = &normal;
		sources.tile_info = &tiles;
		sources.tilestrip = &levels;
		sources.light_bytes = light;
		return opennova::terrain::compose_terrain_tile_page(cold_job(4), sources);
	};
	// Each column's last block row (texel rows 124..127, a solid DXT5 block) its own colour, the rest the body.
	Rgba8Image three = solid_image(192, 128, body);
	fill_rect(three, 0, 124, 64, 128, left);
	fill_rect(three, 64, 124, 128, 128, middle);
	fill_rect(three, 128, 124, 192, 128, right);
	const Rgba8Image past = page_of(three, 7);
	if (!expect(past.is_valid(), "a page with an index past the atlas composes")) return false;
	const std::array<uint8_t, 4> edge = overlay_pixel(base, dxt5_texel(middle), white);
	for (const int at : {0, 20, 40, 63})
		if (!expect_pixel(past, at, at, edge, "index 7 of a 3 x 2 set draws column 1's last texel row")) return false;
	if (!expect_pixel(past, 64, 64, base, "the tile covers its 16 units alone")) return false;
	// A 4 x 1 set: index 4 is column 0, a row below; column 0's last block row is its own.
	Rgba8Image four = solid_image(256, 64, body);
	fill_rect(four, 0, 60, 64, 64, left);
	fill_rect(four, 64, 60, 256, 64, right);
	const Rgba8Image below = page_of(four, 4);
	if (!expect(below.is_valid(), "a page with index 4 of a 4 x 1 set composes")) return false;
	if (!expect_pixel(below, 32, 32, overlay_pixel(base, dxt5_texel(left), white),
			"index 4 of a 4 x 1 set draws column 0's last texel row")) return false;
	// A set 32 wide: one column, index 0 drawn over the whole set's width.
	Rgba8Image thin = solid_image(32, 64, body);
	fill_rect(thin, 0, 0, 16, 64, left);
	fill_rect(thin, 16, 0, 32, 64, right);
	const Rgba8Image narrow = page_of(thin, 0);
	if (!expect(narrow.is_valid(), "a page over a set 32 wide composes")) return false;
	return expect_pixel(narrow, 8, 8, overlay_pixel(base, dxt5_texel(left), white),
			"a set 32 wide draws its left half over the tile's first half") &&
			expect_pixel(narrow, 56, 8, overlay_pixel(base, dxt5_texel(right), white),
					"a set 32 wide draws its right half over the tile's second half");
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

// A tile-set atlas of our own in the shipped atlases' form (TRNTILE10.TGA's and trntileA1.tga's: 512 x 512, 32
// bits, the TGA 2.0 footer), its texels from integer data and no two cells alike: written by the TGA writer and
// read back as the oracle legs read the shipped ones, so the hashes pin the atlas cell, the flags and the TGA row
// orientation over it.
bool minted_tile_atlas(Rgba8Image &tilestrip) {
	constexpr uint32_t kSide = 512;
	std::vector<uint8_t> rgba(size_t(kSide) * kSide * 4u);
	for (uint32_t y = 0; y < kSide; ++y)
		for (uint32_t x = 0; x < kSide; ++x) {
			uint8_t *p = &rgba[(size_t(y) * kSide + x) * 4u];
			p[0] = uint8_t(x * 7 + y * 3);
			p[1] = uint8_t(x ^ (y * 5));
			p[2] = uint8_t(((x >> 6) + (y >> 6) * 8) * 4);
			p[3] = 255;
		}
	std::vector<uint8_t> tga;
	std::string error;
	return expect(opennova::tga::tga_write(rgba.data(), kSide, kSide,
						  opennova::tga::TgaForm{32, opennova::tga::TGA_DESCRIPTOR_ALPHA_8, true}, tga, error),
			       "the minted atlas writes as a shipped atlas's TGA") &&
			expect(decode_uncompressed_tga_rgba(tga, tilestrip),
					"the minted atlas decodes into normalized top-origin RGBA rows");
}

opennova::TilOverlayEntry witnessed_entry(int x, int z, uint8_t tile_index, uint8_t flags) {
	opennova::TilOverlayEntry entry;
	entry.x_fixed = opennova::til_x_fixed_from_world(x);
	entry.z_fixed = opennova::til_z_fixed_from_world(z);
	entry.tile_index = tile_index;
	entry.flags = flags;
	return entry;
}

bool placed_at(const opennova::TilOverlayEntry &entry, int x, int z) {
	return static_cast<int>(std::lround(opennova::til_world_x_from_fixed(entry.x_fixed))) == x &&
			static_cast<int>(std::lround(opennova::til_world_z_from_fixed(entry.z_fixed))) == z;
}

// The CP12 oracle: JOTAC's CP12.TIL entries 53 and 1013 (witnessed: tile 42 at (-426, -1560) and tile 41 at
// (-426, -1576), both flags 7, on the LOD-2 page at sector (-512, -2048), local (0, 256)), composed over the minted
// atlas. The retail run matched them over TRNTILEA1.TGA (RGB hashes d99b29b978eb8576 and 6e24c8ae86b9ad45); the
// per-pixel mip-2 comparison in til_entry_oracle is the rule, the hashes its pin over this atlas.
bool test_cp12_entries() {
	Rgba8Image tilestrip;
	if (!minted_tile_atlas(tilestrip)) return false;
	const opennova::TilOverlayEntry entry_53 = witnessed_entry(-426, -1560, 42, 7);
	const opennova::TilOverlayEntry entry_1013 = witnessed_entry(-426, -1576, 41, 7);
	if (!expect(placed_at(entry_53, -426, -1560) && placed_at(entry_1013, -426, -1576),
			"the CP12 entries stand at the witnessed placements")) return false;
	const opennova::TerrainTilePageKey cp12_key{-512, -2048, 0, 256, 2};
	return til_entry_oracle(entry_53, tilestrip, 53, cp12_key,
			UINT64_C(0x329ca6f063ae9cb4),
			UINT64_C(0xdce53c1df8560f83)) &&
			til_entry_oracle(entry_1013, tilestrip, 1013, cp12_key,
					UINT64_C(0x7fe1efb0b6d006e9),
					UINT64_C(0xdce53c1df8560f83));
}

// D-TIL-4 oracle: the 00TRa driving-course fork retail draws through the
// 00tra-tire-marks-retail fixture camera is built from ROTATE_90-plus-single-
// flip entries (0x05/0x06) — exactly the combos where retail's mirror-then-
// corner-cycle order diverges from flip-then-rotate. The CP12 oracles are
// flags-7 and blind to that order, so this leg pins one entry of each combo:
// JO:CA's 00TRA.TIL entries 761 (tile 42 at (317, 350), flags 0x05) and 781
// (tile 42 at (308, 363), flags 0x06) on the LOD-2 page at local (256, 256),
// composed over the minted atlas. The transform itself is independently
// witnessed in PolyTrn_DrawTileOverlayQuad and pinned synthetically by
// til_render_uv_test; the per-pixel mip-2 comparison shares
// til_transform_local_uv and therefore proves placement/channel/level parity,
// not transform independence. The retail run matched them over TRNTILE10.TGA
// (SHA-256 eb3b25ca50f66f2006668198919c8e25374d093c0290e9aceb613ee37d8bc490;
// RGB hashes 5242a373347a260e and 5d281e2b51b47088). Alpha pins are the
// all-zero page hash: the overlay loops run under COLORWRITEENABLE = 7 and
// never write A.
bool test_00tra_fork_entries() {
	Rgba8Image tilestrip;
	if (!minted_tile_atlas(tilestrip)) return false;
	const opennova::TilOverlayEntry entry_761 = witnessed_entry(317, 350, 42, 0x05);
	const opennova::TilOverlayEntry entry_781 = witnessed_entry(308, 363, 42, 0x06);
	if (!expect(placed_at(entry_761, 317, 350) && placed_at(entry_781, 308, 363),
			"the 00TRa fork entries stand at the witnessed placements")) return false;
	const opennova::TerrainTilePageKey fork_key{0, 0, 256, 256, 2};
	return til_entry_oracle(entry_761, tilestrip, 761, fork_key,
			UINT64_C(0xa3c150933928948e),
			UINT64_C(0xdce53c1df8560f83)) &&
			til_entry_oracle(entry_781, tilestrip, 781, fork_key,
					UINT64_C(0xac15ff6710402b00),
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
	if (!expect(compressed.is_valid() && compressed.levels(2).size() == 3 &&
			compressed.levels(2)[0].pixels == opennova::renderer::encode_rgba8(
					opennova::renderer::decode_dxt_surface(chain[0])) &&
			compressed.levels(2)[2].pixels == opennova::renderer::encode_rgba8(
					opennova::renderer::decode_dxt_surface(chain[2])) &&
			compressed.quadrants[2]->dxt.size() == 3 && compressed.quadrants[2]->dxt[1].blocks == chain[1].blocks,
			"a compressed quadrant is its DXT1 chain decoded, its blocks kept")) {
		return false;
	}
	if (!expect(plain.is_valid() && plain.levels(2).size() == 3 && plain.quadrants[2]->dxt.empty() &&
			plain.levels(2)[0].pixels == block.pixels &&
			plain.levels(2)[1].pixels == opennova::renderer::encode_rgba8(
					opennova::renderer::box_filter_half(opennova::renderer::decode_rgba8(
							block.pixels.data(), 16, 16), 16, 16)),
			"an uncompressed quadrant is D3DX's box chain of its bytes")) {
		return false;
	}
	const Rgba8Image map_plain = opennova::terrain::terrain_colormap_device_image(atlas, 2);
	const Rgba8Image map_compressed = opennova::terrain::terrain_colormap_device_image(atlas, 1);
	return expect(map_plain.pixels == atlas.pixels &&
			pixel(map_compressed, 20, 3) == pixel(compressed.levels(2)[0], 4, 3) &&
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

// An editor's tile edit retires only the pages its changed records' squares overlap: a record changed in place
// names its old and new squares, an added or removed one its own, an unchanged one none; a reorder names every
// index it moved past.
bool test_overlay_changes() {
	using opennova::terrain::TerrainTileSquareQ16;
	using opennova::terrain::terrain_tile_overlay_changes;
	opennova::TilFile before;
	before.entries.resize(3);
	before.entries[0].x_fixed = 0;
	before.entries[0].z_fixed = 0;
	before.entries[1].x_fixed = 32 << 16;
	before.entries[1].z_fixed = -(16 << 16);
	before.entries[1].tile_index = 4;
	before.entries[2].x_fixed = 64 << 16;
	before.entries[2].tile_index = 9;
	if (!expect(terrain_tile_overlay_changes(before, before).empty(), "the same placement changes no square")) return false;
	opennova::TilFile turned = before;
	turned.entries[1].flags = 4;
	std::vector<TerrainTileSquareQ16> squares = terrain_tile_overlay_changes(before, turned);
	if (!expect(squares.size() == 2 && squares[0].minimum_x == (32 << 16) && squares[0].minimum_z == (16 << 16) &&
			squares[0].maximum_x == (48 << 16) && squares[0].maximum_z == (32 << 16), "a turn names the record's square, z negated"))
		return false;
	opennova::TilFile moved = before;
	moved.entries[0].x_fixed = 100 << 16;
	squares = terrain_tile_overlay_changes(before, moved);
	if (!expect(squares.size() == 2 && squares[0].minimum_x == 0 && squares[1].minimum_x == (100 << 16),
			"a move names the square it left and the one it took"))
		return false;
	opennova::TilFile added = before;
	added.entries.push_back(before.entries[0]);
	added.entries.back().x_fixed = 200 << 16;
	squares = terrain_tile_overlay_changes(before, added);
	if (!expect(squares.size() == 1 && squares[0].minimum_x == (200 << 16), "an added record names its own square")) return false;
	squares = terrain_tile_overlay_changes(added, before);
	if (!expect(squares.size() == 1 && squares[0].minimum_x == (200 << 16), "a removed record names its own square")) return false;
	opennova::TilFile reordered = before;
	std::swap(reordered.entries[0], reordered.entries[2]);
	squares = terrain_tile_overlay_changes(before, reordered);
	return expect(squares.size() == 4, "a reorder names every index it moved past, the unmoved middle none");
}

// An editor's paint of the colour map makes again only the quadrants its rect touches, each the bytes a build of the
// painted map holds; an atlas of another size makes none.
// A painted rect carried through the quadrants in place (an editor's paint, ADR 0046 S25): every level of every
// quadrant the bytes a build of the painted map makes, under the uncompressed flags and the DXT1 ones (a session's
// texcompression_level 1, the default), for a rect inside one quadrant, one across the quadrants' seams, one at the
// map's far corner, one texel and the whole map; the quadrants it misses keep their chains (shared, not copied).
bool test_quadrant_update() {
	using opennova::terrain::Rgba8Image;
	using opennova::terrain::TerrainTileQuadrantChange;
	using opennova::terrain::TerrainTileQuadrantSource;
	Rgba8Image atlas;
	atlas.width = 128;
	atlas.height = 128;
	atlas.pixels.resize(128 * 128 * 4);
	for (size_t i = 0; i < atlas.pixels.size(); ++i)
		atlas.pixels[i] = static_cast<uint8_t>((i * 7 + (i / 512) * 13 + 3) % 251);
	struct Paint {
		int x, y, w, h;
		unsigned quadrants;
	};
	const Paint paints[] = {
			{10, 40, 4, 4, 1u << 0},              // quadrant 0 (x's first half, y's first)
			{60, 60, 9, 7, 0xFu},                 // across both seams: all four
			{124, 121, 4, 7, 1u << 3},            // the far corner: quadrant 3
			{33, 97, 1, 1, 1u << 1},              // one texel, quadrant 1 (x's first half, y's second)
			{0, 0, 128, 128, 0xFu},               // the whole map
	};
	for (const uint32_t flags : {opennova::terrain::kTerrainQuadrantCreationFlags,
				 opennova::terrain::terrain_colormap_quadrant_flags(1)}) {
		const bool compressed = flags != opennova::terrain::kTerrainQuadrantCreationFlags;
		for (const Paint &paint : paints) {
			TerrainTileQuadrantSource source = opennova::terrain::build_terrain_tile_quadrant_source(atlas, flags);
			if (!expect(source.is_valid() && source.quadrants[0]->dxt.empty() != compressed,
					"the source keeps a DXT quadrant's blocks")) return false;
			const TerrainTileQuadrantSource before = source;
			Rgba8Image painted = atlas;
			for (int y = paint.y; y < paint.y + paint.h; ++y)
				for (int x = paint.x; x < paint.x + paint.w; ++x) {
					uint8_t *texel = &painted.pixels[4 * (static_cast<size_t>(y) * 128 + x)];
					texel[0] = 255;
					texel[1] = static_cast<uint8_t>(texel[1] / 2);
					texel[2] = 7;
				}
			TerrainTileQuadrantChange change;
			const unsigned made = opennova::terrain::update_terrain_tile_quadrants(
					source, painted, paint.x, paint.y, paint.w, paint.h, &change);
			const TerrainTileQuadrantSource built = opennova::terrain::build_terrain_tile_quadrant_source(painted, flags);
			bool same = made == paint.quadrants && change.quadrants == made;
			for (size_t q = 0; q < 4 && same; ++q) {
				const auto &mine = *source.quadrants[q];
				const auto &theirs = *built.quadrants[q];
				same = mine.levels.size() == theirs.levels.size() && mine.dxt.size() == theirs.dxt.size();
				for (size_t level = 0; same && level < theirs.levels.size(); ++level)
					same = mine.levels[level].pixels == theirs.levels[level].pixels;
				for (size_t level = 0; same && level < theirs.dxt.size(); ++level)
					same = mine.dxt[level].blocks == theirs.dxt[level].blocks;
				// A quadrant the paint missed keeps its chain; one it touched has a new one and each level's change.
				const bool touched = (paint.quadrants >> q) & 1u;
				same = same && (source.quadrants[q] == before.quadrants[q]) == !touched &&
						(change.levels[q].size() == (touched ? mine.levels.size() : 0u));
			}
			if (!expect(same, compressed ? "a painted rect's DXT1 quadrants in place are the painted map's build"
										 : "a painted rect's quadrants in place are the painted map's build"))
				return false;
		}
	}
	TerrainTileQuadrantSource source = opennova::terrain::build_terrain_tile_quadrant_source(atlas);
	Rgba8Image other;
	other.width = 32;
	other.height = 32;
	other.pixels.assign(32 * 32 * 4, 0);
	return expect(opennova::terrain::update_terrain_tile_quadrants(source, other, 0, 0, 4, 4) == 0 &&
			opennova::terrain::update_terrain_tile_quadrants(source, atlas, 200, 0, 4, 4) == 0,
			"a map of another size, or a rect past the map, makes nothing");
}

// Several rects at once (an editor's sculpt: the boxes of the normals a region made again): the bytes a build of the
// changed map holds, a change kept apart for each rect (two far apart in one quadrant never widen into the span
// between them: a page sampling only that span is no page they reach), and nothing made where one rect is refused.
bool test_quadrant_update_rects() {
	using opennova::terrain::CoordsRect;
	using opennova::terrain::Rgba8Image;
	using opennova::terrain::TerrainTileQuadrantChange;
	using opennova::terrain::TerrainTileQuadrantSource;
	Rgba8Image atlas;
	atlas.width = 1024;
	atlas.height = 1024;
	atlas.pixels.resize(size_t(1024) * 1024 * 4);
	for (size_t i = 0; i < atlas.pixels.size(); ++i)
		atlas.pixels[i] = static_cast<uint8_t>((i * 5 + (i / 4096) * 11 + 1) % 253);
	TerrainTileQuadrantSource source = opennova::terrain::build_terrain_tile_quadrant_source(atlas);
	const TerrainTileQuadrantSource before = source;
	// Two columns of quadrant 0 at its two edges (a locked axis's wrapped tap), and one in quadrant 3.
	const std::vector<CoordsRect> rects = {{0, 100, 2, 6}, {510, 100, 2, 6}, {700, 900, 4, 4}};
	Rgba8Image changed = atlas;
	for (const CoordsRect &rect : rects)
		for (int y = rect.z; y < rect.z + rect.h; ++y)
			for (int x = rect.x; x < rect.x + rect.w; ++x) changed.pixels[(size_t(y) * 1024 + size_t(x)) * 4] ^= 0x5A;
	std::vector<TerrainTileQuadrantChange> changes;
	const unsigned made = opennova::terrain::update_terrain_tile_quadrants(source, changed, rects, changes);
	const TerrainTileQuadrantSource built = opennova::terrain::build_terrain_tile_quadrant_source(changed);
	bool same = made == ((1u << 0) | (1u << 3)) && changes.size() == rects.size();
	for (size_t q = 0; q < 4 && same; ++q) {
		const auto &mine = source.levels(q), &theirs = built.levels(q);
		same = mine.size() == theirs.size();
		for (size_t level = 0; same && level < mine.size(); ++level) same = mine[level].pixels == theirs[level].pixels;
	}
	if (!expect(same, "several rects made again are the changed map's build, a change each")) return false;
	if (!expect(source.quadrants[1] == before.quadrants[1] && source.quadrants[2] == before.quadrants[2],
			"the quadrants no rect touches keep their chains")) return false;
	// A level-1 page (64 units, its quadrant's level 0 over 512 x 512) whose origin routes to the middle of
	// quadrant 0's x span, rows 100..106: neither column change reaches it.
	const bool middle = opennova::terrain::terrain_tile_page_samples_any(changes, source, 4, 256, 96, 256);
	const bool edge = opennova::terrain::terrain_tile_page_samples_any(changes, source, 4, 0, 96, 256);
	if (!expect(!middle && edge, "a page samples the rects it reads, never the span between two of them")) return false;
	TerrainTileQuadrantSource untouched = before;
	std::vector<CoordsRect> refused = rects;
	refused.push_back({1020, 0, 8, 2});
	return expect(opennova::terrain::update_terrain_tile_quadrants(untouched, changed, refused, changes) == 0 &&
			changes.empty() && untouched.quadrants[0] == before.quadrants[0],
			"a rect past the map refuses them all, nothing made");
}

// Which resident pages a painted region reaches: by what each samples (the quadrant its source origin routes to, the
// level its footprint picks, the texels its bilinear taps reach), wherever the frame drew it.
bool test_page_samples() {
	using opennova::terrain::Rgba8Image;
	Rgba8Image atlas;
	atlas.width = 1024;
	atlas.height = 1024;
	atlas.pixels.assign(size_t(1024) * 1024 * 4, 90);
	opennova::terrain::TerrainTileQuadrantSource source = opennova::terrain::build_terrain_tile_quadrant_source(atlas);
	opennova::terrain::TerrainTileQuadrantChange corner, middle;
	// The atlas' first texels (the flat page's texel), and texels (300..307, 100..107) of quadrant 0.
	if (!expect(opennova::terrain::update_terrain_tile_quadrants(source, atlas, 0, 0, 2, 2, &corner) == 1u &&
			opennova::terrain::update_terrain_tile_quadrants(source, atlas, 300, 100, 8, 8, &middle) == 1u,
			"the rects change quadrant 0 alone")) return false;
	const auto samples = [&](const opennova::terrain::TerrainTileQuadrantChange &change, uint8_t lod, int sx, int sz) {
		return opennova::terrain::terrain_tile_page_samples(change, source, lod, sx, sz, 256);
	};
	return expect(samples(corner, 0, 0, 0), "the flat page reads texel (0, 0) of quadrant 0") &&
			expect(!samples(middle, 0, 0, 0), "and nothing past it") &&
			expect(samples(middle, 1, 0, 0) && samples(corner, 1, 0, 0), "a level-1 page over the quadrant reads both") &&
			expect(samples(middle, 4, 256, 64) && !samples(middle, 4, 192, 64) && !samples(middle, 4, 320, 64),
					"a 64-unit page reads its own span, its taps one texel past it") &&
			expect(samples(middle, 4, 256, 64 + 512) == false && !samples(middle, 4, 256 + 512, 64),
					"a page routed to another quadrant reads none of it");
}

int main() {
	if (!test_map_split_rules()) return 1;
	if (!test_overlay_changes()) return 1;
	if (!test_quadrant_update()) return 1;
	if (!test_quadrant_update_rects()) return 1;
	if (!test_page_samples()) return 1;
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
	if (!test_til_index_past_the_atlas_draws_the_clamped_edge()) return 1;
	if (!test_cp12_entries()) return 1;
	if (!test_00tra_fork_entries()) return 1;
	std::printf("OK: terrain tile page composition\n");
	return 0;
}
