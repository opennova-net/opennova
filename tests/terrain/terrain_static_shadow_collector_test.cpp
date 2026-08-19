// Static terrain-shadow collector: admission, projected page intersection,
// selected-LOD/all-ROBJ ordering, and the page content stamp.
#include <terrain/terrain_static_shadow.h>
#include <mission/placement_traits.h>

#include <cstdio>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

opennova::terrain::TerrainStaticShadowCandidate candidate(
		int bms_id, int entity_kind, uint32_t order,
		float min_x, float min_y, float min_z,
		float max_x, float max_y, float max_z,
		uint64_t geometry_key, uint16_t lod0_parts, uint16_t lod1_parts) {
	opennova::terrain::TerrainStaticShadowCandidate value;
	value.bms_id = bms_id;
	value.entity_kind = entity_kind;
	value.collector_order = order;
	value.world_bounds = {min_x, min_y, min_z, max_x, max_y, max_z};
	value.geometry.geometry_key = geometry_key;
	value.geometry.render_object_counts = {lod0_parts, lod1_parts};
	value.transform_revision = 1;
	return value;
}

} // namespace

int main() {
	using namespace opennova;
	using namespace opennova::mission;
	using namespace opennova::terrain;

	const TerrainStaticShadowRenderObjectCoverage valid_empty{};
	if (!expect(valid_empty.is_valid_empty() && valid_empty.is_complete(),
			"an authored zero-surface ROBJ is an exact no-op")) return 1;
	TerrainStaticShadowRenderObjectCoverage complete;
	complete.authored_surface_count = 2;
	complete.valid_surface_count = 2;
	if (!expect(!complete.is_valid_empty() && complete.is_complete(),
			"all authored surfaces resolving is complete geometry")) return 1;
	TerrainStaticShadowRenderObjectCoverage partial = complete;
	partial.valid_surface_count = 1;
	if (!expect(!partial.is_complete(),
			"partial surface loss cannot publish an exact silhouette")) return 1;
	TerrainStaticShadowRenderObjectCoverage malformed = complete;
	malformed.malformed_indices = true;
	if (!expect(!malformed.is_complete(),
			"malformed indices fail closed even when another surface resolves")) return 1;
	TerrainStaticShadowRenderObjectCoverage short_uv = complete;
	short_uv.missing_required_uvs = true;
	if (!expect(!short_uv.is_complete(),
			"an alpha-dependent surface with short UVs fails closed")) return 1;
	const uint16_t relative_indices[] = {0, 1, 2};
	const uint16_t absolute_indices[] = {5, 6, 7};
	const uint16_t malformed_indices[] = {0, 1, 3};
	if (!expect(terrain_static_shadow_strip_indices_are_valid(
				relative_indices, 3, 0, 3, 8, 5, 3) &&
			terrain_static_shadow_strip_indices_are_valid(
				absolute_indices, 3, 0, 3, 8, 5, 3),
			"relative and absolute authored index conventions both resolve")) {
		return 1;
	}
	if (!expect(!terrain_static_shadow_strip_indices_are_valid(
				malformed_indices, 3, 0, 3, 8, 5, 3) &&
			!terrain_static_shadow_strip_indices_are_valid(
				relative_indices, 2, 0, 3, 8, 5, 3) &&
			!terrain_static_shadow_strip_indices_are_valid(
				relative_indices, 3, -1, 3, 8, 5, 3),
			"out-of-range, truncated, and negative authored index ranges fail")) {
		return 1;
	}

	// The page is [0,64)x[0,64). The first building starts outside at x=66,
	// but a surface-to-light direction of (+x,+y) sweeps its 8-unit height
	// back onto the page. This is the exact false-negative the collector's
	// projection-expanded broad phase must prevent.
	auto projected_building = candidate(40, kEntityKindBuilding, 9,
			66.0f, 0.0f, 16.0f, 70.0f, 8.0f, 20.0f, 400, 2, 3);
	auto admitted_item = candidate(58, kEntityKindItem, 2,
			8.0f, 0.0f, 8.0f, 12.0f, 4.0f, 12.0f, 580, 1, 2);
	admitted_item.item_attrib2 = kItemAttrib2StaticShadow;
	auto rejected_plain_item = candidate(59, kEntityKindItem, 1,
			4.0f, 0.0f, 4.0f, 6.0f, 2.0f, 6.0f, 590, 4, 4);
	auto rejected_no_shadow_building = candidate(41, kEntityKindBuilding, 1,
			1.0f, 0.0f, 1.0f, 3.0f, 2.0f, 3.0f, 410, 4, 4);
	rejected_no_shadow_building.entity_attrib = kEntityAttribNoShadow;
	auto off_page_building = candidate(42, kEntityKindBuilding, 3,
			200.0f, 0.0f, 200.0f, 204.0f, 2.0f, 204.0f, 420, 4, 4);

	TerrainStaticShadowCollector collector;
	collector.replace({admitted_item, rejected_plain_item,
			off_page_building, projected_building,
			rejected_no_shadow_building});
	if (!expect(collector.candidate_count() == 5 &&
			collector.admitted_count() == 3,
			"collector owns the witnessed building/item admission policy")) return 1;

	TerrainStaticShadowPageInput coarse;
	if (!expect(coarse.light_epoch ==
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 1.0f, 0.0f),
			"default static light epoch exactly matches the default world direction")) {
		return 1;
	}
	coarse.page = TerrainTilePageKey{0, 0, 0, 0, 4};
	coarse.surface_to_light = {1.0f, 1.0f, 0.0f};
	coarse.light_epoch =
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 1.0f, 1.0f);
	coarse.receiver_height = 0.0f;
	if (!expect(terrain_tile_light_epoch_from_environment_tuple(
			0.5f, 1.0f, 0.0f) == TerrainTileLightEpoch{127, 191, 255},
			"terrain light epoch preserves retail's truncated (g2,g0,g1) bytes")) {
		return 1;
	}
	if (!expect(terrain_tile_light_epoch_from_environment_tuple(
			std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f) ==
					kDefaultTerrainTileLightEpoch,
			"a non-finite getter channel fails to the neutral terrain-light byte")) {
		return 1;
	}
	const TerrainStaticShadowPageJob fine_job = collector.compile(coarse);
	if (!expect(fine_job.draws.size() == 3,
			"fine page includes projected building and item, but excludes off-page caster")) return 1;
	if (!expect(fine_job.draws[0].bms_id == 40 &&
			fine_job.draws[1].bms_id == 40 &&
			fine_job.draws[2].bms_id == 58,
			"pool-2 buildings precede pool-1 items and every selected-LOD ROBJ is emitted")) return 1;
	if (!expect(fine_job.draws[0].geometry.lod_index == 0 &&
			fine_job.draws[0].geometry.render_object_index == 0 &&
			fine_job.draws[1].geometry.render_object_index == 1 &&
			fine_job.draws[2].geometry.geometry_key == 580,
			"level 4 keeps the first LOD and carries stable geometry references")) return 1;

	coarse.page.page_lod_level = 3;
	const TerrainStaticShadowPageJob coarse_job = collector.compile(coarse);
	if (!expect(coarse_job.draws.size() == 5 &&
			coarse_job.draws[0].geometry.lod_index == 1 &&
			coarse_job.draws[2].geometry.render_object_index == 2 &&
			coarse_job.draws[3].bms_id == 58 &&
			coarse_job.draws[4].geometry.render_object_index == 1,
			"levels 1-3 use the second render LOD when present, in ROBJ order")) return 1;

	// The collector result and stamp cannot depend on insertion/hash iteration.
	TerrainStaticShadowCollector permuted;
	permuted.replace({rejected_no_shadow_building, projected_building,
			rejected_plain_item, admitted_item, off_page_building});
	const TerrainStaticShadowPageJob permuted_job = permuted.compile(coarse);
	if (!expect(permuted_job.content.value == coarse_job.content.value &&
			permuted_job.draws == coarse_job.draws,
			"stable pool/order identity makes input permutation deterministic")) return 1;

	// Content changes that affect the page must dirty its cache layer.
	coarse.surface_to_light = {0.5f, 1.0f, 0.0f};
	coarse.light_epoch =
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 1.0f, 0.5f);
	const TerrainStaticShadowPageJob relit = collector.compile(coarse);
	if (!expect(relit.content.value != coarse_job.content.value,
			"sun projection changes the page contribution stamp")) return 1;
	coarse.surface_to_light.x = std::nextafter(
			coarse.surface_to_light.x, 1.0f);
	coarse.light_epoch = terrain_tile_light_epoch_from_environment_tuple(
			0.0f, 1.0f, coarse.surface_to_light.x);
	const TerrainStaticShadowPageJob sub_byte_relit = collector.compile(coarse);
	if (!expect(sub_byte_relit.content.value == relit.content.value,
			"sub-quantum sun projection changes retain the quantized page epoch")) {
		return 1;
	}
	coarse.surface_to_light.x = 0.51f;
	coarse.light_epoch = terrain_tile_light_epoch_from_environment_tuple(
			0.0f, 1.0f, coarse.surface_to_light.x);
	const TerrainStaticShadowPageJob next_light_epoch = collector.compile(coarse);
	if (!expect(next_light_epoch.content.value != relit.content.value,
			"crossing a terrain-light byte boundary invalidates the page contribution")) {
		return 1;
	}
	TerrainTileCompositionCache light_epoch_cache;
	TerrainTileCompositionRequest light_epoch_request{
			coarse.page, 7, 0, 0, relit.content};
	const auto initial_light_epoch = light_epoch_cache.request(light_epoch_request);
	if (!expect(initial_light_epoch && initial_light_epoch->job &&
			light_epoch_cache.publish(*initial_light_epoch->job),
			"initial light epoch composes and publishes the page")) {
		return 1;
	}
	light_epoch_request.content = sub_byte_relit.content;
	const auto retained_light_epoch = light_epoch_cache.request(light_epoch_request);
	if (!expect(retained_light_epoch && retained_light_epoch->binding.ready &&
			!retained_light_epoch->job,
			"sub-quantum raw light movement is a no-job ready hit")) {
		return 1;
	}
	light_epoch_request.content = next_light_epoch.content;
	const auto crossed_light_epoch = light_epoch_cache.request(light_epoch_request);
	if (!expect(crossed_light_epoch && crossed_light_epoch->job &&
			crossed_light_epoch->binding.ready &&
			crossed_light_epoch->binding.stale,
			"crossing the light-byte boundary emits a replacement compose job "
			"while the published page keeps serving stale")) {
		return 1;
	}
	coarse.surface_to_light.x = 0.5f;
	coarse.light_epoch =
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 1.0f, 0.5f);
	auto resized = projected_building;
	resized.world_bounds.max_y += 1.0f;
	collector.replace({resized, admitted_item, off_page_building});
	const TerrainStaticShadowPageJob resized_job = collector.compile(coarse);
	if (!expect(resized_job.content.value != relit.content.value,
			"caster bounds change the page contribution stamp even before an adapter revision")) return 1;
	projected_building.transform_revision = 2;
	collector.replace({projected_building, admitted_item, off_page_building});
	const TerrainStaticShadowPageJob moved = collector.compile(coarse);
	if (!expect(moved.content.value != relit.content.value,
			"caster transform revision changes the page contribution stamp")) return 1;

	// Half-open page rectangles: a non-projected footprint beginning at x=64
	// belongs to its right-hand page and must not dirty the left one.
	auto edge = candidate(77, kEntityKindBuilding, 0,
			64.0f, 0.0f, 4.0f, 68.0f, 0.0f, 8.0f, 770, 1, 0);
	collector.replace({edge});
	coarse.page.page_lod_level = 4;
	coarse.surface_to_light = {0.0f, 1.0f, 0.0f};
	coarse.light_epoch =
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 1.0f, 0.0f);
	if (!expect(collector.compile(coarse).draws.empty(),
			"page intersection is half-open at an adjacent edge")) return 1;

	// Receiver height is page-local. A low page can receive the long projection
	// of a high caster while an otherwise-identical high page cannot; feeding a
	// mission-global minimum to both pages would over-admit the second case.
	auto high_caster = candidate(88, kEntityKindBuilding, 0,
			100.0f, 100.0f, 8.0f, 104.0f, 108.0f, 12.0f,
			880, 1, 0);
	collector.replace({high_caster});
	coarse.surface_to_light = {1.0f, 1.0f, 0.0f};
	coarse.light_epoch =
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 1.0f, 1.0f);
	coarse.receiver_height = 0.0f;
	const TerrainStaticShadowPageJob low_receiver = collector.compile(coarse);
	coarse.receiver_height = 100.0f;
	const TerrainStaticShadowPageJob high_receiver = collector.compile(coarse);
	if (!expect(low_receiver.draws.size() == 1 &&
			high_receiver.draws.empty(),
			"each page's own minimum receiver height controls its projected broad phase")) {
		return 1;
	}
	if (!expect(low_receiver.content.value != high_receiver.content.value,
			"page-local receiver height participates in the contribution stamp")) {
		return 1;
	}

	std::puts("OK: terrain static-shadow collector admission/order/intersection/stamp");
	return 0;
}
