// Literal vectors independently calculated from the recovered instructions.
#include <formats/foliage/runtime.h>

#include <algorithm>
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
	// The terrain frame's tail generates a missing key before the scene
	// core's detail passes draw, so a new key draws in its own frame.
	// [orig: Render_ProcessMainSceneFrame @ 0x5ca654 then @ 0x5ca8ec;
	// PolyTrn_RenderFrame @ 0x60f0ea..0x60f10f]
	auto out = runtime.render_frame(one_detail(10.0f), world);
	const auto near_stats = runtime.get_stats();
	if (!expect(out.detail.size() == 72 &&
	                near_stats.detail.misses == 1 &&
	                near_stats.detail.regenerations == 1 &&
	                near_stats.detail.submissions == 2,
	            "a newly collected near cell generates and draws twice in one frame")) return false;

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

	// Key 0x00100030 is the cell X [16,32), Z [48,64): the low half is the
	// Z-MIN and local B ADDS to it; the yaw step is retail's flt_7CD4DC
	// (0x38C90FD0), a hair below 2*pi/65536. Vectors from an independent
	// emulation of the instruction sequence.
	// [orig: generate_foliage_instances_0 @ 0x5ffe88..0x5fffa2]
	const struct Expected { float x, z, yaw; } expected[3] = {
	    {18.63215637f, 49.69863892f, 4.83126879f},
	    {20.66067505f, 50.26528931f, 1.83435190f},
	    {23.21640015f, 49.27233887f, 4.25919008f},
	};
	for (int i = 0; i < 3; ++i) {
		const auto &got = out.detail[static_cast<size_t>(i)];
		if (!expect(got.cell_key == 0x00100030u && got.candidate == i,
		            "detail preserves key and candidate index")) return false;
		if (!expect(near(got.center.x, expected[i].x) &&
		                near(got.center.z, expected[i].z) &&
		                near(got.yaw_radians, expected[i].yaw, 1.0e-6f),
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
	auto flat = one_detail(10.0f);
	flat.detail_cells[0].key |= 0x80000000u;
	if (!expect(runtime.render_frame(flat, world).detail.empty(),
	            "flat-sector detail keys generate no geometry")) return false;
	return true;
}

// The thermal view (the scene core's fourth argument, the held weapon's
// thermal byte) forces every detail patch onto the primary LOW pass and
// scales its c6 fade by flt_7C69F4 = 0.1: no HIGH, no strict-LESS secondary.
// [orig: Foliage_RenderFarPatches @ 0x60a193..0x60a19c (forced LOW),
// @ 0x60a497..0x60a4ae (x0.1); Render_ProcessMainSceneFrame
// @ 0x5ca2da..0x5ca2e3, 0x5ca8e3]
bool detail_thermal_view_forces_low_at_a_tenth_fade() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	auto near_request = one_detail(10.0f);
	near_request.thermal_view = true;
	const auto near_out = runtime.render_frame(near_request, world);
	if (!expect(near_out.detail.size() == 36 &&
	                runtime.get_stats().detail.submissions == 1,
	            "a thermal near patch submits once")) return false;
	for (const DetailInstance &instance : near_out.detail) {
		if (!expect(instance.pass == DetailPass::LowAlphaTest &&
		                instance.alpha_reference == 8 &&
		                !instance.near_secondary &&
		                near(instance.alpha, 0.1f, 1.0e-6f),
		            "thermal near patch is the primary LOW pass at fade 0.1")) {
			return false;
		}
	}
	auto fading = one_detail(31.0f);
	fading.thermal_view = true;
	const auto fading_out = runtime.render_frame(fading, world);
	if (!expect(fading_out.detail.size() == 36 &&
	                fading_out.detail[0].pass == DetailPass::LowAlphaTest &&
	                near(fading_out.detail[0].alpha, 0.05f, 1.0e-6f),
	            "the thermal scale multiplies the distance fade")) return false;
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

	// The anchor at Godot (32, 48) is mission (x, y) = (32, -48): quadrant
	// bit 1 steps mission y, and the low half keys the mission-y cell TOP.
	// [orig: Foliage_UpdateModelTiles @ 0x601fd0..0x60205b]
	const uint32_t quadrant_keys[4] = {
	    0x00207FE0u, 0x00107FE0u, 0x00207FD0u, 0x00107FD0u,
	};
	const int quadrant_counts[4] = {1, 3, 2, 3};
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

	// Mission candidate (keyHi + A, keyLo - B), Godot z = B - keyLo.
	// [orig: Foliage_GenerateModelTileInstances @ 0x600af5..0x600b0c]
	const auto &first = out.silhouettes[0];
	if (!expect(first.cell_key == 0x00207FE0u &&
	                first.quadrant == 0 && first.candidate == 30,
	            "silhouette retains quadrant/key/candidate identity")) return false;
	if (!expect(near(first.center.x, 34.00364685f) &&
	                near(first.center.z, 46.20242310f) &&
	                near(first.yaw_radians, 1.53810215f, 1.0e-6f),
	            "silhouette placement matches the PRNG literal")) return false;

	// Corners (keyHi + A', keyLo - B') in the mission frame, sampled on the
	// Godot plane. [orig: Foliage_GenerateModelTileInstances
	// @ 0x600bd0..0x600c6a]
	const GroundCorner expected_corners[4] = {
	    {35.45381165f, 44.65419006f, 86.08673096f},
	    {35.55187988f, 47.65258789f, 91.14562988f},
	    {32.45541382f, 44.75225830f, 78.07762146f},
	    {32.55348206f, 47.75065613f, 83.05718231f},
	};
	for (int i = 0; i < 4; ++i) {
		if (!expect(near(first.corners[i].x, expected_corners[i].x) &&
		                near(first.corners[i].z, expected_corners[i].z) &&
		                near(first.corners[i].height,
		                     expected_corners[i].height, 2.0e-4f),
		            "silhouette corner vector matches four ground samples")) return false;
	}
	const float expected_fold[4] = {
	    -0.06971931f, 0.00000572f, -0.03578377f, -0.00000191f,
	};
	for (int i = 0; i < 4; ++i) {
		if (!expect(near(first.fold[i], expected_fold[i], 2.0e-4f),
		            "silhouette fold matches four midpoint controls")) return false;
	}
	if (!expect(near(first.center_height, 84.48628807f, 2.0e-4f),
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
	if (!expect(first.detail.size() == 144 &&
	                first_stats.detail.misses == 2 &&
	                first_stats.detail.regenerations == 1 &&
	                first_stats.detail.residents == 1 &&
	                first_stats.detail.submissions == 4 &&
	                first.detail_generated.size() == 1,
	            "duplicate detail misses allocate one resident both keys draw at once")) return false;
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
	if (!expect(rewarmed.detail.size() == 144 &&
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
	if (!expect(second.detail.size() == 72 &&
	                second.detail[0].cache_revision ==
	                    second.detail_generated[0].revision &&
	                second.detail_evicted.size() == 1 &&
	                second.detail_generated.size() == 1 &&
	                second.detail_evicted[0].revision ==
	                    first.detail_generated[0].revision &&
	                runtime.get_stats().detail.evictions == 1,
	            "detail replacement emits old and new identities and draws the new "
	            "geometry in its generation frame")) return false;
	const auto third = runtime.render_frame(second_request, world);
	if (!expect(third.detail.size() == 72 &&
	                third.detail[0].cache_revision ==
	                    second.detail_generated[0].revision,
	            "the replacement resident keeps its revision on the next hit")) return false;
	return true;
}

bool flat_detail_keys_retain_empty_cache_entries() {
	Runtime runtime;
	auto request = one_detail(10.0f);
	request.slots[0].source_vertex_count = 1820; // One resident slot.
	auto world = world_with_foliage_mask(0x1u);
	const uint32_t authored_key = request.detail_cells[0].key;
	runtime.render_frame(request, world);
	if (!expect(runtime.render_frame(request, world).detail.size() == 72,
	            "the authored control warms a drawable resident")) return false;

	int samples = 0;
	world.detail_foliage_mask_at = [&samples](int32_t, int32_t) { ++samples; return 1u; };
	world.height_at = [&samples](float, float) { ++samples; return 0.0f; };
	world.path_blocked = [&samples](float, float, float) { ++samples; return false; };
	request.detail_cells[0].key |= 0x80000000u;
	const uint32_t flat_key = request.detail_cells[0].key;
	const auto flat = runtime.render_frame(request, world);
	if (!expect(flat.detail.empty() && flat.detail_evicted.size() == 1 &&
	                flat.detail_evicted[0].key == authored_key &&
	                flat.detail_generated.size() == 1 &&
	                flat.detail_generated[0].key == flat_key &&
	                runtime.get_stats().detail.residents == 1 && samples == 0,
	            "a flat key evicts the authored resident and generates an empty unsampled entry")) return false;
	const auto repeat = runtime.render_frame(request, world);
	if (!expect(repeat.detail.empty() && repeat.detail_generated.empty() &&
	                repeat.detail_evicted.empty() && runtime.get_stats().detail.hits == 1 &&
	                runtime.get_stats().detail.regenerations == 0 && samples == 0,
	            "a repeated flat key hits its resident without sampling or drawing")) return false;

	request.detail_cells[0].key = authored_key;
	const auto restored = runtime.render_frame(request, world);
	if (!expect(restored.detail.size() == 72 &&
	                restored.detail_evicted.size() == 1 &&
	                restored.detail_evicted[0].key == flat_key &&
	                restored.detail_generated.size() == 1 && samples > 0,
	            "returning to authored terrain evicts the empty flat resident, "
	            "regenerates and draws in that frame")) return false;
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
	// On the phase every visit regenerates: both anchors' four visits.
	// [orig: Foliage_UpdateModelTiles @ 0x602085..0x6020aa]
	const auto phase_eight = runtime.render_frame(request, world);
	if (!expect(phase_eight.silhouettes.empty() &&
	                runtime.get_stats().model.hits == 8 &&
	                runtime.get_stats().model.regenerations == 8 &&
	                phase_eight.model_evicted.size() == 4 &&
	                phase_eight.model_generated.size() == 4,
	            "every visit regenerates on definition zero's phase")) return false;

	foliage_mask = 0x1u;
	if (!expect(runtime.render_frame(request, world).silhouettes.empty() &&
	                runtime.get_stats().model.regenerations == 0,
	            "empty model result is negative-cached off phase")) return false;
	for (int frame = 10; frame <= 15; ++frame) {
		runtime.render_frame(request, world);
	}
	const auto phase_sixteen = runtime.render_frame(request, world);
	if (!expect(phase_sixteen.silhouettes.size() == 18 &&
	                runtime.get_stats().model.regenerations == 8,
	            "negative model entries recover on every visit of the next phase")) return false;
	return true;
}

// Two anchors 8 units apart share two cells. On the definition's phase each
// visit regenerates the shared resident around ITS OWN anchor and draws it at
// once, so the second anchor's submission is placed around the second anchor;
// off the phase both visits draw the resident as the last visit left it.
// [orig: Foliage_UpdateModelTiles @ 0x601f50, hit touch/phase/regenerate
// @ 0x60208b..0x6020aa, the immediate draw @ 0x6021a5;
// Foliage_GenerateModelTileInstances @ 0x600b11..0x600b45 (+-4 box)]
bool overlapping_model_cells_regenerate_per_visit_on_phase() {
	Runtime runtime;
	auto world = world_with_foliage_mask(0x1u);
	auto request = one_silhouette(38.0f, 63.0f);
	request.silhouette_anchors.push_back({{40.0f, 48.0f}, 38.0f, 63.0f});

	runtime.render_frame(request, world);
	if (!expect(runtime.get_stats().model.misses == 6 &&
	                runtime.get_stats().model.hits == 2,
	            "the second anchor hits the two cells the first one warmed")) {
		return false;
	}
	for (int frame = 2; frame <= 7; ++frame) {
		runtime.render_frame(request, world);
	}

	const auto near_anchor = [](const FrameOutput &output, uint32_t key,
	                            float anchor_x, int visit) {
		std::vector<uint64_t> submissions;
		for (const SilhouetteInstance &instance : output.silhouettes) {
			if (instance.cell_key == key &&
			    std::find(submissions.begin(), submissions.end(),
			              instance.submission_id) == submissions.end()) {
				submissions.push_back(instance.submission_id);
			}
		}
		if (static_cast<int>(submissions.size()) <= visit) return false;
		int count = 0;
		for (const SilhouetteInstance &instance : output.silhouettes) {
			if (instance.submission_id != submissions[static_cast<size_t>(visit)]) continue;
			if (std::fabs(instance.center.x - anchor_x) > 4.0f) return false;
			++count;
		}
		return count > 0;
	};
	const uint32_t shared_key = 0x00207FE0u;
	const auto phase_eight = runtime.render_frame(request, world);
	if (!expect(runtime.get_stats().model.hits == 8 &&
	                runtime.get_stats().model.regenerations == 8,
	            "every on-phase visit regenerates, shared cells included")) {
		return false;
	}
	if (!expect(near_anchor(phase_eight, shared_key, 32.0f, 0) &&
	                near_anchor(phase_eight, shared_key, 40.0f, 1),
	            "each on-phase visit draws the shared cell around its own anchor")) {
		return false;
	}
	const auto after = runtime.render_frame(request, world);
	if (!expect(runtime.get_stats().model.regenerations == 0 &&
	                near_anchor(after, shared_key, 40.0f, 0) &&
	                near_anchor(after, shared_key, 40.0f, 1),
	            "off the phase both visits draw the last visit's regeneration")) {
		return false;
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
	// packed key's 15-bit halves sign-extend back to their bases, with
	// X = high15 + localA and MISSION y = low15 - localB (Godot z = -y). The
	// anchor at Godot z = -48 is mission y = +48, so its y keys are positive.
	// [orig: Foliage_UpdateModelTiles @ 0x601fd0..0x60205b — quadrant snap /
	// key form; Foliage_GenerateModelTileInstances @ 0x600986..0x6009aa —
	// 15-bit decode]
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

	// Hand-derived quadrant keys for mission anchor (-32, +48): snap(anchor
	// +- 8u) masked to 16u tiles gives x in {-32, -48}, y-top in {64, 48}.
	const uint32_t quadrant_keys[4] = {
	    0x7FE00040u, 0x7FD00040u, 0x7FE00030u, 0x7FD00030u,
	};
	const int quadrant_counts[4] = {3, 2, 1, 2};

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
		const int32_t base_y = sext15(instance.cell_key);
		if (!expect(base_x < 0 && base_y > 0,
		            "the anchor keys decode to a negative x and positive y top")) {
			return false;
		}
		const float local_a = instance.center.x - static_cast<float>(base_x);
		const float mission_y = -instance.center.z;
		const float local_b = static_cast<float>(base_y) - mission_y;
		if (!expect(local_a >= 1.0f - 1.0e-4f && local_a <= 15.8f + 1.0e-4f,
		            "X decodes as high15 + localA for negative keys")) {
			return false;
		}
		if (!expect(local_b >= 1.0f - 1.0e-4f && local_b <= 15.8f + 1.0e-4f,
		            "mission y decodes as low15 - localB")) {
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
	if (!detail_thermal_view_forces_low_at_a_tenth_fade()) return 1;
	if (!silhouette_vectors_and_tier_role()) return 1;
	if (!tier_specific_foliage_sampler_routing()) return 1;
	if (!detail_cache_temporal_semantics()) return 1;
	if (!detail_capacity_and_eviction()) return 1;
	if (!flat_detail_keys_retain_empty_cache_entries()) return 1;
	if (!model_cache_phase_negative_and_identity()) return 1;
	if (!overlapping_model_cells_regenerate_per_visit_on_phase()) return 1;
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
