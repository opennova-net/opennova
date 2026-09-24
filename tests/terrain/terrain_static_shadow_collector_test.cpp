// Static terrain-shadow collector: admission, the sun-extended sphere/tile
// test, selected-LOD/all-ROBJ ordering, and the page content stamp.
#include <runtime/terrain/terrain_static_shadow.h>
#include <runtime/mission/placement_traits.h>

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

// A caster at Godot planar (x, z) with a model sphere of `radius` units:
// mission 16.16 x = Godot x, y = -Godot z.
opennova::terrain::TerrainStaticShadowCandidate candidate(
		int bms_id, int entity_kind, uint32_t order, float x, float z,
		float radius, uint64_t geometry_key, uint16_t lod0_parts,
		uint16_t lod1_parts) {
	opennova::terrain::TerrainStaticShadowCandidate value;
	value.bms_id = bms_id;
	value.entity_kind = entity_kind;
	value.collector_order = order;
	value.position_fixed = {static_cast<int32_t>(std::lround(x * 65536.0f)),
			static_cast<int32_t>(std::lround(-z * 65536.0f))};
	value.model_radius_fixed = static_cast<int32_t>(std::lround(radius * 65536.0f));
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

	// The level-4 page spans Godot x and z [0, 64]. Retail extends the tile
	// toward the light by t * r per axis, t = l_axis * 0.5 / max(l_vertical,
	// 0.25) over the 16.16 mission light tuple, and admits a caster whose
	// model sphere overlaps the extended rectangle (inclusive)
	// [orig: Terrain_CollectAndRenderTileModels @0x60D35D..0x60D386,
	// @0x60D465..0x60D54F]. A sun at l = (1, 1, 0) gives t_x = 0.5: a
	// building of radius 4 extends the +x edge by 2 u.
	auto extended_building = candidate(40, kEntityKindBuilding, 9,
			70.0f, 18.0f, 4.0f, 400, 2, 3);
	auto admitted_item = candidate(58, kEntityKindItem, 2,
			10.0f, 10.0f, 2.0f, 580, 1, 2);
	admitted_item.item_attrib2 = kItemAttrib2StaticShadow;
	auto rejected_plain_item = candidate(59, kEntityKindItem, 1,
			5.0f, 5.0f, 1.0f, 590, 4, 4);
	auto rejected_no_shadow_building = candidate(41, kEntityKindBuilding, 1,
			2.0f, 2.0f, 1.0f, 410, 4, 4);
	rejected_no_shadow_building.entity_attrib = kEntityAttribNoShadow;
	auto off_page_building = candidate(42, kEntityKindBuilding, 3,
			202.0f, 202.0f, 2.0f, 420, 4, 4);

	TerrainStaticShadowCollector collector;
	collector.replace({admitted_item, rejected_plain_item,
			off_page_building, extended_building,
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
			"the sun-extended tile admits the building 2 u beyond it and the "
			"item, but not the off-page caster")) return 1;
	if (!expect(fine_job.draws[0].bms_id == 40 &&
			fine_job.draws[1].bms_id == 40 &&
			fine_job.draws[2].bms_id == 58,
			"pool-2 buildings precede pool-1 items and every selected-LOD ROBJ is emitted")) return 1;
	if (!expect(fine_job.draws[0].geometry.lod_index == 0 &&
			fine_job.draws[0].geometry.render_object_index == 0 &&
			fine_job.draws[1].geometry.render_object_index == 1 &&
			fine_job.draws[2].geometry.geometry_key == 580,
			"level 4 keeps the first LOD and carries stable geometry references")) return 1;
	TerrainStaticShadowPageInput overhead = coarse;
	overhead.surface_to_light = {0.0f, 1.0f, 0.0f};
	if (!expect(collector.compile(overhead).draws.size() == 1,
			"with the sun overhead the tile has no extension and the building "
			"(x - r = 66 > 64) drops out")) return 1;

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
	permuted.replace({rejected_no_shadow_building, extended_building,
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
	auto resized = extended_building;
	resized.model_radius_fixed += 65536;
	collector.replace({resized, admitted_item, off_page_building});
	const TerrainStaticShadowPageJob resized_job = collector.compile(coarse);
	if (!expect(resized_job.content.value != relit.content.value,
			"the caster sphere changes the page contribution stamp even before an "
			"adapter revision")) return 1;
	extended_building.transform_revision = 2;
	collector.replace({extended_building, admitted_item, off_page_building});
	const TerrainStaticShadowPageJob moved = collector.compile(coarse);
	if (!expect(moved.content.value != relit.content.value,
			"caster transform revision changes the page contribution stamp")) return 1;

	// The extension is t * r, not the projected silhouette: at a grazing sun
	// (l = (0.75, 0.25, 0), t_x = 1.5) a radius-2 mast standing 6 u east of
	// the tile extends it by only 3 u, so the tile rejects it however tall it
	// is and however far its real shadow falls back across the tile.
	coarse.page = TerrainTilePageKey{0, 0, 0, 0, 4};
	coarse.surface_to_light = {0.75f, 0.25f, 0.0f};
	coarse.light_epoch =
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 0.25f, 0.75f);
	auto mast = candidate(77, kEntityKindBuilding, 0, 70.0f, 32.0f, 2.0f,
			770, 1, 1);
	collector.replace({mast});
	if (!expect(collector.compile(coarse).draws.empty(),
			"the sun-extended tile rejects a narrow caster past t * r")) return 1;
	mast.position_fixed[0] = 69 * 65536;
	collector.replace({mast});
	if (!expect(collector.compile(coarse).draws.size() == 1,
			"one unit closer the extended edge (x - r = 67 <= 64 + 3) admits it")) return 1;

	// The compares are inclusive: a sphere touching the tile edge (x + r = x0,
	// no extension toward -x at this sun) belongs to the page east of it too.
	auto touching = candidate(78, kEntityKindBuilding, 0, 62.0f, 32.0f, 2.0f,
			780, 1, 1);
	collector.replace({touching});
	coarse.surface_to_light = {0.0f, 1.0f, 0.0f};
	coarse.light_epoch =
			terrain_tile_light_epoch_from_environment_tuple(0.0f, 1.0f, 0.0f);
	coarse.page = TerrainTilePageKey{0, 0, 64, 0, 4};
	if (!expect(collector.compile(coarse).draws.size() == 1,
			"a sphere touching the tile's west edge is admitted (inclusive)")) return 1;

	// One tile collects at most 0x400 casters [orig: @0x60D390, @0x60D40E].
	std::vector<TerrainStaticShadowCandidate> crowd;
	for (uint32_t index = 0; index < 1030; ++index) {
		crowd.push_back(candidate(1000 + static_cast<int>(index),
				kEntityKindBuilding, index, 96.0f, 32.0f, 1.0f,
				5000 + index, 1, 1));
	}
	collector.replace(std::move(crowd));
	if (!expect(collector.compile(coarse).draws.size() == 0x400,
			"the tile collector stops at 0x400 casters")) return 1;

	std::puts("OK: terrain static-shadow collector admission/order/tile test/stamp");
	return 0;
}
