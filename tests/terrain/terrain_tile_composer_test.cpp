// Terrain tile-page composer: portable pixel-oracle coverage for the 128-page
// device cache. Synthetic cases pin page density, quadrant-locked source
// sampling, DOT3 byte order, .til atlas orientation, tint/alpha, clipping, and
// painter order. The optional CP12 leg follows docs/asset-gated-tests.md.
#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain/terrain_tile_composition_cache.h>

#include <formats/til/til.h>
#include <formats/til/til_io.h>
#include <formats/til/til_overlay_bake.h>
#include <base/vfs/vfs.h>

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
	opennova::TerrainTileCompositionCache cache;
	const auto decision = cache.request(opennova::TerrainTileCompositionRequest{
			page, 0, source_origin_x, source_origin_z,
			opennova::TerrainTileContentStamp{1}});
	return *decision->job;
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
	const Rgba8Image colormap = solid_image(2, 2, {0, 0, 0, 255});
	const Rgba8Image normal = solid_image(2, 2, {128, 128, 255, 128});
	const Rgba8Image tilestrip = solid_image(64, 64, {255, 255, 255, 255});
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

bool test_rejects_source_atlases_without_a_complete_quadrant() {
	const Rgba8Image valid = solid_image(2, 2, {128, 128, 255, 255});
	const std::array<Rgba8Image, 3> tiny = {
			solid_image(1, 2, {128, 128, 255, 255}),
			solid_image(2, 1, {128, 128, 255, 255}),
			solid_image(1, 1, {128, 128, 255, 255}),
	};
	for (const Rgba8Image &source : tiny) {
		opennova::terrain::TerrainTilePageSourceView sources;
		sources.colormap = &source;
		sources.heightfield_normal = &valid;
		if (!expect(!opennova::terrain::compose_terrain_tile_page(
					cold_job(4), sources).is_valid(),
				"a 1xN, Nx1, or 1x1 colormap cannot provide four quadrants")) {
			return false;
		}
	}
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &valid;
	sources.heightfield_normal = &tiny[0];
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
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
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
	// Keep the first column of cell 1 equal to cell 0. Retail's witnessed
	// half-texel shift intentionally lets the last sample of cell 0 straddle
	// that atlas boundary; matching edge texels isolate page clipping here.
	fill_rect(tilestrip, 65, 0, 96, 32, tile_tl);
	fill_rect(tilestrip, 96, 0, 128, 32, tile_tr);
	fill_rect(tilestrip, 65, 32, 96, 64, tile_bl);
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

	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.tile_info = &tiles;
	sources.tilestrip = &tilestrip;
	sources.tile_overlay_tint = tint;
	sources.light_bytes = light;
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(4), sources);
	if (!expect(page.is_valid(), "asymmetric multi-tile page composes")) return false;

	const std::array<uint8_t, 4> edge_result = overlay_pixel(base, edge, tint);
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
	// quadrant seams. [orig: render_water_quad @ 0x604700 — mirrors
	// @ 0x604782/0x6047a9, rotate cycle @ 0x6047d4..0x604806]
	if (!expect_pixel(page, 80, 80, overlay_pixel(base, tile_tl, tint),
			"combined flags map destination TL to source TL")) return false;
	if (!expect_pixel(page, 112, 80, overlay_pixel(base, tile_bl, tint),
			"combined flags map destination TR to source BL")) return false;
	if (!expect_pixel(page, 80, 112, overlay_pixel(base, tile_tr, tint),
			"output rows follow world +Z through the transposed atlas")) return false;
	if (!expect_pixel(page, 112, 112, overlay_pixel(base, tile_br, tint),
			"combined flags map destination BR to source BR")) return false;
	if (!expect(pixel(page, 80, 80)[3] ==
			overlay_pixel(base, tile_tl, tint)[3],
			"overlay draws leave the page alpha to the DOT3 pass")) return false;

	std::array<uint8_t, 4> overlay_target = base;
	overlay_target[3] = 0;
	const std::array<uint8_t, 4> after_first =
			overlay_render_target_pixel(overlay_target, overlap_first, tint);
	const std::array<uint8_t, 4> after_last = add_dot3_alpha(
			overlay_render_target_pixel(after_first, overlap_last, tint), base[3]);
	return expect_pixel(page, 160, 32, after_last,
			"later .til entries source-over earlier entries in file order");
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

int wrap(int value, int size) {
	value %= size;
	return value < 0 ? value + size : value;
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
	const Rgba8Image colormap = solid_image(2, 2, {0, 0, 0, 255});
	// A neutral normal/light pair quantizes the later DOT3 pass to zero. The
	// overlay draw itself must contribute NO alpha: retail runs the .til loop
	// under COLORWRITEENABLE = 7, so the page alpha is exclusively the DOT3
	// term the terrain lighting pass consumes.
	// [orig: PolyTrn_RenderTile SetRenderState(0xA8, 7) @ 0x60DD6B..0x60DD73]
	const Rgba8Image normal = solid_image(2, 2, {128, 128, 128, 128});
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.tile_info = &single;
	sources.tilestrip = &tilestrip;
	sources.light_bytes = {128, 128, 128};
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			cold_job(2, page_key), sources);
	if (!expect(page.is_valid(), "til entry page composes")) return false;

	std::vector<uint8_t> baked;
	if (!expect(opennova::til_bake_overlay_rgba(single,
				tilestrip.pixels.data(), static_cast<int>(tilestrip.width),
				static_cast<int>(tilestrip.height), 1024, 1024, baked),
			"til entry bakes through the established full-atlas oracle")) {
		return false;
	}
	const int origin_x = static_cast<int>(std::lround(
			opennova::til_world_x_from_fixed(entry.x_fixed)));
	const int origin_z = static_cast<int>(std::lround(
			opennova::til_world_z_from_fixed(entry.z_fixed)));
	// A lod-2 page spans 256 world units over 256 texels, so page texel
	// coords are world minus the page's world origin.
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
			const int baked_x = wrap(origin_x + x, 1024);
			const int baked_y = wrap(-(origin_z + z) - 1, 1024);
			const size_t baked_offset = 4u *
					(static_cast<size_t>(baked_y) * 1024u + baked_x);
			const uint8_t alpha = baked[baked_offset + 3];
			for (int channel = 0; channel < 3; ++channel) {
				const uint8_t actual = page.pixels[actual_offset + channel];
				const int expected = static_cast<int>(std::lround(
						baked[baked_offset + channel] *
						static_cast<float>(alpha) / 255.0f));
				if (!expect(std::abs(static_cast<int>(actual) - expected) <= 1,
						"til page agrees with full-atlas placement/orientation")) {
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

bool test_optional_cp12_assets() {
	const char *root = std::getenv("OPENNOVA_JO_DIR");
	if (root == nullptr || root[0] == '\0') {
		std::printf("SKIP: OPENNOVA_JO_DIR not set (CP12 tile composer oracle)\n");
		return true;
	}

	opennova::Vfs vfs;
	if (!expect(vfs.mount_game(root, "revx02", opennova::VfsMountMode::Packed),
			"OPENNOVA_JO_DIR mounts for the CP12 oracle")) return false;
	std::vector<uint8_t> til_bytes;
	std::vector<uint8_t> tga_bytes;
	if (!vfs.read_file("CP12.TIL", til_bytes) ||
			!vfs.read_file("TRNTILEA1.TGA", tga_bytes)) {
		// Asset-gated SKIP-AS-PASS applies to the DATA too: the var can point
		// at a retail SKU whose packed set lacks the CP12 oracle files (the
		// JO:CA unified install ships without them). A set var with an
		// incompatible install is an environment condition, not a regression.
		std::printf("SKIP: mounted install lacks CP12.TIL/TRNTILEA1.TGA "
				"(CP12 tile composer oracle needs an install carrying them)\n");
		return true;
	}
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
			UINT64_C(0x2d98d83388a18b7f),
			UINT64_C(0xdce53c1df8560f83)) &&
			til_entry_oracle(entry_1013, tilestrip, 1013, cp12_key,
					UINT64_C(0x67c1b609d0d0ff60),
					UINT64_C(0xdce53c1df8560f83));
}

// D-TIL-4 oracle: the 00TRa driving-course fork retail draws through the
// 00tra-tire-marks-retail fixture camera is built from ROTATE_90-plus-single-
// flip entries (0x05/0x06) — exactly the combos where retail's mirror-then-
// corner-cycle order diverges from flip-then-rotate. The CP12 oracles are
// flags-7 and blind to that order, so this leg pins one entry of each combo.
// Gated like the CP12 leg (docs/asset-gated-tests.md).
bool test_optional_00tra_fork_oracle() {
	const char *root = std::getenv("OPENNOVA_JO_DIR");
	if (root == nullptr || root[0] == '\0') {
		std::printf("SKIP: OPENNOVA_JO_DIR not set (00TRa fork oracle)\n");
		return true;
	}

	opennova::Vfs vfs;
	if (!expect(vfs.mount_game(root, "revx02", opennova::VfsMountMode::Packed),
			"OPENNOVA_JO_DIR mounts for the 00TRa fork oracle")) return false;
	std::vector<uint8_t> til_bytes;
	std::vector<uint8_t> tga_bytes;
	if (!vfs.read_file("00TRA.TIL", til_bytes) ||
			!vfs.read_file("TRNTILE10.TGA", tga_bytes)) {
		std::printf("SKIP: mounted install lacks 00TRA.TIL/TRNTILE10.TGA\n");
		return true;
	}
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
	// The transform itself is independently witnessed in render_water_quad
	// and pinned synthetically by til_render_uv_test; the full-atlas bake
	// comparison above shares til_transform_local_uv and therefore proves
	// placement/channel parity, not transform independence. On that payload,
	// 0x05 -> T(x,z)=(z,x) hashes 5690c9449dabde0f and
	// 0x06 -> T(x,z)=(1-z,1-x) hashes a03140eb55c2e69d (re-verified against
	// the live compose 2026-08-23; an earlier swap to 9bd9ceb8/7c7aa42a had
	// re-broken the asset-gated leg). Alpha pins are the all-zero page hash:
	// the overlay loops run under COLORWRITEENABLE = 7 and never write A.
	return til_entry_oracle(entry_761, tilestrip, 761, fork_key,
			UINT64_C(0x5690c9449dabde0f),
			UINT64_C(0xdce53c1df8560f83)) &&
			til_entry_oracle(entry_781, tilestrip, 781, fork_key,
					UINT64_C(0xa03140eb55c2e69d),
					UINT64_C(0xdce53c1df8560f83));
}

} // namespace

int main() {
	if (!test_level_density()) return 1;
	if (!test_rejects_source_atlases_without_a_complete_quadrant()) return 1;
	if (!test_source_orientation_quadrant_clamp_and_dot3()) return 1;
	if (!test_overlay_atlas_flags_tint_clipping_and_order()) return 1;
	if (!test_optional_cp12_assets()) return 1;
	if (!test_optional_00tra_fork_oracle()) return 1;
	std::printf("OK: terrain tile page composition\n");
	return 0;
}
