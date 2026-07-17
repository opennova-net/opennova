// Literal vectors independently calculated from the recovered instructions.
#include <foliage/runtime.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <vector>

using namespace opennova;
using namespace opennova::foliage;

namespace {

bool expect(bool condition, const char *message) {
	if (condition) return true;
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

bool near(float actual, float expected, float epsilon = 1.0e-4f) {
	return std::fabs(actual - expected) <= epsilon;
}

float witness_height(float x, float z) {
	return 0.03125f * x * x + 0.015625f * z * z +
	       0.0078125f * x * z + 0.25f * x - 0.125f * z;
}

WorldSamplers world_with_foliage_mask(uint32_t foliage_mask) {
	WorldSamplers world;
	world.detail_foliage_mask_at = [foliage_mask](int32_t, int32_t) {
		return foliage_mask;
	};
	world.model_foliage_mask_at = [foliage_mask](int32_t, int32_t) {
		return foliage_mask;
	};
	world.height_at = [](float x, float z) { return witness_height(x, z); };
	world.path_blocked = [](float, float, float) { return false; };
	return world;
}

FrameRequest one_detail(float distance) {
	FrameRequest request;
	request.slots[0].enabled = true;
	request.slots[0].model_radius = 2.0f;
	request.detail_cells.push_back({0x00100030u, distance});
	return request;
}

FrameRequest one_silhouette(float depth, float distance) {
	FrameRequest request;
	request.slots[0].enabled = true;
	request.slots[0].model_radius = 2.0f;
	request.silhouette_anchors.push_back({{32.0f, 48.0f}, depth, distance});
	return request;
}

bool detail_vectors_and_gates() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	if (!expect(runtime.render_frame(one_detail(10.0f), world).detail.empty(),
	            "detail cache miss warms without drawing")) return false;
	auto out = runtime.render_frame(one_detail(10.0f), world);
	const auto near_stats = runtime.get_stats();
	if (!expect(out.detail.size() == 72 &&
	                near_stats.detail.submissions == 2,
	            "near detail submits the resident cell twice")) return false;

	const uint64_t high_submission = out.detail[0].submission_id;
	const uint64_t low_submission = out.detail[36].submission_id;
	if (!expect(high_submission != 0 &&
	                low_submission != 0 &&
	                high_submission != low_submission,
	            "near high and low passes have distinct submission identities")) {
		return false;
	}
	for (size_t candidate = 0; candidate < 36; ++candidate) {
		const DetailInstance &high = out.detail[candidate];
		const DetailInstance &low = out.detail[candidate + 36];
		if (!expect(high.submission_id == high_submission &&
		                high.pass == DetailPass::HighAlphaTest &&
		                high.alpha_reference == 180,
		            "near submission order starts with the depth-writing high pass")) {
			return false;
		}
		if (!expect(low.submission_id == low_submission &&
		                low.pass == DetailPass::LowAlphaTest &&
		                low.alpha_reference == 8,
		            "near submission order ends with the no-depth-write low pass")) {
			return false;
		}
		if (!expect(high.slot == low.slot &&
		                high.candidate == low.candidate &&
		                high.cell_key == low.cell_key &&
		                high.cache_revision == low.cache_revision &&
		                high.center.x == low.center.x &&
		                high.center.z == low.center.z &&
		                high.yaw_radians == low.yaw_radians,
		            "near low pass re-submits the exact high-pass geometry")) {
			return false;
		}
		if (!expect(near(high.alpha, 1.0f) && near(low.alpha, 1.0f),
		            "near secondary low pass shares the unscaled c6 fade")) {
			return false;
		}
		if (!expect(!high.near_secondary && low.near_secondary,
		            "only the near low resubmission carries the strict-LESS "
		            "secondary marker")) {
			return false;
		}
	}
	if (!expect(out.detail.size() == 72,
	            "near detail expands 36 accepted candidates into two passes")) return false;
	if (!expect(out.silhouettes.empty(), "detail cells never emit silhouettes")) return false;

	const struct Expected { float x, z, yaw; } expected[3] = {
	    {18.63215637f, 46.30134583f, 4.83127260f},
	    {20.66067505f, 45.73469543f, 1.83435345f},
	    {23.21640015f, 46.72764587f, 4.25919342f},
	};
	for (int i = 0; i < 3; ++i) {
		const auto &got = out.detail[static_cast<size_t>(i)];
		if (!expect(got.cell_key == 0x00100030u && got.candidate == i,
		            "detail preserves key and candidate index")) return false;
		if (!expect(near(got.center.x, expected[i].x) &&
		                near(got.center.z, expected[i].z) &&
		                near(got.yaw_radians, expected[i].yaw),
		            "detail position/yaw matches the recovered literal vector")) return false;
		if (!expect(near(got.alpha, 1.0f) &&
		                got.pass == DetailPass::HighAlphaTest &&
		                got.alpha_reference == 180,
		            "detail near range uses the high alpha-test pass")) return false;
	}

	world.detail_foliage_mask_at = [](int32_t, int32_t) { return 0u; };
	runtime.reset();
	runtime.render_frame(one_detail(10.0f), world);
	if (!expect(runtime.render_frame(one_detail(10.0f), world).detail.empty(),
	            "detail gates on the authored foliage map")) return false;

	world = world_with_foliage_mask(0x1u);
	world.path_blocked = [](float, float, float range) { return near(range, 2.0f); };
	runtime.reset();
	runtime.render_frame(one_detail(10.0f), world);
	if (!expect(runtime.render_frame(one_detail(10.0f), world).detail.empty(),
	            "detail rejects blocked candidates")) return false;
	auto forced = one_detail(10.0f);
	forced.slots[0].attrib_flags = FOLIAGE_ATTRIB_FORCE_ON;
	runtime.reset();
	runtime.render_frame(forced, world);
	if (!expect(runtime.render_frame(forced, world).detail.size() == 72,
	            "attrib bit 0 bypasses the blocked-path gate")) return false;

	world = world_with_foliage_mask(0x1u);
	runtime.reset();
	runtime.render_frame(one_detail(31.0f), world);
	auto fading = runtime.render_frame(one_detail(31.0f), world);
	if (!expect(fading.detail.size() == 72 &&
	                near(fading.detail[0].alpha, 0.5f) &&
	                near(fading.detail[36].alpha, 0.5f) &&
	                fading.detail[0].pass == DetailPass::HighAlphaTest &&
	                fading.detail[36].pass == DetailPass::LowAlphaTest &&
	                fading.detail[36].near_secondary &&
	                fading.detail[0].alpha_reference == 180,
	            "detail fade is 1 through 20 then linear to 0 at 42, shared "
	            "by both near submissions")) return false;
	auto low = runtime.render_frame(one_detail(33.0f), world);
	if (!expect(low.detail.size() == 36 &&
	                near(low.detail[0].alpha, 9.0f / 22.0f) &&
	                low.detail[0].pass == DetailPass::LowAlphaTest &&
	                !low.detail[0].near_secondary &&
	                low.detail[0].alpha_reference == 8,
	            "distance 33 switches to the primary low alpha-test pass")) return false;
	if (!expect(runtime.render_frame(one_detail(42.01f), world).detail.empty(),
	            "detail cells beyond 42 units are rejected")) return false;
	auto invalid = one_detail(10.0f);
	invalid.detail_cells[0].key |= 0x80000000u;
	if (!expect(runtime.render_frame(invalid, world).detail.empty(),
	            "detail key bit 31 is the retail invalid sentinel")) return false;
	return true;
}

bool silhouette_vectors_and_tier_role() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	if (!expect(runtime.render_frame(one_silhouette(37.99f, 63.0f), world)
	                .silhouettes.empty(),
	            "silhouette anchors require view depth at least 38")) return false;

	auto out = runtime.render_frame(one_silhouette(38.0f, 63.0f), world);
	if (!expect(out.detail.empty(), "silhouette anchors never emit detail")) return false;
	if (!expect(out.silhouettes.size() == 9,
	            "four quadrants plus +/-4 anchor gate match the golden count")) return false;

	const uint32_t quadrant_keys[4] = {
	    0x00200040u, 0x00100040u, 0x00200030u, 0x00100030u,
	};
	const int quadrant_counts[4] = {3, 3, 1, 2};
	std::map<uint32_t, int> counts;
	for (const auto &inst : out.silhouettes) {
		++counts[inst.cell_key];
		if (!expect(inst.alpha_reference == 64,
		            "silhouette alpha ref is int(4096/(distance+1))")) return false;
		if (!expect(std::fabs(inst.center.x - 32.0f) <= 4.0f &&
		                std::fabs(inst.center.z - 48.0f) <= 4.0f,
		            "silhouette obeys the +/-4u Chebyshev gate")) return false;
	}
	for (int q = 0; q < 4; ++q) {
		if (!expect(counts[quadrant_keys[q]] == quadrant_counts[q],
		            "silhouette quadrant key/count matches the literal vector")) return false;
		if (!expect(counts[quadrant_keys[q]] <= 21,
		            "each silhouette cell respects the retail cap")) return false;
	}

	const auto &first = out.silhouettes[0];
	if (!expect(first.cell_key == 0x00200040u &&
	                first.quadrant == 0 && first.candidate == 24,
	            "silhouette retains quadrant/key/candidate identity")) return false;
	if (!expect(near(first.center.x, 33.30752563f) &&
	                near(first.center.z, 51.89562988f) &&
	                near(first.yaw_radians, 0.47121975f),
	            "silhouette placement matches the PRNG literal")) return false;

	const GroundCorner expected_corners[4] = {
	    {32.65196228f, 53.91311646f, 93.90994263f},
	    {35.32501221f, 52.55119324f, 98.91120148f},
	    {31.29003906f, 51.24006653f, 85.56327820f},
	    {33.96308899f, 49.87815857f, 90.40946960f},
	};
	for (int i = 0; i < 4; ++i) {
		if (!expect(near(first.corners[i].x, expected_corners[i].x) &&
		                near(first.corners[i].z, expected_corners[i].z) &&
		                near(first.corners[i].height,
		                     expected_corners[i].height, 2.0e-4f),
		            "silhouette corner vector matches four ground samples")) return false;
	}
	const float expected_fold[4] = {
	    -0.04954147f, 0.00000381f, -0.05598831f, 0.00000381f,
	};
	for (int i = 0; i < 4; ++i) {
		if (!expect(near(first.fold[i], expected_fold[i], 2.0e-4f),
		            "silhouette fold matches four midpoint controls")) return false;
	}
	if (!expect(near(first.center_height, 92.09294128f, 2.0e-4f),
	            "silhouette center height uses the eight-sample fit")) return false;

	world.model_foliage_mask_at = [](int32_t, int32_t) { return 0u; };
	runtime.reset();
	if (!expect(runtime.render_frame(one_silhouette(38.0f, 63.0f), world)
	                .silhouettes.empty(),
	            "silhouette gates on foliagemap, not surface/charmap")) return false;
	return true;
}

bool tier_specific_foliage_sampler_routing() {
	int detail_calls = 0;
	int model_calls = 0;
	int32_t sampled_detail_x = 0;
	int32_t sampled_detail_z = 0;
	auto world = world_with_foliage_mask(0u);
	world.detail_foliage_mask_at =
	    [&](int32_t world_x_fixed, int32_t world_z_fixed) {
		if (detail_calls == 0) {
			sampled_detail_x = world_x_fixed;
			sampled_detail_z = world_z_fixed;
		}
		++detail_calls;
		return 0x1u;
	};
	world.model_foliage_mask_at = [&model_calls](int32_t, int32_t) {
		++model_calls;
		return 0u;
	};
	Runtime detail_runtime;
	detail_runtime.render_frame(one_detail(10.0f), world);
	const auto detail_output =
	    detail_runtime.render_frame(one_detail(10.0f), world);
	if (!expect(detail_calls > 0 && model_calls == 0 &&
	                detail_output.detail.size() == 72 &&
	                sampled_detail_x ==
	                    static_cast<int32_t>(
	                        detail_output.detail[0].center.x * 65536.0f) &&
	                sampled_detail_z ==
	                    static_cast<int32_t>(
	                        detail_output.detail[0].center.z * 65536.0f),
	            "detail generation uses only the flat-map sampler")) {
		return false;
	}

	detail_calls = 0;
	model_calls = 0;
	int32_t sampled_model_x = 0;
	int32_t sampled_model_z = 0;
	world.detail_foliage_mask_at = [&detail_calls](int32_t, int32_t) {
		++detail_calls;
		return 0u;
	};
	world.model_foliage_mask_at =
	    [&](int32_t world_x_fixed, int32_t world_z_fixed) {
		if (model_calls == 0) {
			sampled_model_x = world_x_fixed;
			sampled_model_z = world_z_fixed;
		}
		++model_calls;
		return 0x1u;
	};
	Runtime model_runtime;
	const auto model_output =
	    model_runtime.render_frame(one_silhouette(38.0f, 63.0f), world);
	return expect(detail_calls == 0 && model_calls > 0 &&
	                  model_output.silhouettes.size() == 9 &&
	                  sampled_model_x ==
	                      static_cast<int32_t>(
	                          model_output.silhouettes[0].center.x * 65536.0f) &&
	                  sampled_model_z ==
	                      static_cast<int32_t>(
	                          model_output.silhouettes[0].center.z * 65536.0f),
	              "MODEL generation uses only the sector-routed sampler");
}

bool detail_cache_temporal_semantics() {
	Runtime runtime;
	uint32_t foliage_mask = 0x1u;
	auto world = world_with_foliage_mask(0x1u);
	world.detail_foliage_mask_at = [&foliage_mask](int32_t, int32_t) {
		return foliage_mask;
	};
	auto request = one_detail(10.0f);
	request.detail_cells.push_back(request.detail_cells[0]);

	const auto first = runtime.render_frame(request, world);
	const auto first_stats = runtime.get_stats();
	if (!expect(first.detail.empty() &&
	                first_stats.detail.misses == 2 &&
	                first_stats.detail.regenerations == 1 &&
	                first_stats.detail.residents == 1 &&
	                first.detail_generated.size() == 1,
	            "duplicate detail misses allocate one warm resident")) return false;
	const uint64_t revision = first.detail_generated[0].revision;

	const auto second = runtime.render_frame(request, world);
	const auto second_stats = runtime.get_stats();
	if (!expect(second.detail.size() == 144 &&
	                second_stats.detail.hits == 2 &&
	                second_stats.detail.submissions == 4 &&
	                second_stats.detail.regenerations == 0 &&
	                second.detail_generated.empty(),
	            "duplicate resident keys each retain both retail pass submissions")) return false;
	std::set<uint64_t> submission_ids;
	for (const DetailInstance &instance : second.detail) {
		if (!expect(instance.cache_revision == revision,
		            "detail revision stays stable on a hit")) return false;
		submission_ids.insert(instance.submission_id);
	}
	if (!expect(submission_ids.size() == 4,
	            "each duplicate key has distinct high and low submission ids")) return false;

	foliage_mask = 0u;
	if (!expect(runtime.render_frame(request, world).detail.size() == 144,
	            "detail hit reuses cached geometry without resampling")) return false;

	runtime.reset();
	if (!expect(runtime.get_stats().detail.residents == 0 &&
	                runtime.get_stats().terrain_scene_counter == 0,
	            "runtime reset invalidates detail state and cadence")) return false;
	foliage_mask = 0x1u;
	const auto rewarmed = runtime.render_frame(request, world);
	if (!expect(rewarmed.detail.empty() &&
	                rewarmed.detail_generated.size() == 1 &&
	                rewarmed.detail_generated[0].revision != revision,
	            "reset forces a new detail cache revision")) return false;

	Runtime key_zero_runtime;
	auto key_zero = one_detail(10.0f);
	key_zero.detail_cells[0].key = 0u;
	key_zero_runtime.render_frame(key_zero, world);
	if (!expect(key_zero_runtime.render_frame(key_zero, world).detail.size() == 72,
	            "explicit validity makes packed key zero cache normally")) return false;
	return true;
}

bool detail_capacity_and_eviction() {
	if (!expect(Runtime::detail_cache_capacity(0) == 0 &&
	                Runtime::detail_cache_capacity(1) == 128 &&
	                Runtime::detail_cache_capacity(14) == 128 &&
	                Runtime::detail_cache_capacity(15) == 121 &&
	                Runtime::detail_cache_capacity(1820) == 1 &&
	                Runtime::detail_cache_capacity(1821) == 0,
	            "detail capacity follows the strict 16-bit retail formula")) return false;

	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	auto first_request = one_detail(10.0f);
	first_request.slots[0].source_vertex_count = 1820;
	const auto first = runtime.render_frame(first_request, world);
	if (!expect(first.detail_generated.size() == 1,
	            "capacity-one detail pool generates its first key")) return false;

	auto second_request = first_request;
	second_request.detail_cells[0].key = 0x00200030u;
	const auto second = runtime.render_frame(second_request, world);
	if (!expect(second.detail.empty() &&
	                second.detail_evicted.size() == 1 &&
	                second.detail_generated.size() == 1 &&
	                second.detail_evicted[0].revision ==
	                    first.detail_generated[0].revision &&
	                runtime.get_stats().detail.evictions == 1,
	            "detail replacement emits old and new cache identities")) return false;
	const auto third = runtime.render_frame(second_request, world);
	if (!expect(third.detail.size() == 72 &&
	                third.detail[0].cache_revision ==
	                    second.detail_generated[0].revision,
	            "replacement detail geometry appears one render later")) return false;
	return true;
}

bool model_cache_phase_negative_and_identity() {
	Runtime runtime;
	uint32_t foliage_mask = 0x1u;
	auto world = world_with_foliage_mask(0x1u);
	world.model_foliage_mask_at = [&foliage_mask](int32_t, int32_t) {
		return foliage_mask;
	};
	auto request = one_silhouette(38.0f, 63.0f);
	request.silhouette_anchors.push_back(request.silhouette_anchors[0]);

	const auto first = runtime.render_frame(request, world);
	const auto first_stats = runtime.get_stats();
	if (!expect(first.silhouettes.size() == 18 &&
	                first_stats.model.misses == 4 &&
	                first_stats.model.hits == 4 &&
	                first_stats.model.regenerations == 4 &&
	                first_stats.model.submissions == 8 &&
	                first.model_generated.size() == 4,
	            "model miss emits immediately and repeated anchors hit cache")) return false;
	std::map<uint32_t, std::set<uint64_t>> revisions_by_key;
	std::map<uint32_t, std::set<uint64_t>> submissions_by_key;
	for (const SilhouetteInstance &instance : first.silhouettes) {
		revisions_by_key[instance.cell_key].insert(instance.cache_revision);
		submissions_by_key[instance.cell_key].insert(instance.submission_id);
	}
	for (const auto &item : revisions_by_key) {
		if (!expect(item.second.size() == 1 &&
		                submissions_by_key[item.first].size() == 2,
		            "off-phase repeated model key reuses revision but not submission")) return false;
	}

	foliage_mask = 0u;
	for (int frame = 2; frame <= 7; ++frame) {
		const auto reused = runtime.render_frame(request, world);
		if (!expect(reused.silhouettes.size() == 18 &&
		                runtime.get_stats().model.regenerations == 0,
		            "definition zero reuses model geometry before phase eight")) return false;
	}
	const auto phase_eight = runtime.render_frame(request, world);
	if (!expect(phase_eight.silhouettes.empty() &&
	                runtime.get_stats().model.hits == 8 &&
	                runtime.get_stats().model.regenerations == 4 &&
	                phase_eight.model_evicted.size() == 4 &&
	                phase_eight.model_generated.size() == 4,
	            "each resident key refreshes once on definition zero phase")) return false;

	foliage_mask = 0x1u;
	if (!expect(runtime.render_frame(request, world).silhouettes.empty() &&
	                runtime.get_stats().model.regenerations == 0,
	            "empty model result is negative-cached off phase")) return false;
	for (int frame = 10; frame <= 15; ++frame) {
		runtime.render_frame(request, world);
	}
	const auto phase_sixteen = runtime.render_frame(request, world);
	if (!expect(phase_sixteen.silhouettes.size() == 18 &&
	                runtime.get_stats().model.regenerations == 4,
	            "negative model entries recover once per key on the next phase")) return false;
	return true;
}

bool overlapping_model_cells_refresh_once_per_frame() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	auto request = one_silhouette(38.0f, 63.0f);
	request.silhouette_anchors.push_back(
	    {{34.0f, 48.0f}, 38.0f, 63.0f});

	const auto first = runtime.render_frame(request, world);
	if (!expect(!first.silhouettes.empty() &&
	                runtime.get_stats().model.regenerations == 4,
	            "overlapping MODEL anchors warm four unique cell keys")) {
		return false;
	}
	const size_t stable_count = first.silhouettes.size();
	for (int frame = 2; frame <= 7; ++frame) {
		if (!expect(runtime.render_frame(request, world).silhouettes.size() ==
		                stable_count,
		            "overlapping MODEL cells remain stable before refresh")) {
			return false;
		}
	}

	const auto phase_eight = runtime.render_frame(request, world);
	if (!expect(phase_eight.silhouettes.size() == stable_count &&
	                runtime.get_stats().model.hits == 8 &&
	                runtime.get_stats().model.regenerations == 4 &&
	                phase_eight.model_generated.empty() &&
	                phase_eight.model_evicted.empty(),
	            "unchanged overlapping MODEL cells keep their resident mesh revision")) {
		return false;
	}
	std::map<uint32_t, std::set<uint64_t>> revisions_by_key;
	for (const SilhouetteInstance &instance : phase_eight.silhouettes) {
		revisions_by_key[instance.cell_key].insert(instance.cache_revision);
	}
	for (const auto &item : revisions_by_key) {
		if (!expect(item.second.size() == 1,
		            "overlapping submissions share the first refreshed cell revision")) {
			return false;
		}
	}
	return true;
}

bool model_key_lookup_work_is_bounded_by_cell_visits() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	auto request = one_silhouette(38.0f, 63.0f);
	constexpr int kAnchorCount = 64;
	for (int index = 1; index < kAnchorCount; ++index) {
		request.silhouette_anchors.push_back(request.silhouette_anchors[0]);
	}

	const auto output = runtime.render_frame(request, world);
	const uint64_t expected_cell_visits =
	    static_cast<uint64_t>(kAnchorCount) * 4u;
	if (!expect(!output.silhouettes.empty() &&
	                runtime.get_stats().model_key_lookup_steps ==
	                    expected_cell_visits,
	            "MODEL key lookup work stays proportional to cell visits, not "
	            "the 1000-entry cache capacity")) {
		return false;
	}
	return true;
}

bool model_definition_stagger() {
	Runtime runtime;
	uint32_t foliage_mask = 0xFu;
	auto world = world_with_foliage_mask(foliage_mask);
	world.model_foliage_mask_at = [&foliage_mask](int32_t, int32_t) {
		return foliage_mask;
	};
	auto request = one_silhouette(38.0f, 63.0f);
	for (int slot = 1; slot < FOLIAGE_MAX_DEFS; ++slot) {
		request.slots[slot].enabled = true;
		request.slots[slot].model_radius = 2.0f;
	}
	if (!expect(runtime.render_frame(request, world).silhouettes.size() == 36,
	            "all four model definitions populate on miss")) return false;
	foliage_mask = 0u;
	for (int frame = 2; frame <= 8; ++frame) {
		const auto output = runtime.render_frame(request, world);
		int counts[FOLIAGE_MAX_DEFS] = {};
		for (const SilhouetteInstance &instance : output.silhouettes) {
			++counts[instance.slot];
		}
		for (int slot = 0; slot < FOLIAGE_MAX_DEFS; ++slot) {
			const int phase = 8 - 2 * slot;
			const int expected = frame >= phase ? 0 : 9;
			if (!expect(counts[slot] == expected,
			            "definitions refresh on phases 0,6,4,2 respectively")) return false;
		}
	}
	return true;
}

bool model_cache_lru_and_identity_events() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x0u);
	FrameRequest fill;
	fill.slots[0].enabled = true;
	fill.slots[0].model_radius = 2.0f;
	for (int index = 0; index < 250; ++index) {
		fill.silhouette_anchors.push_back(
		    {{32.0f + 64.0f * static_cast<float>(index), 48.0f},
		     38.0f,
		     63.0f});
	}
	const auto populated = runtime.render_frame(fill, world);
	if (!expect(populated.model_generated.size() == 1000 &&
	                runtime.get_stats().model.misses == 1000 &&
	                runtime.get_stats().model.evictions == 0 &&
	                runtime.get_stats().model.residents == 1000,
	            "model cache has exactly 1000 resident entries per definition")) return false;

	FrameRequest touch;
	touch.slots[0] = fill.slots[0];
	touch.silhouette_anchors.push_back(fill.silhouette_anchors[0]);
	runtime.render_frame(touch, world);
	if (!expect(runtime.get_stats().model.hits == 4,
	            "touch refreshes the first four model LRU entries")) return false;

	FrameRequest replace;
	replace.slots[0] = fill.slots[0];
	replace.silhouette_anchors.push_back(
	    {{32.0f + 64.0f * 250.0f, 48.0f}, 38.0f, 63.0f});
	const auto replaced = runtime.render_frame(replace, world);
	if (!expect(replaced.model_evicted.size() == 4 &&
	                replaced.model_generated.size() == 4 &&
	                runtime.get_stats().model.evictions == 4,
	            "full model cache replaces four entries for a new anchor")) return false;
	for (size_t index = 0; index < 4; ++index) {
		if (!expect(replaced.model_evicted[index].key ==
		                populated.model_generated[index + 4].key &&
		                replaced.model_evicted[index].revision ==
		                populated.model_generated[index + 4].revision,
		            "strict equal-age LRU evicts the lowest untouched indices")) return false;
	}

	FrameRequest probe;
	probe.slots[0] = fill.slots[0];
	probe.silhouette_anchors.push_back(fill.silhouette_anchors[1]);
	probe.silhouette_anchors.push_back(fill.silhouette_anchors[249]);
	runtime.render_frame(probe, world);
	if (!expect(runtime.get_stats().model.misses == 4 &&
	                runtime.get_stats().model.hits == 4,
	            "evicted low-index anchor misses while late resident anchor hits")) return false;
	return true;
}

bool model_path_blocker_gate_and_force_on_bypass() {
	// The model tier consults the placed-tile path blocker once per candidate
	// that survives the +-4u anchor box, at the witnessed 2.0u spacing, and a
	// blocked candidate is dropped. Definition attrib bit 0 skips the gate
	// entirely (the sampler is never called).
	// [orig: Foliage_PathBlockedByPlacedTile @ 0x606490;
	// Foliage_GenerateModelTileInstances @ 0x600980 — the FORCE_ON attribute
	// gate on the def record's attrib byte]
	auto world = world_with_foliage_mask(0x1u);
	int blocked_calls = 0;
	bool range_is_retail = true;
	world.path_blocked = [&blocked_calls, &range_is_retail](
	                         float, float, float range) {
		++blocked_calls;
		if (!near(range, 2.0f)) range_is_retail = false;
		return true;
	};

	Runtime blocked_runtime;
	if (!expect(blocked_runtime.render_frame(one_silhouette(38.0f, 63.0f), world)
	                .silhouettes.empty(),
	            "a blocking placed tile rejects every model-tier candidate")) {
		return false;
	}
	if (!expect(blocked_calls == 9,
	            "each of the 9 in-box candidates consults the blocker once")) {
		return false;
	}
	if (!expect(range_is_retail,
	            "the model tier passes the witnessed 2.0u (0x20000) spacing")) {
		return false;
	}

	blocked_calls = 0;
	Runtime forced_runtime;
	auto forced = one_silhouette(38.0f, 63.0f);
	forced.slots[0].attrib_flags = FOLIAGE_ATTRIB_FORCE_ON;
	if (!expect(forced_runtime.render_frame(forced, world)
	                    .silhouettes.size() == 9,
	            "FORCE_ON bypasses the blocker and restores the golden count")) {
		return false;
	}
	if (!expect(blocked_calls == 0,
	            "FORCE_ON never calls the path sampler")) return false;
	return true;
}

bool per_slot_mask_bit_selection_in_both_tiers() {
	// Both tiers gate candidates on mask & (1u << slot_index). A foliage mask
	// of 2 carries only slot 1's bit: slot 0 generates nothing while slot 1
	// generates the full candidate sets, so a regression to `mask & 1u` fails.
	// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
	// Foliage_GenerateModelTileInstances @ 0x600980]
	auto world = world_with_foliage_mask(0x2u);

	Runtime detail_slot0;
	detail_slot0.render_frame(one_detail(10.0f), world);
	if (!expect(detail_slot0.render_frame(one_detail(10.0f), world)
	                .detail.empty(),
	            "detail slot 0 generates nothing when only bit 1 is set")) {
		return false;
	}

	Runtime detail_slot1;
	FrameRequest detail_request;
	detail_request.slots[1].enabled = true;
	detail_request.slots[1].model_radius = 2.0f;
	detail_request.detail_cells.push_back({0x00100030u, 10.0f});
	detail_slot1.render_frame(detail_request, world);
	const auto detail_out = detail_slot1.render_frame(detail_request, world);
	if (!expect(detail_out.detail.size() == 72,
	            "detail slot 1 accepts all 36 candidates under mask bit 1")) {
		return false;
	}
	for (const DetailInstance &instance : detail_out.detail) {
		if (!expect(instance.slot == 1,
		            "mask-selected detail instances carry slot index 1")) {
			return false;
		}
	}

	Runtime model_slot0;
	if (!expect(model_slot0.render_frame(one_silhouette(38.0f, 63.0f), world)
	                .silhouettes.empty(),
	            "model slot 0 generates nothing when only bit 1 is set")) {
		return false;
	}

	Runtime model_slot1;
	FrameRequest model_request;
	model_request.slots[1].enabled = true;
	model_request.slots[1].model_radius = 2.0f;
	model_request.silhouette_anchors.push_back({{32.0f, 48.0f}, 38.0f, 63.0f});
	const auto model_out = model_slot1.render_frame(model_request, world);
	if (!expect(model_out.silhouettes.size() == 9,
	            "model slot 1 emits the golden 9 under mask bit 1")) return false;
	for (const SilhouetteInstance &instance : model_out.silhouettes) {
		if (!expect(instance.slot == 1,
		            "mask-selected model instances carry slot index 1")) {
			return false;
		}
	}
	return true;
}

bool negative_anchor_cell_keys_sign_extend() {
	// Negative-coordinate cells: the quadrant snap wraps in 32 bits and the
	// packed key's 15-bit halves sign-extend back to the negative bases, with
	// X = high15 + localA and Z = low15 - localB (local B runs toward -Z).
	// [orig: Foliage_UpdateModelTiles @ 0x601f50 — quadrant snap / key form;
	// Foliage_GenerateModelTileInstances @ 0x600980 — 15-bit decode]
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	FrameRequest request;
	request.slots[0].enabled = true;
	request.slots[0].model_radius = 2.0f;
	request.silhouette_anchors.push_back({{-32.0f, -48.0f}, 38.0f, 63.0f});

	const auto out = runtime.render_frame(request, world);
	if (!expect(out.silhouettes.size() == 8,
	            "the negative anchor emits the golden candidate count")) {
		return false;
	}

	// Hand-derived quadrant keys for anchor (-32, -48): snap(anchor +- 8u)
	// masked to 16u tiles gives x in {-32, -48}, z-top in {-32, -48}.
	const uint32_t quadrant_keys[4] = {
	    0x7FE07FE0u, 0x7FD07FE0u, 0x7FE07FD0u, 0x7FD07FD0u,
	};
	const int quadrant_counts[4] = {2, 2, 2, 2};

	// Independent 15-bit sign extension (subtraction form, distinct from the
	// runtime's OR-mask form).
	const auto sext15 = [](uint32_t value) {
		const int32_t low = static_cast<int32_t>(value & 0x7FFFu);
		return (low & 0x4000) != 0 ? low - 0x8000 : low;
	};
	std::map<uint32_t, int> counts;
	for (const SilhouetteInstance &instance : out.silhouettes) {
		++counts[instance.cell_key];
		const int32_t base_x = sext15(instance.cell_key >> 16u);
		const int32_t base_z = sext15(instance.cell_key);
		if (!expect(base_x < 0 && base_z < 0,
		            "negative-anchor keys decode to negative 16u bases")) {
			return false;
		}
		const float local_a = instance.center.x - static_cast<float>(base_x);
		const float local_b = static_cast<float>(base_z) - instance.center.z;
		if (!expect(local_a >= 1.0f - 1.0e-4f && local_a <= 15.8f + 1.0e-4f,
		            "X decodes as high15 + localA for negative keys")) {
			return false;
		}
		if (!expect(local_b >= 1.0f - 1.0e-4f && local_b <= 15.8f + 1.0e-4f,
		            "Z decodes as low15 - localB for negative keys")) {
			return false;
		}
		if (!expect(std::fabs(instance.center.x + 32.0f) <= 4.0f &&
		                std::fabs(instance.center.z + 48.0f) <= 4.0f,
		            "negative-key instances stay inside the +-4u anchor box")) {
			return false;
		}
	}
	for (int q = 0; q < 4; ++q) {
		if (!expect(counts[quadrant_keys[q]] == quadrant_counts[q],
		            "negative quadrant key/count matches the literal vector")) {
			return false;
		}
	}
	return true;
}

bool detail_cache_lru_evicts_oldest_at_capacity_three() {
	// Strict signed-age LRU across the persistent detail pool at a capacity
	// where eviction order is distinguishable: with residents A,B,C and only
	// A,B re-touched, a new key replaces the OLDEST-stamped entry C — not the
	// newest and not slot zero.
	// [orig: Foliage_UpdateFarCellSlots @ 0x601b30;
	// terrain_tile_init_buffers @ 0x5ff920 — pool sizing]
	if (!expect(Runtime::detail_cache_capacity(500) == 3,
	            "source vertex count 500 sizes the pool to three cells")) {
		return false;
	}

	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	const uint32_t key_a = 0x00100030u;
	const uint32_t key_b = 0x00200030u;
	const uint32_t key_c = 0x00300030u;
	const uint32_t key_d = 0x00400030u;
	const auto make_request = [](std::initializer_list<uint32_t> keys) {
		FrameRequest request;
		request.slots[0].enabled = true;
		request.slots[0].model_radius = 2.0f;
		request.slots[0].source_vertex_count = 500;
		for (uint32_t key : keys) {
			request.detail_cells.push_back({key, 10.0f});
		}
		return request;
	};

	const auto first = runtime.render_frame(make_request({key_a}), world);
	if (!expect(first.detail_generated.size() == 1 &&
	                first.detail_generated[0].key == key_a,
	            "frame one warms resident A")) return false;
	const auto second = runtime.render_frame(make_request({key_a, key_b}), world);
	if (!expect(second.detail_generated.size() == 1 &&
	                second.detail_generated[0].key == key_b &&
	                runtime.get_stats().detail.hits == 1,
	            "frame two touches A and warms resident B")) return false;
	const auto third = runtime.render_frame(make_request({key_b, key_c}), world);
	if (!expect(third.detail_generated.size() == 1 &&
	                third.detail_generated[0].key == key_c &&
	                runtime.get_stats().detail.residents == 3 &&
	                runtime.get_stats().detail.evictions == 0,
	            "frame three fills the pool without evicting")) return false;
	const uint64_t revision_c = third.detail_generated[0].revision;

	const auto fourth = runtime.render_frame(make_request({key_a, key_b}), world);
	if (!expect(fourth.detail_generated.empty() &&
	                runtime.get_stats().detail.hits == 2 &&
	                runtime.get_stats().detail.misses == 0,
	            "frame four re-touches A and B, leaving C oldest")) return false;

	const auto fifth = runtime.render_frame(make_request({key_d}), world);
	if (!expect(fifth.detail_evicted.size() == 1 &&
	                fifth.detail_evicted[0].key == key_c &&
	                fifth.detail_evicted[0].revision == revision_c &&
	                fifth.detail_generated.size() == 1 &&
	                fifth.detail_generated[0].key == key_d &&
	                runtime.get_stats().detail.evictions == 1,
	            "the new key evicts the oldest-stamped resident C, not the "
	            "newest and not slot zero")) return false;

	// C occupied the last pool slot; a fixed-index policy could fake the
	// first eviction. Round two leaves the oldest resident (A) in the FIRST
	// slot: only true oldest-first eviction picks it again.
	const uint64_t revision_a = first.detail_generated[0].revision;
	const auto sixth = runtime.render_frame(make_request({key_b, key_d}), world);
	if (!expect(sixth.detail_generated.empty() &&
	                runtime.get_stats().detail.hits == 2 &&
	                runtime.get_stats().detail.misses == 0,
	            "frame six re-touches B and D, leaving A oldest")) return false;
	const uint32_t key_e = 0x00500030u;
	const auto seventh = runtime.render_frame(make_request({key_e}), world);
	if (!expect(seventh.detail_evicted.size() == 1 &&
	                seventh.detail_evicted[0].key == key_a &&
	                seventh.detail_evicted[0].revision == revision_a &&
	                seventh.detail_generated.size() == 1 &&
	                seventh.detail_generated[0].key == key_e &&
	                runtime.get_stats().detail.evictions == 1,
	            "round two evicts the oldest-stamped resident A from the "
	            "first pool slot")) return false;

	const auto eighth = runtime.render_frame(make_request({key_b, key_d}), world);
	if (!expect(eighth.detail.size() == 144 &&
	                runtime.get_stats().detail.hits == 2 &&
	                runtime.get_stats().detail.misses == 0,
	            "the re-touched residents B and D survived both evictions")) {
		return false;
	}
	return true;
}

bool invalid_anchors_and_reconfigure_stats() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	auto request = one_silhouette(38.0f, 63.0f);
	request.silhouette_anchors.push_back(
	    {{std::numeric_limits<float>::quiet_NaN(), 0.0f}, 38.0f, 63.0f});
	request.silhouette_anchors.push_back(
	    {{std::numeric_limits<float>::infinity(), 0.0f}, 38.0f, 63.0f});
	request.silhouette_anchors.push_back(
	    {{32768.0f, 0.0f}, 38.0f, 63.0f});
	request.silhouette_anchors.push_back(
	    {{0.0f, 0.0f}, std::numeric_limits<float>::quiet_NaN(), 63.0f});
	request.silhouette_anchors.push_back(
	    {{0.0f, 0.0f}, 38.0f, std::numeric_limits<float>::infinity()});
	const auto output = runtime.render_frame(request, world);
	if (!expect(output.silhouettes.size() == 9,
	            "invalid public silhouette anchors are ignored without changing valid output")) {
		return false;
	}

	Runtime extreme_runtime;
	auto extreme_distance = one_silhouette(
	    38.0f, std::numeric_limits<float>::max());
	const auto extreme_output =
	    extreme_runtime.render_frame(extreme_distance, world);
	if (!expect(extreme_output.silhouettes.size() == 9 &&
	                extreme_output.silhouettes[0].alpha_reference == 8,
	            "large finite camera distance clamps before integer conversion")) {
		return false;
	}

	for (float invalid_radius :
	     {std::numeric_limits<float>::quiet_NaN(),
	      std::numeric_limits<float>::infinity(),
	      std::numeric_limits<float>::max(),
	      -1.0f}) {
		Runtime radius_runtime;
		auto invalid_slot = one_silhouette(38.0f, 63.0f);
		invalid_slot.slots[0].model_radius = invalid_radius;
		if (!expect(
		        radius_runtime.render_frame(invalid_slot, world)
		            .silhouettes.empty(),
		        "invalid or unrepresentable public model radii are rejected")) {
			return false;
		}
	}

	Runtime detail_runtime;
	auto detail_world = world_with_foliage_mask(0x1u);
	auto detail_request = one_detail(10.0f);
	detail_runtime.render_frame(detail_request, detail_world);
	detail_runtime.render_frame(detail_request, detail_world);
	detail_request.slots[0].model_radius = 3.0f;
	const auto reconfigured =
	    detail_runtime.render_frame(detail_request, detail_world);
	if (!expect(reconfigured.detail_evicted.size() == 1 &&
	                detail_runtime.get_stats().detail.evictions == 1,
	            "slot reconfiguration accounts for every emitted cache eviction")) {
		return false;
	}
	return true;
}

bool fd_bake_vector() {
	Runtime runtime;
	std::vector<uint8_t> source(8u * 8u * 4u);
	for (int y = 0; y < 8; ++y) {
		for (int x = 0; x < 8; ++x) {
			const size_t offset =
			    (static_cast<size_t>(y) * 8u + static_cast<size_t>(x)) * 4u;
			source[offset + 0u] = static_cast<uint8_t>((11 + x * 19 + y * 7) & 0xFF);
			source[offset + 1u] = static_cast<uint8_t>((5 + x * 3 + y * 29) & 0xFF);
			source[offset + 2u] = static_cast<uint8_t>((17 + x * 23 + y * 11) & 0xFF);
			source[offset + 3u] = static_cast<uint8_t>((13 + x * 31 + y * 37) & 0xFF);
		}
	}

	FdMipChain chain;
	if (!expect(runtime.build_fd_rgba_mip_chain(
	                source.data(), 8, 8, chain),
	            "8x8 :fd input builds a complete mip chain")) {
		return false;
	}
	if (!expect(chain.width == 8 && chain.height == 8 &&
	                chain.retail_level_count == 2 &&
	                chain.rgba.size() == 8u * 8u * 4u + 4u * 4u * 4u +
	                                         2u * 2u * 4u + 1u * 1u * 4u,
	            "retail emits 8x8/4x4 and the Godot payload appends 2x2/1x1")) {
		return false;
	}

	static constexpr uint8_t expected_alpha[64] = {
	    71, 56, 87, 118, 149, 180, 179, 149,
	    79, 81, 112, 143, 174, 173, 172, 77,
	    84, 118, 149, 180, 179, 178, 81, 82,
	    121, 155, 186, 185, 184, 87, 86, 87,
	    158, 160, 175, 158, 93, 92, 91, 124,
	    163, 165, 100, 83, 98, 97, 128, 161,
	    168, 74, 73, 72, 103, 134, 165, 166,
	    97, 66, 65, 96, 127, 158, 189, 175,
	};
	for (size_t index = 0; index < 64u; ++index) {
		if (!expect(chain.rgba[index * 4u + 0u] == source[index * 4u + 0u] &&
		                chain.rgba[index * 4u + 1u] == source[index * 4u + 1u] &&
		                chain.rgba[index * 4u + 2u] == source[index * 4u + 2u] &&
		                chain.rgba[index * 4u + 3u] == expected_alpha[index],
		            "mip zero preserves RGB and replaces only wrapped-smoothed alpha")) {
			return false;
		}
	}

	static constexpr uint8_t expected_4x4[64] = {
	    89, 87, 92, 71, 103, 90, 110, 115,
	    117, 92, 127, 169, 131, 94, 144, 144,
	    94, 109, 101, 119, 108, 111, 118, 175,
	    122, 114, 135, 157, 137, 116, 152, 84,
	    99, 131, 109, 161, 113, 133, 126, 129,
	    128, 135, 143, 95, 142, 138, 161, 126,
	    104, 153, 117, 101, 119, 155, 134, 76,
	    133, 157, 152, 130, 147, 159, 169, 173,
	};
	static constexpr uint8_t expected_2x2[16] = {
	    98, 99, 105, 120, 126, 104, 139, 138,
	    108, 143, 121, 116, 137, 147, 156, 131,
	};
	static constexpr uint8_t expected_1x1[4] = {117, 123, 130, 126};
	const size_t offset_4x4 = 8u * 8u * 4u;
	const size_t offset_2x2 = offset_4x4 + 4u * 4u * 4u;
	const size_t offset_1x1 = offset_2x2 + 2u * 2u * 4u;
	for (size_t index = 0; index < 64u; ++index) {
		if (!expect(chain.rgba[offset_4x4 + index] == expected_4x4[index],
		            "4x4 level matches weight 160 and floor-rounded source boxes")) {
			return false;
		}
	}
	for (size_t index = 0; index < 16u; ++index) {
		if (!expect(chain.rgba[offset_2x2 + index] == expected_2x2[index],
		            "2x2 terminal level is the exact floor box of 4x4 output")) {
			return false;
		}
	}
	for (size_t index = 0; index < 4u; ++index) {
		if (!expect(chain.rgba[offset_1x1 + index] == expected_1x1[index],
		            "1x1 terminal level is the exact floor box of 2x2 output")) {
			return false;
		}
	}

	FdMipChain untouched;
	untouched.width = 99;
	untouched.rgba = {7u, 8u, 9u};
	std::vector<uint8_t> non_power_of_two(3u * 4u * 4u, 42u);
	if (!expect(!runtime.build_fd_rgba_mip_chain(
	                non_power_of_two.data(), 3, 4, untouched) &&
	                untouched.width == 99 &&
	                untouched.rgba == std::vector<uint8_t>({7u, 8u, 9u}),
	            "invalid dimensions leave the output chain untouched")) {
		return false;
	}
	return true;
}

} // namespace

int main() {
	if (!detail_vectors_and_gates()) return 1;
	if (!silhouette_vectors_and_tier_role()) return 1;
	if (!tier_specific_foliage_sampler_routing()) return 1;
	if (!detail_cache_temporal_semantics()) return 1;
	if (!detail_capacity_and_eviction()) return 1;
	if (!model_cache_phase_negative_and_identity()) return 1;
	if (!overlapping_model_cells_refresh_once_per_frame()) return 1;
	if (!model_key_lookup_work_is_bounded_by_cell_visits()) return 1;
	if (!model_definition_stagger()) return 1;
	if (!model_cache_lru_and_identity_events()) return 1;
	if (!model_path_blocker_gate_and_force_on_bypass()) return 1;
	if (!per_slot_mask_bit_selection_in_both_tiers()) return 1;
	if (!negative_anchor_cell_keys_sign_extend()) return 1;
	if (!detail_cache_lru_evicts_oldest_at_capacity_three()) return 1;
	if (!invalid_anchors_and_reconfigure_stats()) return 1;
	if (!fd_bake_vector()) return 1;
	std::printf("OK: fresh foliage runtime matches retail vectors\n");
	return 0;
}
