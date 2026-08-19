// TerrainTileCompositionCache -- the portable page request/compiler seam for
// retail's composed terrain texture cache. Tests observe only request results,
// completion publications, invalidation, and spatial resident lookup.
#include <terrain/terrain_tile_composition_cache.h>

#include <array>
#include <cmath>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	using opennova::TerrainTileCompositionCache;
	using opennova::TerrainTileCompositionRequest;
	using opennova::TerrainTileContentStamp;
	using opennova::TerrainTilePageKey;

	TerrainTileCompositionCache cache;
	const std::array<int, 4> expected_spans = {512, 256, 128, 64};
	const std::array<float, 4> expected_densities = {0.5f, 1.0f, 2.0f, 4.0f};
	const std::array<int, 4> expected_tile_footprints = {8, 16, 32, 64};

	for (int i = 0; i < 4; ++i) {
		const uint8_t page_lod = static_cast<uint8_t>(i + 1);
		const TerrainTilePageKey key{0, 0, 0, 0, page_lod};
		const auto decision = cache.request(TerrainTileCompositionRequest{
				key, i, 0, 0,
				TerrainTileContentStamp{static_cast<uint64_t>(10 + i)}});
		if (!expect(decision.has_value(), "levels 1-4 are valid page requests")) return 1;
		if (!expect(decision->job.has_value(), "a cold page request emits a compose job")) return 1;
		const auto &layout = decision->job->layout;
		if (!expect(TerrainTileCompositionCache::kDimension == 256 &&
				layout.texture_dimension == 256,
				"active-quality cache pages are 256 texels square")) return 1;
		if (!expect(TerrainTileCompositionCache::page_world_span(page_lod) ==
					expected_spans[i] && layout.world_span == expected_spans[i],
				"page span follows 1024 >> page_lod")) return 1;
		if (!expect(std::fabs(layout.texels_per_world_unit - expected_densities[i]) < 1e-6f,
				"page texel density follows the retail page level")) return 1;
		if (!expect(layout.texel_footprint(16) == expected_tile_footprints[i],
				"a 16-unit .til entry keeps its level-dependent footprint")) return 1;
	}

	// A repeated source page at two routed world-sector origins is two cache
	// identities. The compose job retains the exact atlas source selected by
	// the terrain compiler for the renderer to draw.
	const TerrainTilePageKey first_sector{1024, -512, 64, 128, 4};
	const auto first = cache.request(TerrainTileCompositionRequest{
			first_sector, 73, 576, 640, TerrainTileContentStamp{50}});
	if (!expect(first.has_value() && first->job.has_value(),
			"first routed sector emits a compose job")) return 1;
	if (!expect(first->job->tile_index == 73 &&
				first->job->source_origin_x == 576 &&
				first->job->source_origin_z == 640,
			"compose job carries the selected tile and exact atlas origin")) return 1;

	TerrainTilePageKey second_sector = first_sector;
	second_sector.sector_origin_x += 512;
	const auto second = cache.request(TerrainTileCompositionRequest{
			second_sector, 73, 576, 640, TerrainTileContentStamp{50}});
	if (!expect(second.has_value() && second->job.has_value(),
			"the repeated source page in another sector is a cache miss")) return 1;
	if (!expect(second->binding.layer != first->binding.layer,
			"distinct routed sector pages receive distinct resident layers")) return 1;

	// Publication is the renderer handoff: until the compose job completes the
	// layer is not sampleable. A repeated identical request after publication
	// is a ready hit and emits no duplicate work.
	TerrainTileCompositionCache hit_cache;
	const TerrainTileCompositionRequest hit_request{
			TerrainTilePageKey{0, 0, 128, 64, 4},
			11, 128, 64, TerrainTileContentStamp{0xA55A}};
	const auto cold = hit_cache.request(hit_request);
	if (!expect(cold.has_value() && cold->job.has_value() &&
				!cold->binding.ready && cold->binding.generation > 0,
			"cold request reserves a generation but is not ready")) return 1;
	if (!expect(hit_cache.can_publish(*cold->job),
			"the current pending generation may be validated before upload")) return 1;
	if (!expect(hit_cache.publish(*cold->job),
			"publishing the current compose job succeeds")) return 1;
	const auto hit = hit_cache.request(hit_request);
	if (!expect(hit.has_value() && !hit->job.has_value() && hit->binding.ready,
			"same key, source, and content stamp is a no-job ready hit")) return 1;
	if (!expect(hit->binding.layer == cold->binding.layer &&
				hit->binding.generation == cold->binding.generation,
			"ready hit preserves its layer generation")) return 1;

	// The fixed pool is strict LRU. Refreshing the first of 128 ready pages
	// protects it, so the 129th distinct request reuses the second page's
	// layer. Its old job token cannot publish into the reused generation.
	TerrainTileCompositionCache lru_cache;
	std::array<TerrainTileCompositionRequest,
			TerrainTileCompositionCache::kCapacity> resident_requests{};
	std::optional<opennova::TerrainTileCompositionJob> stale_second_job;
	uint16_t second_layer = 0;
	for (int i = 0; i < TerrainTileCompositionCache::kCapacity; ++i) {
		resident_requests[i] = TerrainTileCompositionRequest{
				TerrainTilePageKey{i * 512, 0, 0, 0, 1},
				i, 0, 0, TerrainTileContentStamp{static_cast<uint64_t>(1000 + i)}};
		const auto inserted = lru_cache.request(resident_requests[i]);
		if (!expect(inserted.has_value() && inserted->job.has_value(),
				"each of the first 128 pages gets a compose job")) return 1;
		if (i == 1) {
			stale_second_job = inserted->job;
			second_layer = inserted->binding.layer;
		}
		if (!expect(lru_cache.publish(*inserted->job),
				"each initial cache page publishes")) return 1;
	}
	const auto refreshed = lru_cache.request(resident_requests[0]);
	if (!expect(refreshed.has_value() && !refreshed->job.has_value() &&
				refreshed->binding.ready,
			"an exact ready hit refreshes LRU age")) return 1;

	const TerrainTileCompositionRequest page_129{
			TerrainTilePageKey{TerrainTileCompositionCache::kCapacity * 512,
					0, 0, 0, 1},
			999, 0, 0, TerrainTileContentStamp{9999}};
	const auto evicting = lru_cache.request(page_129);
	if (!expect(evicting.has_value() && evicting->job.has_value() &&
				evicting->binding.layer == second_layer,
			"the 129th page evicts the strict least-recently-used layer")) return 1;
	if (!expect(stale_second_job.has_value() &&
				!lru_cache.can_publish(*stale_second_job) &&
				!lru_cache.publish(*stale_second_job),
			"an evicted generation cannot publish into its reused layer")) return 1;
	const auto first_survives = lru_cache.request(resident_requests[0]);
	if (!expect(first_survives.has_value() && !first_survives->job.has_value(),
			"the refreshed oldest insertion remains resident")) return 1;
	const auto second_misses = lru_cache.request(resident_requests[1]);
	if (!expect(second_misses.has_value() && second_misses->job.has_value(),
			"the unrefreshed second insertion was evicted")) return 1;

	// The renderer records layer indices into a deferred draw list. Once a
	// binding has been returned in a frame, that layer must not be recycled
	// underneath an earlier draw before submission. All 128 unique bindings
	// fit; the 129th miss fails closed until the next frame releases pins.
	TerrainTileCompositionCache frame_cache;
	frame_cache.begin_frame(700);
	std::array<TerrainTileCompositionRequest,
			TerrainTileCompositionCache::kCapacity> frame_requests{};
	for (int i = 0; i < TerrainTileCompositionCache::kCapacity; ++i) {
		frame_requests[i] = TerrainTileCompositionRequest{
				TerrainTilePageKey{i * 512, 512, 0, 0, 1},
				i, 0, 0,
				TerrainTileContentStamp{static_cast<uint64_t>(2000 + i)}};
		const auto inserted = frame_cache.request(frame_requests[i]);
		if (!expect(inserted && inserted->job &&
				frame_cache.publish(*inserted->job),
				"each of 128 unique pages can bind and publish in one frame")) return 1;
	}
	const TerrainTileCompositionRequest frame_page_129{
			TerrainTilePageKey{TerrainTileCompositionCache::kCapacity * 512,
					512, 0, 0, 1},
			999, 0, 0, TerrainTileContentStamp{9999}};
	const auto pinned_miss = frame_cache.request(frame_page_129);
	if (!expect(!pinned_miss.has_value(),
			"the 129th unique page fails closed instead of overwriting a layer used this frame")) return 1;
	frame_cache.begin_frame(700);
	if (!expect(!frame_cache.request(frame_page_129).has_value(),
			"repeating the same frame id preserves existing layer pins")) return 1;
	frame_cache.begin_frame(701);
	const auto next_frame = frame_cache.request(frame_page_129);
	if (!expect(next_frame && next_frame->job &&
			next_frame->binding.layer == 0,
			"a new frame releases pins and evicts the strict least-recently-used layer")) return 1;
	const auto revisit_first = frame_cache.request(frame_requests[0]);
	if (!expect(revisit_first && revisit_first->job &&
			revisit_first->binding.layer == 1 &&
			revisit_first->binding.layer != next_frame->binding.layer,
			"next-frame replacement stays pinned while strict LRU chooses an unpinned layer")) return 1;

	// Existing ready bindings are just as dangerous to recycle as cold ones.
	// Terrain first selects the pages for this frame through request(); the
	// shared spatial lookup can then hand one of those bindings to foliage.
	TerrainTileCompositionCache lookup_pin_cache;
	for (int i = 0; i < TerrainTileCompositionCache::kCapacity; ++i) {
		const auto inserted = lookup_pin_cache.request(frame_requests[i]);
		if (!expect(inserted && inserted->job &&
				lookup_pin_cache.publish(*inserted->job),
				"lookup-pin fixture fills the resident cache")) return 1;
	}
	lookup_pin_cache.begin_frame(800);
	const auto exact_pin = lookup_pin_cache.request(frame_requests[0]);
	const auto spatial_candidate = lookup_pin_cache.request(frame_requests[1]);
	const auto spatial_pin = lookup_pin_cache.best_ready(
			opennova::TerrainTileResidentPoint{
					frame_requests[1].page.sector_origin_x,
					frame_requests[1].page.sector_origin_z,
					static_cast<float>(frame_requests[1].page.sector_origin_x + 1),
					static_cast<float>(frame_requests[1].page.sector_origin_z + 1)});
	if (!expect(exact_pin && exact_pin->binding.ready &&
			spatial_candidate && spatial_candidate->binding.ready && spatial_pin &&
			spatial_pin->layer == 1,
			"spatial lookup returns a terrain-selected current-frame binding")) return 1;
	const auto after_lookup_pins = lookup_pin_cache.request(frame_page_129);
	if (!expect(after_lookup_pins && after_lookup_pins->job &&
			after_lookup_pins->binding.layer == 2,
			"strict LRU skips layers selected for deferred terrain/foliage draws")) return 1;

	// The opaque content stamp collects renderer-owned contributors without
	// teaching the cache what they are. A changed stamp or selected source
	// recompiles the existing page layer; explicit invalidation does the same
	// when a contributor cannot be represented by the caller's stamp alone.
	TerrainTileCompositionCache dirty_cache;
	TerrainTileCompositionRequest dirty_request{
			TerrainTilePageKey{0, 512, 256, 0, 2},
			44, 768, 0, TerrainTileContentStamp{100}};
	const auto original = dirty_cache.request(dirty_request);
	if (!expect(original.has_value() && original->job.has_value() &&
				dirty_cache.publish(*original->job),
			"baseline contributor set publishes")) return 1;

	dirty_request.content = TerrainTileContentStamp{101};
	const auto changed_content = dirty_cache.request(dirty_request);
	if (!expect(changed_content.has_value() && changed_content->job.has_value() &&
				changed_content->binding.layer == original->binding.layer &&
				changed_content->binding.generation > original->binding.generation,
			"changed contributor stamp recompiles the same resident layer")) return 1;
	if (!expect(changed_content->binding.ready && changed_content->binding.stale,
			"a published layer keeps serving, marked stale, while the replacement composes")) return 1;
	const auto repeat_during_recompose = dirty_cache.request(dirty_request);
	if (!expect(repeat_during_recompose.has_value() &&
				!repeat_during_recompose->job.has_value() &&
				repeat_during_recompose->binding.ready &&
				repeat_during_recompose->binding.stale,
			"a repeated request during recompose is a stale hit without duplicate work")) return 1;
	if (!expect(dirty_cache.publish(*changed_content->job),
			"changed contributor generation publishes")) return 1;
	const auto republished = dirty_cache.request(dirty_request);
	if (!expect(republished.has_value() && !republished->job.has_value() &&
				republished->binding.ready && !republished->binding.stale,
			"publication clears the stale mark")) return 1;

	dirty_request.tile_index = 45;
	dirty_request.source_origin_x = 512;
	const auto changed_source = dirty_cache.request(dirty_request);
	if (!expect(changed_source.has_value() && changed_source->job.has_value() &&
				changed_source->binding.layer == original->binding.layer,
			"changed selected terrain source recompiles the page")) return 1;
	if (!expect(changed_source->binding.ready && changed_source->binding.stale,
			"a changed selected source also stale-serves the published payload")) return 1;
	if (!expect(dirty_cache.invalidate(dirty_request.page),
			"an occupied page can be explicitly invalidated")) return 1;
	if (!expect(!dirty_cache.can_publish(*changed_source->job) &&
			!dirty_cache.publish(*changed_source->job),
			"explicit invalidation rejects an outstanding stale job")) return 1;
	const auto dirtied = dirty_cache.request(dirty_request);
	if (!expect(dirtied.has_value() && dirtied->job.has_value() &&
				dirtied->binding.layer == original->binding.layer &&
				dirtied->binding.generation > changed_source->binding.generation,
			"an explicitly dirty page emits fresh work on its next request")) return 1;
	if (!expect(!dirtied->binding.ready && !dirtied->binding.stale,
			"explicit invalidation drops the payload instead of stale-serving it")) return 1;
	if (!expect(!dirty_cache.invalidate(
				TerrainTilePageKey{999, 999, 0, 0, 1}),
			"invalidating a nonresident page reports no change")) return 1;

	// Terrain and foliage ask the same spatial question. The cache chooses the
	// finest ready page containing the point in that routed sector, falling
	// back to a coarser ready page while finer work is dirty or pending.
	TerrainTileCompositionCache spatial_cache;
	const TerrainTileCompositionRequest coarse_request{
			TerrainTilePageKey{1000, 2000, 0, 0, 1},
			1, 0, 0, TerrainTileContentStamp{1}};
	const TerrainTileCompositionRequest middle_request{
			TerrainTilePageKey{1000, 2000, 256, 0, 2},
			2, 256, 0, TerrainTileContentStamp{2}};
	const TerrainTileCompositionRequest fine_request{
			TerrainTilePageKey{1000, 2000, 256, 64, 4},
			3, 256, 64, TerrainTileContentStamp{3}};
	const auto coarse = spatial_cache.request(coarse_request);
	const auto middle = spatial_cache.request(middle_request);
	const auto fine = spatial_cache.request(fine_request);
	if (!expect(coarse && coarse->job && spatial_cache.publish(*coarse->job) &&
				middle && middle->job && spatial_cache.publish(*middle->job) &&
				fine && fine->job && spatial_cache.publish(*fine->job),
			"overlapping coarse-to-fine pages publish")) return 1;

	const opennova::TerrainTileResidentPoint fine_point{
			1000, 2000, 1300.0f, 2080.0f};
	const auto best_fine = spatial_cache.best_ready(fine_point);
	if (!expect(best_fine.has_value() &&
				best_fine->page.page_lod_level == 4,
			"spatial lookup chooses the finest ready containing page")) return 1;
	// A stale-serving page remains the spatial answer while it recomposes.
	TerrainTileCompositionRequest fine_retarget = fine_request;
	fine_retarget.content = TerrainTileContentStamp{4};
	const auto fine_stale = spatial_cache.request(fine_retarget);
	if (!expect(fine_stale.has_value() && fine_stale->job.has_value() &&
				fine_stale->binding.ready && fine_stale->binding.stale,
			"a re-targeted ready page keeps serving stale")) return 1;
	const auto best_stale = spatial_cache.best_ready(fine_point);
	if (!expect(best_stale.has_value() &&
				best_stale->page.page_lod_level == 4 && best_stale->stale,
			"spatial lookup serves the stale page while its replacement composes")) return 1;
	if (!expect(spatial_cache.invalidate(fine_request.page),
			"fine page can be dirtied for fallback probe")) return 1;
	const auto fallback = spatial_cache.best_ready(fine_point);
	if (!expect(fallback.has_value() &&
				fallback->page.page_lod_level == 2,
			"dirty fine page falls back to the best coarser ready page")) return 1;
	const auto wrong_sector = spatial_cache.best_ready(
			opennova::TerrainTileResidentPoint{1512, 2000, 1300.0f, 2080.0f});
	if (!expect(!wrong_sector.has_value(),
			"spatial lookup never crosses routed sector identity")) return 1;

	// A resident fine page can outlive the traversal that selected it. Once a
	// later frame selects only a coarser page for the same ground, foliage must
	// borrow that current draw page rather than the finer prior-frame resident.
	TerrainTileCompositionCache current_frame_cache;
	const TerrainTileCompositionRequest prior_fine_request{
			TerrainTilePageKey{0, 0, 256, 64, 4},
			20, 256, 64, TerrainTileContentStamp{700}};
	const TerrainTileCompositionRequest current_coarse_request{
			TerrainTilePageKey{0, 0, 0, 0, 1},
			21, 0, 0, TerrainTileContentStamp{700}};
	current_frame_cache.begin_frame(900);
	const auto prior_fine = current_frame_cache.request(prior_fine_request);
	if (!expect(prior_fine && prior_fine->job &&
			current_frame_cache.publish(*prior_fine->job),
			"prior frame publishes a fine resident page")) return 1;
	current_frame_cache.begin_frame(901);
	const auto current_coarse = current_frame_cache.request(current_coarse_request);
	if (!expect(current_coarse && current_coarse->job &&
			current_frame_cache.publish(*current_coarse->job),
			"current frame publishes its selected coarse page")) return 1;
	const auto current_page = current_frame_cache.best_ready(
			opennova::TerrainTileResidentPoint{0, 0, 300.0f, 80.0f});
	if (!expect(current_page &&
			current_page->layer == current_coarse->binding.layer &&
			current_page->page.page_lod_level == 1,
			"spatial lookup borrows the current frame's page, not a finer stale resident")) {
		return 1;
	}

	// Content changes make the same cross-frame leak observable in lighting and
	// projected-shadow alpha: an unrequested fine page still carries its old
	// content generation while the current coarse page has the fresh stamp.
	TerrainTileCompositionCache current_content_cache;
	current_content_cache.begin_frame(910);
	const auto stale_content_fine = current_content_cache.request(
			TerrainTileCompositionRequest{
					prior_fine_request.page, prior_fine_request.tile_index,
					prior_fine_request.source_origin_x,
					prior_fine_request.source_origin_z,
					TerrainTileContentStamp{800}});
	if (!expect(stale_content_fine && stale_content_fine->job &&
			current_content_cache.publish(*stale_content_fine->job),
			"prior frame publishes the old-content fine page")) return 1;
	current_content_cache.begin_frame(911);
	const TerrainTileCompositionRequest fresh_coarse_request{
			current_coarse_request.page, current_coarse_request.tile_index,
			current_coarse_request.source_origin_x,
			current_coarse_request.source_origin_z,
			TerrainTileContentStamp{801}};
	const auto fresh_coarse = current_content_cache.request(fresh_coarse_request);
	if (!expect(fresh_coarse && fresh_coarse->job &&
			current_content_cache.publish(*fresh_coarse->job),
			"current frame publishes the fresh-content coarse page")) return 1;
	const auto fresh_page = current_content_cache.best_ready(
			opennova::TerrainTileResidentPoint{0, 0, 300.0f, 80.0f});
	if (!expect(fresh_page &&
			fresh_page->layer == fresh_coarse->binding.layer &&
			fresh_page->page.page_lod_level == 1,
			"spatial lookup cannot leak stale lighting/shadow content from an old fine page")) {
		return 1;
	}

	// Frame ids are caller-owned tokens, not a promised monotonic sequence.
	// Returning to an older numeric id starts a new logical frame and must not
	// revive the pages that happened to be selected the last time it was used.
	TerrainTileCompositionCache reused_frame_id_cache;
	reused_frame_id_cache.begin_frame(42);
	const auto first_42 = reused_frame_id_cache.request(prior_fine_request);
	if (!expect(first_42 && first_42->job &&
			reused_frame_id_cache.publish(*first_42->job),
			"first logical frame 42 publishes a fine page")) return 1;
	reused_frame_id_cache.begin_frame(43);
	reused_frame_id_cache.begin_frame(42);
	const auto revived_old_42 = reused_frame_id_cache.best_ready(
			opennova::TerrainTileResidentPoint{0, 0, 300.0f, 80.0f});
	if (!expect(!revived_old_42.has_value(),
			"reusing a numeric frame id does not revive its older logical-frame selection")) {
		return 1;
	}

	// Page rectangles are half-open. At a shared edge the point belongs to the
	// page beginning there, regardless of which adjacent page was touched
	// most recently; this keeps terrain and foliage sampling unambiguous.
	TerrainTileCompositionCache edge_cache;
	const TerrainTileCompositionRequest edge_left{
			TerrainTilePageKey{0, 0, 0, 0, 2},
			4, 0, 0, TerrainTileContentStamp{4}};
	const TerrainTileCompositionRequest edge_right{
			TerrainTilePageKey{0, 0, 256, 0, 2},
			5, 256, 0, TerrainTileContentStamp{5}};
	const auto left = edge_cache.request(edge_left);
	const auto right = edge_cache.request(edge_right);
	if (!expect(left && left->job && edge_cache.publish(*left->job) &&
				right && right->job && edge_cache.publish(*right->job),
			"adjacent same-level pages publish")) return 1;
	const opennova::TerrainTileResidentPoint shared_edge{0, 0, 256.0f, 128.0f};
	const auto boundary_owner = edge_cache.best_ready(shared_edge);
	if (!expect(boundary_owner && boundary_owner->layer == right->binding.layer,
			"shared edge belongs to the page whose half-open rect begins there")) return 1;
	const auto refresh_left = edge_cache.request(edge_left);
	if (!expect(refresh_left && !refresh_left->job,
			"exact request refreshes the older edge page")) return 1;
	const auto refreshed_edge = edge_cache.best_ready(shared_edge);
	if (!expect(refreshed_edge && refreshed_edge->layer == right->binding.layer,
			"refreshing the page ending at the edge cannot steal its neighbor's point")) return 1;

	edge_cache.invalidate_all();
	if (!expect(!edge_cache.best_ready(shared_edge).has_value(),
			"bulk invalidation removes every page from ready lookup")) return 1;
	const auto after_bulk_dirty = edge_cache.request(edge_left);
	if (!expect(after_bulk_dirty && after_bulk_dirty->job,
			"bulk-invalidated page recompiles on its next exact request")) return 1;

	// Mission/device replacement is a hard cache reset, including same-numbered
	// frame pins, but must never let a pre-reset composition job publish into a
	// newly allocated layer with an aliased generation.
	TerrainTileCompositionCache reset_cache;
	reset_cache.begin_frame(42);
	const auto before_reset = reset_cache.request(edge_left);
	if (!expect(before_reset && before_reset->job,
			"reset fixture reserves a pending page")) return 1;
	reset_cache.clear();
	reset_cache.begin_frame(42);
	const auto after_reset = reset_cache.request(edge_right);
	if (!expect(after_reset && after_reset->job &&
			after_reset->binding.layer == 0 &&
			after_reset->binding.generation != before_reset->binding.generation,
			"hard reset releases frame pins and leases layer zero with fresh identity")) return 1;
	if (!expect(!reset_cache.can_publish(*before_reset->job) &&
			reset_cache.can_publish(*after_reset->job) &&
			!reset_cache.publish(*before_reset->job) &&
			reset_cache.publish(*after_reset->job),
			"hard reset rejects stale work but accepts the new layer generation")) return 1;

	// CPU page work follows current view demand rather than the historical miss
	// order. A newer generation for one leased layer replaces its queued work;
	// newest demand frames run first, with FIFO retained inside one frame.
	opennova::TerrainTileCompositionDemandQueue demand_queue;
	if (!expect(demand_queue.enqueue({0, 1, 10, 1}, 4).accepted &&
			demand_queue.enqueue({1, 1, 10, 2}, 4).accepted &&
			demand_queue.enqueue({2, 1, 11, 3}, 4).accepted &&
			demand_queue.enqueue({3, 1, 11, 4}, 4).accepted &&
			demand_queue.size() == 4,
			"rapid view demand fills but never exceeds the bounded worker queue")) {
		return 1;
	}
	const auto superseded = demand_queue.enqueue({0, 2, 12, 5}, 4);
	if (!expect(superseded.accepted &&
			superseded.removed_sequences.size() == 1 &&
			superseded.removed_sequences.front() == 1 &&
			demand_queue.size() == 4,
			"the current layer generation coalesces its older queued work even at capacity")) {
		return 1;
	}
	const auto stale_demand = demand_queue.enqueue({3, 0, 13, 6}, 4);
	if (!expect(!stale_demand.accepted && stale_demand.rejected_stale &&
			demand_queue.size() == 4,
			"an older generation cannot displace current queued work")) return 1;
	const auto over_capacity = demand_queue.enqueue({4, 1, 13, 7}, 4);
	if (!expect(!over_capacity.accepted && !over_capacity.rejected_stale &&
			demand_queue.size() == 4,
			"unrelated demand cannot grow the bounded queue past capacity")) return 1;
	const auto newest = demand_queue.take_next();
	const auto same_frame_first = demand_queue.take_next();
	const auto same_frame_second = demand_queue.take_next();
	const auto oldest = demand_queue.take_next();
	if (!expect(newest && newest->sequence == 5 &&
			same_frame_first && same_frame_first->sequence == 3 &&
			same_frame_second && same_frame_second->sequence == 4 &&
			oldest && oldest->sequence == 2 && demand_queue.empty(),
			"workers choose newest demand frame first and preserve FIFO within that frame")) {
		return 1;
	}

	// Completed CPU pages use the same ordering before the two-per-frame upload
	// gate. Superseding a cache layer removes an already-finished old generation,
	// while unrelated current-frame results still publish ahead of old-view output.
	opennova::TerrainTileCompositionDemandQueue completion_queue;
	if (!expect(completion_queue.enqueue({0, 1, 20, 20}, 4).accepted &&
			completion_queue.enqueue({1, 1, 20, 21}, 4).accepted &&
			completion_queue.enqueue({2, 1, 21, 22}, 4).accepted,
			"completed page demand remains bounded by the shared policy")) return 1;
	const auto removed_completion =
			completion_queue.remove_older_generations(0, 2);
	if (!expect(removed_completion.size() == 1 &&
			removed_completion.front() == 20 && completion_queue.size() == 2,
			"a replacement generation purges its already-completed stale payload")) {
		return 1;
	}
	if (!expect(completion_queue.enqueue({0, 2, 22, 23}, 4).accepted,
			"the current generation can occupy the released completion slot")) return 1;
	const auto current_completion = completion_queue.take_next();
	const auto prior_frame_completion = completion_queue.take_next();
	if (!expect(current_completion && current_completion->sequence == 23 &&
			prior_frame_completion && prior_frame_completion->sequence == 22,
			"current-frame completion publication cannot sit behind old-view output")) {
		return 1;
	}

	opennova::TerrainTileCompositionDemandQueue rapid_churn;
	std::array<uint64_t, 4> rapid_generations{};
	for (uint64_t frame = 1; frame <= 64; ++frame) {
		const uint16_t layer = static_cast<uint16_t>(frame % 4);
		const auto queued = rapid_churn.enqueue(
				{layer, ++rapid_generations[layer], 1000 + frame, frame}, 4);
		if (!expect(queued.accepted && rapid_churn.size() <= 4,
				"rapid page/view churn stays bounded while each layer coalesces")) {
			return 1;
		}
	}
	const auto latest_churn = rapid_churn.take_next();
	if (!expect(latest_churn && latest_churn->sequence == 64,
			"the latest view wins immediately after sustained page churn")) return 1;

	TerrainTileCompositionCache churn_cache;
	opennova::TerrainTileCompositionDemandQueue churn_demand;
	TerrainTileCompositionRequest churn_request{
			TerrainTilePageKey{0, 0, 0, 0, 4},
			1, 0, 0, TerrainTileContentStamp{100}};
	churn_cache.begin_frame(100);
	const auto old_view_job = churn_cache.request(churn_request);
	if (!expect(old_view_job && old_view_job->job &&
			churn_demand.enqueue({old_view_job->binding.layer,
					old_view_job->binding.generation, 100, 10}, 4).accepted,
			"old-view work enters the bounded demand queue")) return 1;
	churn_cache.begin_frame(101);
	churn_request.content = TerrainTileContentStamp{101};
	const auto current_view_job = churn_cache.request(churn_request);
	if (!expect(current_view_job && current_view_job->job,
			"current view reserves a replacement generation")) return 1;
	const auto current_enqueue = churn_demand.enqueue(
			{current_view_job->binding.layer,
					current_view_job->binding.generation, 101, 11}, 4);
	const auto current_demand = churn_demand.take_next();
	if (!expect(current_enqueue.accepted &&
			current_enqueue.removed_sequences.size() == 1 &&
			current_demand && current_demand->sequence == 11 &&
			!churn_cache.can_publish(*old_view_job->job) &&
			churn_cache.can_publish(*current_view_job->job) &&
			churn_cache.publish(*current_view_job->job),
			"rapid same-layer churn publishes current-frame quality before superseded work")) {
		return 1;
	}

	std::printf("OK: terrain tile composition cache\n");
	return 0;
}
