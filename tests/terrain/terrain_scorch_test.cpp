// Permanent terrain scorch routing, registry (sector buckets, per-frame
// stamp, generation), mip, and page-composition oracle. These are distinct
// from world::ScarCache's entity-surface decals.
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/terrain/terrain_tile_composer.h>
#include <base/crt/crt_rng.h>
#include <runtime/world/terrain_scorch_events.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace opennova::crt;

namespace {

using opennova::terrain::Rgba8Image;

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

Rgba8Image solid(uint32_t width, uint32_t height,
		std::array<uint8_t, 4> color) {
	Rgba8Image image{width, height, {}};
	image.pixels.resize(static_cast<std::size_t>(width) * height * 4u);
	for (std::size_t offset = 0; offset < image.pixels.size(); offset += 4) {
		std::copy(color.begin(), color.end(), image.pixels.begin() + offset);
	}
	return image;
}

uint8_t byte(float value) {
	return static_cast<uint8_t>(std::clamp(
			static_cast<int>(std::lround(
					std::clamp(value, 0.0f, 1.0f) * 255.0f)),
			0, 255));
}

std::array<uint8_t, 4> pixel(const Rgba8Image &image, int x, int y) {
	const std::size_t offset = 4u *
			(static_cast<std::size_t>(y) * image.width + x);
	return {image.pixels[offset], image.pixels[offset + 1],
			image.pixels[offset + 2], image.pixels[offset + 3]};
}

opennova::TerrainTileCompositionJob job(
		opennova::TerrainTilePageKey key) {
	opennova::TerrainTileCompositionCache cache;
	cache.begin_frame(0);
	cache.begin_frame(0);
	const auto decision = cache.request({key, 0, 0, 0});
	return *decision->job;
}

bool test_crt_and_router() {
	crt_srand(1);
	if (!expect(crt_rand15() == 41 &&
			crt_rand15() == 18467 && crt_rand15() == 6334,
			"scorch RNG is the statically linked MSVC CRT stream")) return false;
	const auto small = opennova::terrain::resolve_standard_terrain_scorch(
			0x100000, -0x200000, 1, 5);
	if (!expect(small.valid && small.entry.texture_index == 2 &&
			small.entry.minimum_x_q16 == 0x0FC000 &&
			small.entry.maximum_x_q16 == 0x104000 &&
			small.entry.minimum_z_q16 == -0x204000 &&
			small.entry.maximum_z_q16 == -0x1FC000,
			"type 1 routes to a random 0.5-unit trscrch quad")) return false;
	const auto large = opennova::terrain::resolve_standard_terrain_scorch(
			0, 0, 7, 4);
	if (!expect(large.valid && large.entry.texture_index == 1 &&
			large.entry.minimum_x_q16 == -0x40000 &&
			large.entry.maximum_z_q16 == 0x40000,
			"types 2/7 route to a random 8-unit trscrch quad")) return false;
	const auto burn = opennova::terrain::resolve_standard_terrain_scorch(
			0, 0, 8, 0);
	if (!expect(burn.valid && burn.entry.texture_index == 4 &&
			burn.entry.minimum_x_q16 == -0xC0000 &&
			burn.entry.maximum_x_q16 == 0xC0000 &&
			burn.entry.minimum_z_q16 == -0x140000 &&
			burn.entry.maximum_z_q16 == 0x140000,
			"type 8 is the fixed 24x40 qburn quad")) return false;
	if (!expect(!opennova::terrain::resolve_standard_terrain_scorch(
			0, 0, 0, 0).valid,
			"type 0 does not manufacture a terrain scorch")) return false;
	const auto sized = opennova::terrain::resolve_sized_terrain_scorch(
			0x10000, 0x20000, 2, 0x30000, 3);
	return expect(sized.valid && sized.entry.texture_index == 0 &&
			sized.entry.minimum_x_q16 == -0x20000 &&
			sized.entry.maximum_z_q16 == 0x50000,
			"sized router applies its caller half extent to both axes");
}

bool test_producer_mailbox_and_capacity() {
	crt_srand(1);
	opennova::world::TerrainScorchEvents events;
	if (!expect(events.emit_standard(10 * 65536, -20 * 65536, 2, 77),
			"standard producer appends its resolved permanent scorch")) {
		return false;
	}
	if (!expect(events.pending().size() == 1 &&
			events.pending()[0].mission_bounds.texture_index == 2 &&
			events.pending()[0].mission_bounds.minimum_x_q16 == 6 * 65536 &&
			events.pending()[0].mission_bounds.maximum_z_q16 == -16 * 65536 &&
			events.pending()[0].tick == 77 &&
			events.pending()[0].source_order == 0,
			"producer records CRT choice, mission-plane bounds, tick, and order")) {
		return false;
	}
	const uint32_t state_after_random = crt_rand_state();
	if (!expect(events.emit_standard(0, 0, 8, 78) &&
			crt_rand_state() == state_after_random,
			"fixed qburn producer does not consume CRT rand")) return false;
	if (!expect(!events.emit_standard(0, 0, 0, 79) &&
			crt_rand_state() == state_after_random,
			"inert scorch ids neither append nor consume CRT rand")) return false;
	events.clear_pending();
	if (!expect(events.pending().empty() && events.record_count() == 2,
			"presentation drain does not reopen retail registry capacity")) {
		return false;
	}
	for (std::size_t index = events.record_count();
			index < opennova::terrain::kTerrainScorchCapacity; ++index) {
		if (!events.emit_standard(0, 0, 8, 80)) {
			return expect(false, "mailbox accepts all 4096 lifetime records");
		}
	}
	crt_srand(1);
	if (!expect(!events.emit_standard(0, 0, 2, 81) &&
			events.record_count() == opennova::terrain::kTerrainScorchCapacity &&
			events.rejected_count() == 1 && crt_rand_state() != 1,
			"record 4097 is rejected after consuming its random texture roll")) {
		return false;
	}
	events.reset();
	return expect(events.record_count() == 0 && events.pending().empty() &&
			events.rejected_count() == 0,
			"mission reset clears permanent scorch producer state");
}

bool test_mips_registry_and_boundary_overlap() {
	Rgba8Image base = solid(8, 8, {0, 0, 0, 255});
	for (int y = 0; y < 2; ++y) {
		for (int x = 0; x < 2; ++x) {
			const std::size_t offset = 4u *
					(static_cast<std::size_t>(y) * base.width + x);
			base.pixels[offset] = static_cast<uint8_t>(1 + y * 2 + x);
		}
	}
	const auto texture =
			opennova::terrain::build_terrain_scorch_texture(base);
	if (!expect(texture.is_valid() && texture.mips.size() == 2 &&
			texture.mips[0].width == 8 && texture.mips[1].width == 4,
			"box mip chain stops at the retail 4x4 terminal level")) return false;
	if (!expect(texture.mips[1].pixels[0] == 2,
			"D3DX box stand-in truncates the 1+2+3+4 average")) return false;

	opennova::terrain::TerrainScorchRegistry registry;
	const opennova::terrain::TerrainScorchEntry boundary{
			0, 64 << 16, 8 << 16, 65 << 16, 9 << 16};
	if (!expect(registry.append(boundary), "valid scorch appends")) return false;
	const opennova::TerrainTilePageKey left{0, 0, 0, 0, 4};
	const opennova::TerrainTilePageKey right{0, 0, 64, 0, 4};
	const auto left_plan = registry.plan(left);
	const auto right_plan = registry.plan(right);
	if (!expect(left_plan.valid && left_plan.entries.size() == 1 &&
			right_plan.valid && right_plan.entries.size() == 1,
			"inclusive retail overlap invalidates both pages at a shared edge")) {
		return false;
	}
	registry.clear();
	for (std::size_t index = 0;
			index < opennova::terrain::kTerrainScorchCapacity; ++index) {
		if (!registry.append({0, static_cast<int32_t>(index), 0,
				static_cast<int32_t>(index + 1), 1})) {
			return expect(false, "all 4096 retail scorch records append");
		}
	}
	return expect(registry.full() &&
			!registry.append({0, 0, 0, 1, 1}),
			"the append-only retail list rejects record 4097 without eviction");
}

bool same_entry(const opennova::terrain::TerrainScorchEntry &left,
		const opennova::terrain::TerrainScorchEntry &right) {
	return left.texture_index == right.texture_index &&
			left.minimum_x_q16 == right.minimum_x_q16 &&
			left.minimum_z_q16 == right.minimum_z_q16 &&
			left.maximum_x_q16 == right.maximum_x_q16 &&
			left.maximum_z_q16 == right.maximum_z_q16;
}

// The plan walk: the records bucketed into the sectors the page touches,
// merged in insertion order without duplicates, inclusive at sector edges.
bool test_registry_buckets_and_generation() {
	opennova::terrain::TerrainScorchRegistry registry;
	const uint64_t fresh = registry.generation();
	if (!expect(!registry.append({0, 5 << 16, 5 << 16, 5 << 16, 6 << 16}) &&
			registry.generation() == fresh,
			"a rejected record does not advance the registry generation")) {
		return false;
	}
	// The page [448,512] x [0,64] in sector 0 touches sector cells 0 and 1
	// along x (inclusive edge at 512).
	const opennova::TerrainTilePageKey edge_page{0, 0, 448, 0, 4};
	const opennova::terrain::TerrainScorchEntry straddles{
			0, 500 << 16, 8 << 16, 520 << 16, 9 << 16};
	const opennova::terrain::TerrainScorchEntry inside{
			1, 450 << 16, 8 << 16, 460 << 16, 9 << 16};
	const opennova::terrain::TerrainScorchEntry on_edge{
			2, 512 << 16, 8 << 16, 530 << 16, 9 << 16};
	const opennova::terrain::TerrainScorchEntry next_sector{
			4, 600 << 16, 8 << 16, 610 << 16, 9 << 16};
	const opennova::terrain::TerrainScorchEntry ends_on_edge{
			1, 400 << 16, 8 << 16, 448 << 16, 9 << 16};
	for (const auto &entry : {straddles, inside, on_edge, next_sector,
			ends_on_edge}) {
		if (!registry.append(entry)) {
			return expect(false, "bucket test records append");
		}
	}
	if (!expect(registry.generation() == fresh + 5,
			"every accepted append advances the generation by one")) {
		return false;
	}
	const auto plan = registry.plan(edge_page);
	if (!expect(plan.valid && plan.entries.size() == 4,
			"the page sees every inclusive overlap across both sector cells")) {
		return false;
	}
	if (!expect(same_entry(plan.entries[0], straddles) &&
			same_entry(plan.entries[1], inside) &&
			same_entry(plan.entries[2], on_edge) &&
			same_entry(plan.entries[3], ends_on_edge),
			"the merged bucket walk keeps retail insertion order, once each")) {
		return false;
	}
	// A page fully inside sector 1 sees only its own overlaps, in order.
	const opennova::TerrainTilePageKey sector_one_page{512, 0, 0, 0, 3};
	const auto sector_one = registry.plan(sector_one_page);
	if (!expect(sector_one.valid && sector_one.entries.size() == 3 &&
			same_entry(sector_one.entries[0], straddles) &&
			same_entry(sector_one.entries[1], on_edge) &&
			same_entry(sector_one.entries[2], next_sector),
			"a neighbouring sector page walks only its bucketed records")) {
		return false;
	}
	// A later append that touches the page joins its plan in order; one
	// that does not leaves it alone.
	if (!registry.append({0, 700 << 16, 8 << 16, 710 << 16, 9 << 16})) {
		return expect(false, "far record appends");
	}
	if (!expect(registry.plan(edge_page).entries.size() == 4,
			"a record outside the page leaves its plan unchanged")) {
		return false;
	}
	if (!registry.append({0, 460 << 16, 8 << 16, 470 << 16, 9 << 16})) {
		return expect(false, "near record appends");
	}
	if (!expect(registry.plan(edge_page).entries.size() == 5,
			"a record overlapping the page joins its plan")) {
		return false;
	}
	const uint64_t before_clear = registry.generation();
	registry.clear();
	const auto cleared = registry.plan(edge_page);
	if (!expect(registry.generation() == before_clear + 1 &&
			cleared.valid && cleared.entries.empty(),
			"clear advances the generation and empties every page walk")) {
		return false;
	}
	const opennova::TerrainTilePageKey unroutable{0, 0, 0, 0, 5};
	return expect(!registry.plan(unroutable).valid,
			"a page level outside 0..4 cannot be routed");
}

bool test_ordered_page_composition() {
	const auto colormap = opennova::terrain::build_terrain_tile_quadrant_source(
			solid(2, 2, {100, 120, 140, 255}));
	const auto normal = opennova::terrain::build_terrain_tile_quadrant_source(
			solid(2, 2, {160, 128, 128, 128}));
	const auto tilestrip = opennova::terrain::build_terrain_tile_set_mips(
			solid(64, 64, {200, 80, 40, 64}));
	opennova::TilFile til;
	til.entries.push_back(opennova::make_til_overlay_entry(1, 1, 0, 0));

	std::array<opennova::terrain::TerrainScorchTexture,
			opennova::terrain::kTerrainScorchTextureSlots> textures;
	for (const uint8_t index : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
		textures[index] = opennova::terrain::build_terrain_scorch_texture(
				solid(4, 4, {64, 128, 192, 17}));
	}
	opennova::terrain::TerrainScorchRegistry registry;
	if (!registry.append({0, 16 << 16, 16 << 16, 32 << 16, 32 << 16}) ||
			!registry.append({1, 16 << 16, 16 << 16, 32 << 16, 32 << 16})) {
		return expect(false, "ordered overlap test records append");
	}
	const opennova::TerrainTilePageKey key{0, 0, 0, 0, 4};
	const auto plan = registry.plan(key);
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.tile_info = &til;
	sources.tilestrip = &tilestrip;
	sources.light_bytes = {160, 128, 128};
	sources.scorch_plan = &plan;
	sources.scorch_textures = &textures;
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(
			job(key), sources);
	if (!expect(page.is_valid(), "page with permanent scorches composes")) {
		return false;
	}

	// Pixel (80,80) samples world (20,20): inside the authored 16u .til quad
	// and both later scorch quads. Reproduce the fixed-function order.
	std::array<uint8_t, 4> expected = {
			byte((100.0f / 255.0f) * (256.0f / 255.0f)),
			byte((120.0f / 255.0f) * (256.0f / 255.0f)),
			byte((140.0f / 255.0f) * (256.0f / 255.0f)), 0};
	const float overlay_alpha = 64.0f / 255.0f;
	// The atlas is DXT5 (flags 0x100203): the solid (200, 80, 40) cell reads
	// back as its 5:6:5 colour. [orig: Terrain_LoadTileSetAtlas @ 0x604B24]
	const uint8_t overlay_rgb[3] = {197, 81, 41};
	for (int channel = 0; channel < 3; ++channel) {
		expected[channel] = byte(
				(expected[channel] / 255.0f) * (1.0f - overlay_alpha) +
				(overlay_rgb[channel] / 255.0f) * overlay_alpha);
	}
	// The overlay/scorch loops run with COLORWRITEENABLE = 7: alpha stays 0.
	// [orig: PolyTrn_RenderTile SetRenderState(0xA8, 7) @ 0x60DD6B..0x60DD73]
	// The stage colour is MODULATE2X(texture, 0x808080) = texture * 256/255,
	// saturated, and DESTCOLOR/SRCCOLOR doubles it into the target.
	// [orig: PolyTrn_RenderTile diffuse 0xFF808080 @ 0x60E033..0x60E04C]
	const float scorch_rgb[3] = {64.0f / 255.0f,
			128.0f / 255.0f, 192.0f / 255.0f};
	for (int draw = 0; draw < 2; ++draw) {
		for (int channel = 0; channel < 3; ++channel) {
			const float stage = std::min(1.0f,
					scorch_rgb[channel] * (256.0f / 255.0f));
			expected[channel] = byte(
					2.0f * stage * (expected[channel] / 255.0f));
		}
	}
	float dot = 0.0f;
	for (int channel = 0; channel < 3; ++channel) {
		const float n = (channel == 0 ? 160.0f : 128.0f) / 255.0f - 0.5f;
		const float l = (channel == 0 ? 160.0f : 128.0f) / 255.0f - 0.5f;
		dot += n * l;
	}
	expected[3] = static_cast<uint8_t>(std::min(
			255, static_cast<int>(expected[3]) + byte(4.0f * dot)));
	if (!expect(pixel(page, 80, 80) == expected,
			".til -> ordered scorch multiply -> DOT3 alpha matches retail")) {
		const auto actual = pixel(page, 80, 80);
		std::fprintf(stderr,
				"  actual=(%u,%u,%u,%u) expected=(%u,%u,%u,%u)\n",
				actual[0], actual[1], actual[2], actual[3],
				expected[0], expected[1], expected[2], expected[3]);
		return false;
	}
	return expect(pixel(page, 40, 40) != expected,
			"scorch quad does not leak beyond its exact fixed-point bounds");
}

opennova::terrain::TerrainTilePageSourceView bare_sources(
		const opennova::terrain::TerrainTileQuadrantSource &colormap,
		const opennova::terrain::TerrainTileQuadrantSource &normal) {
	opennova::terrain::TerrainTilePageSourceView sources;
	sources.colormap = &colormap;
	sources.heightfield_normal = &normal;
	sources.light_bytes = {128, 128, 255};
	return sources;
}

// The record's quad puts UV (0,0) at (minimum X, maximum Z) and UV (1,1) at
// (maximum X, minimum Z): the texture's first row lies along the record's
// maximum-Z edge. [orig: PolyTrn_RenderTile scorch positions
// @ 0x60DFD1..0x60E02A, UV (0,0)-(1,1) @ 0x60E06B..0x60E08E]
bool test_scorch_texture_rows_run_from_maximum_z() {
	const auto colormap = opennova::terrain::build_terrain_tile_quadrant_source(
			solid(2, 2, {128, 128, 128, 255}));
	const auto normal = opennova::terrain::build_terrain_tile_quadrant_source(
			solid(2, 2, {128, 128, 255, 128}));
	Rgba8Image top_red = solid(64, 64, {0, 0, 255, 255});
	for (std::size_t offset = 0; offset < 64u * 32u * 4u; offset += 4) {
		top_red.pixels[offset] = 255;
		top_red.pixels[offset + 2] = 0;
	}
	std::array<opennova::terrain::TerrainScorchTexture,
			opennova::terrain::kTerrainScorchTextureSlots> textures;
	for (const uint8_t index : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
		textures[index] = opennova::terrain::build_terrain_scorch_texture(top_red);
	}
	opennova::terrain::TerrainScorchRegistry registry;
	// A 16x16-unit record over a 1:1-per-quarter-unit LOD-4 page: 64 texels
	// on 64 pixels, so level 0 samples.
	if (!registry.append({0, 16 << 16, 16 << 16, 32 << 16, 32 << 16})) {
		return expect(false, "orientation record appends");
	}
	const opennova::TerrainTilePageKey key{0, 0, 0, 0, 4};
	const auto plan = registry.plan(key);
	auto sources = bare_sources(colormap, normal);
	sources.scorch_plan = &plan;
	sources.scorch_textures = &textures;
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(job(key), sources);
	if (!expect(page.is_valid(), "orientation page composes")) return false;
	// Page row 70 is world z 17.5 (near minimum Z) -> texture rows near 64:
	// the blue half. Page row 122 is world z 30.5 (near maximum Z) -> the
	// red first rows.
	const auto near_minimum = pixel(page, 80, 70);
	const auto near_maximum = pixel(page, 80, 122);
	return expect(near_minimum[2] > near_minimum[0],
			"rows near the record's minimum Z take the texture's last rows") &&
			expect(near_maximum[0] > near_maximum[2],
					"rows near the record's maximum Z take the texture's first rows");
}

// Scorch textures load with flags 0 (WRAP, LINEAR, MIPFILTER POINT), so a
// record drawn at four texels per page pixel samples box level 2.
// [orig: Terrain_LoadScorchTextures @ 0x604CE0 (Texture_LoadByNameWithChannel
// flags @ 0x58B728); apply_texture_stages @ 0x68084C..0x680870]
bool test_scorch_samples_the_nearest_box_level() {
	const auto colormap = opennova::terrain::build_terrain_tile_quadrant_source(
			solid(2, 2, {255, 255, 255, 255}));
	const auto normal = opennova::terrain::build_terrain_tile_quadrant_source(
			solid(2, 2, {128, 128, 255, 128}));
	// One bright column in four: level 1 alternates 127/0 and level 2 is the
	// constant truncated 63, while level 0 would blend to 127.
	Rgba8Image stripes = solid(64, 64, {0, 0, 0, 255});
	for (uint32_t y = 0; y < 64; ++y) {
		for (uint32_t x = 0; x < 64; x += 4) {
			const std::size_t offset = 4u * (static_cast<std::size_t>(y) * 64 + x);
			stripes.pixels[offset] = 254;
			stripes.pixels[offset + 1] = 254;
			stripes.pixels[offset + 2] = 254;
		}
	}
	std::array<opennova::terrain::TerrainScorchTexture,
			opennova::terrain::kTerrainScorchTextureSlots> textures;
	for (const uint8_t index : {uint8_t{0}, uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
		textures[index] = opennova::terrain::build_terrain_scorch_texture(stripes);
	}
	opennova::terrain::TerrainScorchRegistry registry;
	// 4 world units on a LOD-4 page = 16 pixels for 64 texels.
	if (!registry.append({0, 16 << 16, 16 << 16, 20 << 16, 20 << 16})) {
		return expect(false, "level record appends");
	}
	const opennova::TerrainTilePageKey key{0, 0, 0, 0, 4};
	const auto plan = registry.plan(key);
	auto sources = bare_sources(colormap, normal);
	sources.scorch_plan = &plan;
	sources.scorch_textures = &textures;
	const Rgba8Image page = opennova::terrain::compose_terrain_tile_page(job(key), sources);
	if (!expect(page.is_valid(), "level page composes")) return false;
	const float stage = std::min(1.0f, (63.0f / 255.0f) * (256.0f / 255.0f));
	const uint8_t expected = byte(2.0f * stage * 1.0f);
	for (int x = 65; x < 80; ++x) {
		if (!expect(pixel(page, x, 70)[0] == expected,
				"every covered pixel samples the constant level-2 average")) {
			std::fprintf(stderr, "  x=%d actual=%u expected=%u\n", x,
					pixel(page, x, 70)[0], expected);
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	if (!test_crt_and_router()) return 1;
	if (!test_producer_mailbox_and_capacity()) return 1;
	if (!test_mips_registry_and_boundary_overlap()) return 1;
	if (!test_registry_buckets_and_generation()) return 1;
	if (!test_ordered_page_composition()) return 1;
	if (!test_scorch_texture_rows_run_from_maximum_z()) return 1;
	if (!test_scorch_samples_the_nearest_box_level()) return 1;
	std::printf("OK: permanent terrain scorch parity\n");
	return 0;
}
