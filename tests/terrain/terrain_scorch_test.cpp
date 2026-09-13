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
	const auto decision = cache.request({key, 0, 0, 0, {1}});
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
	if (!expect(left_plan.content_stamp == right_plan.content_stamp,
			"the same ordered page contribution has one stable content stamp")) {
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

// The per-frame stamp walk and the miss-path plan walk are one walk: the
// records bucketed into the sectors the page touches, merged in insertion
// order without duplicates, inclusive at sector edges, with the same stamp.
bool test_registry_buckets_stamp_and_generation() {
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
	const auto stamp = registry.stamp(edge_page);
	if (!expect(plan.valid && stamp.valid &&
			plan.entries.size() == 4 && stamp.entry_count == 4,
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
	if (!expect(stamp.content_stamp == plan.content_stamp,
			"the per-frame stamp equals the composed plan's content stamp")) {
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
	// Identity follows the record list: a later append that touches the page
	// changes its stamp, one that does not leaves it alone.
	if (!registry.append({0, 700 << 16, 8 << 16, 710 << 16, 9 << 16})) {
		return expect(false, "far record appends");
	}
	if (!expect(registry.stamp(edge_page).content_stamp ==
			stamp.content_stamp,
			"a record outside the page leaves its stamp unchanged")) {
		return false;
	}
	if (!registry.append({0, 460 << 16, 8 << 16, 470 << 16, 9 << 16})) {
		return expect(false, "near record appends");
	}
	if (!expect(registry.stamp(edge_page).content_stamp !=
			stamp.content_stamp &&
			registry.stamp(edge_page).entry_count == 5,
			"a record overlapping the page changes its stamp")) {
		return false;
	}
	const uint64_t before_clear = registry.generation();
	registry.clear();
	const auto cleared = registry.stamp(edge_page);
	if (!expect(registry.generation() == before_clear + 1 &&
			cleared.valid && cleared.entry_count == 0 &&
			registry.plan(edge_page).entries.empty(),
			"clear advances the generation and empties every page walk")) {
		return false;
	}
	const opennova::TerrainTilePageKey unroutable{0, 0, 0, 0, 5};
	return expect(!registry.stamp(unroutable).valid &&
			!registry.plan(unroutable).valid,
			"a page level outside 0..4 cannot be routed");
}

bool test_ordered_page_composition() {
	const Rgba8Image colormap = solid(2, 2, {100, 120, 140, 255});
	const Rgba8Image normal = solid(2, 2, {160, 128, 128, 128});
	const Rgba8Image tilestrip = solid(64, 64, {200, 80, 40, 64});
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

	// Pixel (80,80) is world (20.125,20.125): inside the authored 16u .til
	// quad and both later scorch quads. Reproduce the fixed-function order.
	std::array<uint8_t, 4> expected = {
			byte((100.0f / 255.0f) * (256.0f / 255.0f)),
			byte((120.0f / 255.0f) * (256.0f / 255.0f)),
			byte((140.0f / 255.0f) * (256.0f / 255.0f)), 0};
	const float overlay_alpha = 64.0f / 255.0f;
	const uint8_t overlay_rgb[3] = {200, 80, 40};
	for (int channel = 0; channel < 3; ++channel) {
		expected[channel] = byte(
				(expected[channel] / 255.0f) * (1.0f - overlay_alpha) +
				(overlay_rgb[channel] / 255.0f) * overlay_alpha);
	}
	// The overlay/scorch loops run with COLORWRITEENABLE = 7: alpha stays 0.
	// [orig: PolyTrn_RenderTile SetRenderState(0xA8, 7) @ 0x60DD6B..0x60DD73]
	const float scorch_rgb[3] = {64.0f / 255.0f,
			128.0f / 255.0f, 192.0f / 255.0f};
	for (int draw = 0; draw < 2; ++draw) {
		for (int channel = 0; channel < 3; ++channel) {
			expected[channel] = byte(
					2.0f * scorch_rgb[channel] *
					(expected[channel] / 255.0f));
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

} // namespace

int main() {
	if (!test_crt_and_router()) return 1;
	if (!test_producer_mailbox_and_capacity()) return 1;
	if (!test_mips_registry_and_boundary_overlap()) return 1;
	if (!test_registry_buckets_stamp_and_generation()) return 1;
	if (!test_ordered_page_composition()) return 1;
	std::printf("OK: permanent terrain scorch parity\n");
	return 0;
}
