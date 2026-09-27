// TerrainTileCompositionCache -- the portable port of retail's 128-record
// composed terrain page cache: page layout and projection, the claim of the
// least recently used record (never one used this frame or the last), the
// per-frame sweep with its time-of-day refresh, the exact-identity bind, the
// spatial lookup, publication safety, and the spatial invalidations.
#include <runtime/terrain/terrain_tile_composition_cache.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using opennova::TerrainTileCompositionCache;
using opennova::TerrainTileCompositionJob;
using opennova::TerrainTileCompositionRequest;
using opennova::TerrainTilePageKey;
using opennova::TerrainTileResidentPoint;

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

TerrainTileCompositionRequest page_request(int32_t sector_x, int32_t local_x,
		int32_t local_z, uint8_t lod, int32_t sector_z = 0) {
	TerrainTileCompositionRequest request;
	request.page = TerrainTilePageKey{sector_x, sector_z, local_x, local_z, lod};
	request.tile_index = 0;
	request.source_origin_x = local_x;
	request.source_origin_z = local_z;
	return request;
}

// Claims and publishes one page, as the device does inside one frame.
bool compose(TerrainTileCompositionCache &cache,
		const TerrainTileCompositionRequest &request, uint16_t *layer = nullptr) {
	const auto decision = cache.request(request);
	if (!decision.has_value() || !decision->job.has_value()) return false;
	if (layer != nullptr) *layer = decision->job->target.layer;
	return cache.publish(*decision->job);
}

bool test_layout_and_projection() {
	TerrainTileCompositionCache cache;
	cache.begin_frame(0);
	cache.begin_frame(0);
	const std::array<int, 4> expected_spans = {512, 256, 128, 64};
	const std::array<float, 4> expected_densities = {0.5f, 1.0f, 2.0f, 4.0f};
	const std::array<int, 4> expected_tile_footprints = {8, 16, 32, 64};
	for (int i = 0; i < 4; ++i) {
		const uint8_t page_lod = static_cast<uint8_t>(i + 1);
		const auto decision = cache.request(page_request(0, 0, 0, page_lod));
		if (!expect(decision.has_value() && decision->job.has_value(),
				"a cold page request claims a record")) return false;
		const auto &layout = decision->job->layout;
		if (!expect(TerrainTileCompositionCache::kDimension == 256 &&
				layout.texture_dimension == 256,
				"active-quality cache pages are 256 texels square")) return false;
		if (!expect(TerrainTileCompositionCache::page_world_span(page_lod) ==
					expected_spans[i] && layout.world_span == expected_spans[i],
				"page span follows 1024 >> page_lod")) return false;
		if (!expect(std::fabs(layout.texels_per_world_unit - expected_densities[i]) < 1e-6f,
				"page texel density follows the retail page level")) return false;
		if (!expect(layout.texel_footprint(16) == expected_tile_footprints[i],
				"a 16-unit .til entry keeps its level-dependent footprint")) return false;
	}
	if (!expect(!cache.request(page_request(0, 0, 0, 5)).has_value(),
			"a level outside 0..4 is not a page")) return false;

	// The max-quality g_FoliageWindSwayVS path uploads c7/c8 from the packed
	// page record. After its D3D (Z,Y,X) model transform, those rows reduce to
	// the same presentation-world projection used by terrain, foliage,
	// MATCHTERRAIN, and the static-shadow raster.
	const TerrainTilePageKey projected_page{1024, -512, 64, 128, 4};
	const auto projection = TerrainTileCompositionCache::page_projection(
			projected_page);
	if (!expect(projection.has_value() &&
			projection->world_origin_x == 1088.0f &&
			projection->world_origin_z == -384.0f &&
			projection->inverse_world_span == 1.0f / 64.0f &&
			projection->world_span == 64.0f,
			"retail c7/c8 decode preserves routed sector and packed local origin")) {
		return false;
	}
	return expect(projection->project(1088.0f, -384.0f) ==
				std::array<float, 2>({0.0f, 0.0f}) &&
			projection->project(1104.0f, -368.0f) ==
				std::array<float, 2>({0.25f, 0.25f}) &&
			projection->project(1152.0f, -320.0f) ==
				std::array<float, 2>({1.0f, 1.0f}),
			"c7/c8 projection maps page edges and interior without an axis swap");
}

// A miss claims the record with the largest last-use age, first in record
// order on a tie, and only one whose age exceeds 1.
// [orig: PolyTrn_RenderTile @ 0x60DAE4..0x60DB45]
bool test_claim_needs_two_idle_frames() {
	TerrainTileCompositionCache cache;
	cache.begin_frame(0); // frame 1: every empty record has age 1
	if (!expect(!cache.request(page_request(0, 0, 0, 4)).has_value(),
			"the first frame claims nothing: no record is older than one frame")) {
		return false;
	}
	cache.begin_frame(0); // frame 2
	uint16_t layer = 999;
	if (!expect(compose(cache, page_request(0, 0, 0, 4), &layer) && layer == 0,
			"the second frame claims the first record")) return false;
	const auto hit = cache.request(page_request(0, 0, 0, 4));
	if (!expect(hit.has_value() && !hit->job.has_value() && hit->binding.ready &&
			hit->binding.layer == 0,
			"the same identity is a hit that composes nothing")) return false;
	if (!expect(!cache.request(page_request(0, 0, 0, 3))->binding.ready,
			"another level is another record")) return false;

	// Fill the rest of the cache in frame 2: record 129 finds nothing.
	for (int index = 2; index < TerrainTileCompositionCache::kCapacity; ++index) {
		if (!compose(cache, page_request(512 * index, 0, 0, 4))) {
			return expect(false, "all 128 records claim");
		}
	}
	if (!expect(!cache.request(page_request(-512, 0, 0, 4)).has_value(),
			"a full frame leaves the extra page uncomposed")) return false;
	cache.begin_frame(0); // frame 3: every record was used in frame 2
	if (!expect(!cache.request(page_request(-512, 0, 0, 4)).has_value(),
			"a record used in the previous frame never yields")) return false;
	// Keep record 0 in use this frame.
	(void)cache.request(page_request(0, 0, 0, 4));
	cache.begin_frame(0); // frame 4: record 0 used in frame 3, the rest in 2
	uint16_t claimed = 0;
	return expect(compose(cache, page_request(-512, 0, 0, 4), &claimed) &&
			claimed == 1,
			"the oldest record claims, first in record order among equals");
}

// A sweep that composes nothing retires one TOD-stale record, oldest compose
// first, and re-sweeps: the visible page recomposes under the current light.
// [orig: PolyTrn_RenderFrame @ 0x60F080..0x60F0E3; Terrain_EvictOldestTodStaleTile
// @ 0x604600]
bool test_sweep_refreshes_one_tod_stale_page() {
	TerrainTileCompositionCache cache;
	cache.begin_frame(7);
	cache.begin_frame(7);
	const std::vector<TerrainTileCompositionRequest> visible = {
			page_request(0, 0, 0, 4), page_request(0, 64, 0, 4)};
	std::vector<TerrainTileCompositionJob> jobs = cache.sweep(visible);
	if (!expect(jobs.size() == 2, "a cold sweep claims every visible page")) return false;
	for (const TerrainTileCompositionJob &job : jobs) cache.publish(job);
	cache.begin_frame(7);
	if (!expect(cache.sweep(visible).empty(),
			"an all-hit sweep in the same TOD epoch composes nothing")) return false;
	cache.begin_frame(8); // the TOD epoch advances
	jobs = cache.sweep(visible);
	if (!expect(jobs.size() == 1 && jobs[0].target.page.page_local_x == 0 &&
			jobs[0].target.layer == 0,
			"one stale visible page (the first composed) recomposes that frame")) {
		return false;
	}
	cache.publish(jobs[0]);
	cache.begin_frame(8);
	jobs = cache.sweep(visible);
	if (!expect(jobs.size() == 1 && jobs[0].target.page.page_local_x == 64,
			"the next all-hit frame refreshes the next stale page")) return false;
	cache.publish(jobs[0]);
	cache.begin_frame(8);
	return expect(cache.sweep(visible).empty(),
			"once every page carries the current epoch nothing recomposes");
}

// The stale-record eviction needs a compose age above 1: a page composed in
// the previous frame is never retired even though its epoch is old.
bool test_eviction_waits_one_frame() {
	TerrainTileCompositionCache cache;
	cache.begin_frame(1);
	cache.begin_frame(1);
	if (!compose(cache, page_request(0, 0, 0, 4))) return expect(false, "page composes");
	cache.begin_frame(2);
	if (!expect(!cache.evict_one_tod_stale(),
			"a page composed one frame ago is not retired")) return false;
	cache.begin_frame(2);
	return expect(cache.evict_one_tod_stale(), "two frames later it is");
}

// PolyTrn_BindStageTextures binds only an exact, composed identity.
bool test_bind() {
	TerrainTileCompositionCache cache;
	cache.begin_frame(0);
	cache.begin_frame(0);
	const TerrainTileCompositionRequest request = page_request(512, 64, 128, 4);
	const auto decision = cache.request(request);
	if (!expect(decision.has_value() && decision->job.has_value() &&
			!cache.bind(request).has_value(),
			"a claimed but uncomposed page binds nothing")) return false;
	cache.publish(*decision->job);
	const auto bound = cache.bind(request);
	TerrainTileCompositionRequest other_quadrant = request;
	other_quadrant.source_origin_x += 512;
	return expect(bound.has_value() && bound->ready &&
			bound->layer == decision->job->target.layer,
			"the composed page binds") &&
			expect(!cache.bind(other_quadrant).has_value(),
					"the packed source coordinate is part of the identity");
}

// TerrainTile_CacheLookup walks granularity 32, 64 ... 512 and takes the
// first resident record in record order whose masked coordinate matches.
// [orig: TerrainTile_CacheLookup @ 0x6041A4..0x604206]
bool test_lookup_granularity_walk() {
	TerrainTileCompositionCache cache;
	cache.begin_frame(0);
	cache.begin_frame(0);
	uint16_t coarse_layer = 0;
	uint16_t fine_layer = 0;
	if (!compose(cache, page_request(512, 0, 0, 2), &coarse_layer) ||
			!compose(cache, page_request(512, 64, 128, 4), &fine_layer)) {
		return expect(false, "lookup pages compose");
	}
	const auto fine = cache.lookup(TerrainTileResidentPoint{512.0f + 70.0f, 130.0f});
	if (!expect(fine.has_value() && fine->layer == fine_layer,
			"granularity 32 finds the fine page at the point's corner")) return false;
	const auto upper_half = cache.lookup(TerrainTileResidentPoint{512.0f + 100.0f, 130.0f});
	if (!expect(upper_half.has_value() && upper_half->layer == fine_layer,
			"granularity 64 finds it for the rest of its extent")) return false;
	// (130, 130): no record matches below granularity 256, where both the
	// coarse page's (0,0) and the fine page's masked (0,0) match; record order
	// picks the coarse page claimed first.
	const auto outside = cache.lookup(TerrainTileResidentPoint{512.0f + 130.0f, 130.0f});
	if (!expect(outside.has_value() && outside->layer == coarse_layer,
			"a coarse granularity takes the first record in order")) return false;
	// A fine page can answer for a point outside it: with no coarse page, the
	// 64u page at (64,128) answers (200, 140) at granularity 256.
	TerrainTileCompositionCache fine_only;
	fine_only.begin_frame(0);
	fine_only.begin_frame(0);
	if (!compose(fine_only, page_request(0, 64, 128, 4))) return expect(false, "fine composes");
	const auto borrowed = fine_only.lookup(TerrainTileResidentPoint{200.0f, 140.0f});
	if (!expect(borrowed.has_value() && borrowed->page.page_local_x == 64,
			"a coarse-granularity match returns a page that does not contain the point")) {
		return false;
	}
	if (!expect(!fine_only.lookup(TerrainTileResidentPoint{700.0f, 140.0f}).has_value(),
			"another sector never matches")) return false;
	// The flat page's coordinate masks to zero at its canonical sector.
	TerrainTileCompositionCache flat;
	flat.begin_frame(0);
	flat.begin_frame(0);
	if (!compose(flat, page_request(0, 0, 0, 0))) return expect(false, "flat composes");
	return expect(flat.lookup(TerrainTileResidentPoint{16.0f, 16.0f}).has_value() &&
			!flat.lookup(TerrainTileResidentPoint{528.0f, 16.0f}).has_value(),
			"the flat page answers only in its canonical (0,0) sector");
}

bool test_publication_and_invalidation() {
	TerrainTileCompositionCache cache;
	cache.begin_frame(0);
	cache.begin_frame(0);
	const TerrainTileCompositionRequest request = page_request(0, 0, 0, 4);
	const auto decision = cache.request(request);
	if (!expect(decision.has_value() && decision->job.has_value(), "page claims")) return false;
	const TerrainTileCompositionJob job = *decision->job;
	cache.invalidate_all();
	if (!expect(!cache.can_publish(job) && !cache.publish(job),
			"a reset retires an outstanding job")) return false;

	// The scorch append and the destroyed-entity walk share the inclusive
	// page test: an edge-touching rectangle retires both neighbours.
	// [orig: Terrain_AddScorchRecord @0x605CF7..0x605D5F;
	// Terrain_InvalidateTileCacheRegion @0x605C21..0x605C7F]
	cache.begin_frame(0);
	cache.begin_frame(0);
	uint16_t left_layer = 0;
	if (!compose(cache, page_request(0, 0, 0, 4), &left_layer) ||
			!compose(cache, page_request(0, 64, 0, 4)) ||
			!compose(cache, page_request(0, 256, 0, 4))) {
		return expect(false, "invalidation pages compose");
	}
	if (!expect(cache.invalidate_overlapping_q16(64 << 16, 8 << 16, 65 << 16, 9 << 16) == 2,
			"an edge rectangle retires both pages it touches")) return false;
	if (!expect(!cache.bind(page_request(0, 0, 0, 4)).has_value() &&
			!cache.bind(page_request(0, 64, 0, 4)).has_value() &&
			cache.bind(page_request(0, 256, 0, 4)).has_value(),
			"only the overlapped pages retire")) return false;
	// A retired record's last use drops to 0, so it is the first claimed.
	uint16_t reclaimed = 999;
	return expect(compose(cache, page_request(0, 128, 0, 4), &reclaimed) &&
			reclaimed == left_layer,
			"the retired record is claimed first, in record order");
}

} // namespace

int main() {
	if (!test_layout_and_projection()) return 1;
	if (!test_claim_needs_two_idle_frames()) return 1;
	if (!test_sweep_refreshes_one_tod_stale_page()) return 1;
	if (!test_eviction_waits_one_frame()) return 1;
	if (!test_bind()) return 1;
	if (!test_lookup_granularity_walk()) return 1;
	if (!test_publication_and_invalidation()) return 1;
	std::printf("OK: terrain tile composition cache\n");
	return 0;
}
