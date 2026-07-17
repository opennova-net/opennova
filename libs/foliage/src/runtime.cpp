#include "foliage/runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace opennova::foliage {

namespace {

constexpr uint32_t kSeedConstant = 0xA55B1EEDu;
constexpr int kGridWidth = 6;
constexpr int kCandidates = 36;
constexpr int kSilhouetteCellCap = 21;
constexpr float kGridStep = 2.6f;
constexpr float kGridBase = 1.0f;
constexpr float kJitterScale = 1.8f / 65536.0f;
constexpr float kYawScale = 6.28318530717958647692f / 65536.0f;
constexpr float kPathRange = 2.0f;
constexpr float kDetailFadeStart = 20.0f;
constexpr float kDetailPassSwitch = 33.0f;
// The witnessed 0.1 c6 fade scale (flt_7C69F4 @ 0x60a4a8) belongs to the
// water-REFLECTION scene invocation only (arg_8 = reflectionEnabled), which
// also forces every patch to the LOW pass. The main scene never scales the
// fade; this runtime models the main scene.
constexpr float kDetailLimit = 42.0f;
constexpr float kSilhouetteDepth = 38.0f;
constexpr int32_t kFixedOne = 0x10000;
constexpr int32_t kAnchorRadiusFixed = 0x40000;
constexpr int32_t kQuadrantOffsetFixed = 0x80000;
constexpr int32_t kTileSizeFixed = 0x100000;
constexpr uint32_t kTileSnapMask = 0xFFF00000u;

uint32_t rol32(uint32_t value, uint32_t amount) {
	const uint32_t n = amount & 31u;
	if (n == 0u) return value;
	return (value << n) | (value >> (32u - n));
}

uint32_t seed_for_key(uint32_t key) {
	// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
	// Foliage_GenerateModelTileInstances @ 0x600980]
	return (key & 0x01FF01FFu) + rol32(kSeedConstant, key & 31u);
}

uint16_t next_draw(uint32_t &state) {
	state = rol32(state + rol32(state, 11u), 4u) ^ 1u;
	return static_cast<uint16_t>(state);
}

int32_t sign_extend_15(uint32_t value) {
	const uint32_t low = value & 0x7FFFu;
	return (low & 0x4000u) != 0u
	         ? static_cast<int32_t>(low | 0xFFFF8000u)
	         : static_cast<int32_t>(low);
}

int32_t to_fixed(float value) {
	return static_cast<int32_t>(value * static_cast<float>(kFixedOne));
}

bool try_to_fixed(float value, int32_t &result) {
	if (!std::isfinite(value)) return false;
	const double scaled =
	    static_cast<double>(value) * static_cast<double>(kFixedOne);
	if (scaled < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
	    scaled > static_cast<double>(std::numeric_limits<int32_t>::max())) {
		return false;
	}
	result = static_cast<int32_t>(scaled);
	return true;
}

float from_fixed(int32_t value) {
	return static_cast<float>(value) / static_cast<float>(kFixedOne);
}

int32_t midpoint_fixed(int32_t a, int32_t b) {
	const int64_t sum = static_cast<int64_t>(a) + static_cast<int64_t>(b);
	if (sum >= 0) return static_cast<int32_t>(sum / 2);
	return static_cast<int32_t>(-((-sum + 1) / 2));
}

struct Candidate {
	uint8_t index = 0;
	float local_a = 0.0f;
	float local_b = 0.0f;
	float yaw = 0.0f;
	int32_t x_fixed = 0;
	int32_t z_fixed = 0;
};

Candidate next_candidate(uint32_t key, int index, uint32_t &state) {
	// Both retail generators decode HIGH15 as X and LOW15 as the Z-top base;
	// the local B axis runs toward decreasing world/Godot Z.
	// [orig: generate_foliage_instances_0 @ 0x5ffdd0, 0x5fff84..0x600029;
	// Foliage_GenerateModelTileInstances @ 0x600980]
	const uint16_t draw_a = next_draw(state);
	const uint16_t draw_b = next_draw(state);
	const uint16_t draw_yaw = next_draw(state);
	const float local_a = kGridBase +
	                      static_cast<float>(index % kGridWidth) * kGridStep +
	                      static_cast<float>(draw_a) * kJitterScale;
	const float local_b = kGridBase +
	                      static_cast<float>(index / kGridWidth) * kGridStep +
	                      static_cast<float>(draw_b) * kJitterScale;
	const int32_t base_x = sign_extend_15(key >> 16u);
	const int32_t base_z = sign_extend_15(key);
	Candidate candidate;
	candidate.index = static_cast<uint8_t>(index);
	candidate.local_a = local_a;
	candidate.local_b = local_b;
	candidate.yaw = static_cast<float>(draw_yaw) * kYawScale;
	candidate.x_fixed = to_fixed(static_cast<float>(base_x) + local_a);
	candidate.z_fixed = to_fixed(static_cast<float>(base_z) - local_b);
	return candidate;
}

uint32_t pack_silhouette_key(int32_t snap_x, int32_t snap_z) {
	const uint32_t x_part = static_cast<uint32_t>(snap_x) & 0x7FFF0000u;
	const uint32_t z_part =
	    ((static_cast<uint32_t>(snap_z) + static_cast<uint32_t>(kTileSizeFixed)) >> 16u) &
	    0x7FFFu;
	return x_part | z_part;
}

struct SilhouetteCell {
	uint32_t key = 0;
	uint8_t quadrant = 0;
};

std::array<SilhouetteCell, 4> silhouette_cells(int32_t anchor_x,
                                               int32_t anchor_z) {
	std::array<SilhouetteCell, 4> result{};
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		const int32_t x_offset =
		    (quadrant & 1) != 0 ? -kQuadrantOffsetFixed : kQuadrantOffsetFixed;
		const int32_t z_offset =
		    (quadrant & 2) != 0 ? -kQuadrantOffsetFixed : kQuadrantOffsetFixed;
		// Retail integer addition wraps in 32 bits. Perform it unsigned so
		// public coordinates near the fixed-point limits cannot trigger C++
		// signed-overflow UB before the tile mask is applied.
		const int32_t snap_x = static_cast<int32_t>(
		    (static_cast<uint32_t>(anchor_x) +
		     static_cast<uint32_t>(x_offset)) &
		    kTileSnapMask);
		const int32_t snap_z = static_cast<int32_t>(
		    (static_cast<uint32_t>(anchor_z) +
		     static_cast<uint32_t>(z_offset)) &
		    kTileSnapMask);
		result[quadrant].key = pack_silhouette_key(snap_x, snap_z);
		result[quadrant].quadrant = static_cast<uint8_t>(quadrant);
	}
	return result;
}

float sampled_height(const WorldSamplers &world, int32_t x, int32_t z) {
	return world.height_at ? world.height_at(from_fixed(x), from_fixed(z)) : 0.0f;
}

bool candidate_is_blocked(const RuntimeSlot &slot,
                          const WorldSamplers &world,
                          const Candidate &candidate) {
	if ((slot.attrib_flags & FOLIAGE_ATTRIB_FORCE_ON) != 0u) return false;
	return world.path_blocked &&
	       world.path_blocked(from_fixed(candidate.x_fixed),
	                          from_fixed(candidate.z_fixed),
	                          kPathRange);
}

uint8_t silhouette_alpha_reference(float camera_distance) {
	if (!std::isfinite(camera_distance) || camera_distance >= 511.0f) {
		return 8;
	}
	const int distance_units =
	    static_cast<int>(std::max(0.0f, camera_distance));
	const int value = static_cast<int>(4096.0f /
	                                   static_cast<float>(distance_units + 1));
	return static_cast<uint8_t>(std::clamp(value, 8, 128));
}

std::vector<DetailInstance> generate_detail_cell(
    int slot_index,
    const RuntimeSlot &slot,
    uint32_t cell_key,
    const WorldSamplers &world) {
	// The detail tier expands the def model's full source geometry for every
	// accepted transform. This module returns the transforms; the render
	// adapter samples terrain height per transformed source vertex.
	// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
	// Terrain_CollectNearFoliagePatches @ 0x603e60]
	std::vector<DetailInstance> result;
	uint32_t state = seed_for_key(cell_key);
	for (int index = 0; index < kCandidates; ++index) {
		const Candidate candidate = next_candidate(cell_key, index, state);
		if (candidate_is_blocked(slot, world, candidate)) continue;
		const uint32_t mask = world.detail_foliage_mask_at
		                        ? world.detail_foliage_mask_at(
		                              candidate.x_fixed,
		                              candidate.z_fixed)
		                        : 0u;
		if ((mask & (1u << slot_index)) == 0u) continue;

		DetailInstance instance;
		instance.slot = static_cast<uint8_t>(slot_index);
		instance.candidate = candidate.index;
		instance.cell_key = cell_key;
		instance.center = {
		    from_fixed(candidate.x_fixed),
		    from_fixed(candidate.z_fixed),
		};
		instance.yaw_radians = candidate.yaw;
		result.push_back(instance);
	}
	return result;
}

bool make_silhouette_instance(
    int slot_index,
    uint8_t quadrant,
    uint32_t cell_key,
    const Candidate &candidate,
    float footprint,
    uint8_t alpha_reference,
    const WorldSamplers &world,
    SilhouetteInstance &result) {
	// Four rotated corners plus the four edge midpoints feed the retail
	// GridPlacementVS constants. [orig: Foliage_GenerateModelTileInstances
	// @ 0x600980; Foliage_UploadModelTileVSConstants @ 0x600f00]
	SilhouetteInstance instance;
	instance.slot = static_cast<uint8_t>(slot_index);
	instance.candidate = candidate.index;
	instance.quadrant = quadrant;
	instance.cell_key = cell_key;
	instance.center = {
	    from_fixed(candidate.x_fixed),
	    from_fixed(candidate.z_fixed),
	};
	instance.yaw_radians = candidate.yaw;
	instance.alpha_reference = alpha_reference;

	const float cosine = std::cos(candidate.yaw);
	const float sine = std::sin(candidate.yaw);
	const int32_t base_x = sign_extend_15(cell_key >> 16u);
	const int32_t base_z = sign_extend_15(cell_key);
	std::array<int32_t, 4> corner_x{};
	std::array<int32_t, 4> corner_z{};

	for (int corner = 0; corner < 4; ++corner) {
		const float local_a = (corner & 1) != 0 ? footprint : -footprint;
		const float local_b = (corner & 2) != 0 ? footprint : -footprint;
		const float rotated_a = candidate.local_a +
		                        local_a * cosine - local_b * sine;
		const float rotated_b = candidate.local_b +
		                        local_a * sine + local_b * cosine;
		if (!try_to_fixed(static_cast<float>(base_x) + rotated_a,
		                  corner_x[corner]) ||
		    !try_to_fixed(static_cast<float>(base_z) - rotated_b,
		                  corner_z[corner])) {
			return false;
		}
		instance.corners[corner] = {
		    from_fixed(corner_x[corner]),
		    from_fixed(corner_z[corner]),
		    sampled_height(world, corner_x[corner], corner_z[corner]),
		};
	}

	const int edge_pairs[4][2] = {
	    {0, 2}, {1, 3}, {0, 1}, {2, 3},
	};
	std::array<float, 4> midpoint_height{};
	for (int edge = 0; edge < 4; ++edge) {
		const int first = edge_pairs[edge][0];
		const int second = edge_pairs[edge][1];
		midpoint_height[edge] = sampled_height(
		    world,
		    midpoint_fixed(corner_x[first], corner_x[second]),
		    midpoint_fixed(corner_z[first], corner_z[second]));
	}

	const float d_minus_a =
	    midpoint_height[0] -
	    (instance.corners[0].height + instance.corners[2].height) * 0.5f;
	const float d_plus_a =
	    midpoint_height[1] -
	    (instance.corners[1].height + instance.corners[3].height) * 0.5f;
	const float d_minus_b =
	    midpoint_height[2] -
	    (instance.corners[0].height + instance.corners[1].height) * 0.5f;
	const float d_plus_b =
	    midpoint_height[3] -
	    (instance.corners[2].height + instance.corners[3].height) * 0.5f;
	instance.fold[0] = (d_minus_a + d_plus_a) * 0.5f;
	instance.fold[1] = d_plus_a - instance.fold[0];
	instance.fold[2] = (d_minus_b + d_plus_b) * 0.5f;
	instance.fold[3] = d_plus_b - instance.fold[2];
	instance.center_height =
	    (instance.corners[0].height + instance.corners[1].height +
	     instance.corners[2].height + instance.corners[3].height) *
	        0.25f +
	    instance.fold[0] + instance.fold[2];
	result = instance;
	return true;
}

std::vector<SilhouetteInstance> generate_silhouette_cell(
    int slot_index,
    const RuntimeSlot &slot,
    const SilhouetteCell &cell,
    int32_t anchor_x,
    int32_t anchor_z,
	const WorldSamplers &world) {
	std::vector<SilhouetteInstance> result;
	if (!std::isfinite(slot.model_radius) || slot.model_radius < 0.0f) {
		return result;
	}
	const float footprint = slot.model_radius * 0.75f;
	uint32_t state = seed_for_key(cell.key);
	for (int index = 0; index < kCandidates; ++index) {
		const Candidate candidate = next_candidate(cell.key, index, state);
		const int64_t dx =
		    static_cast<int64_t>(candidate.x_fixed) - anchor_x;
		const int64_t dz =
		    static_cast<int64_t>(candidate.z_fixed) - anchor_z;
		if (std::llabs(dx) > kAnchorRadiusFixed ||
		    std::llabs(dz) > kAnchorRadiusFixed) {
			continue;
		}
		if (candidate_is_blocked(slot, world, candidate)) continue;
		const uint32_t mask = world.model_foliage_mask_at
		                        ? world.model_foliage_mask_at(
		                              candidate.x_fixed,
		                              candidate.z_fixed)
		                        : 0u;
		if ((mask & (1u << slot_index)) == 0u) continue;

		SilhouetteInstance instance;
		if (!make_silhouette_instance(slot_index,
		                              cell.quadrant,
		                              cell.key,
		                              candidate,
		                              footprint,
		                              0u,
		                              world,
		                              instance)) {
			continue;
		}
		result.push_back(instance);
		if (static_cast<int>(result.size()) >= kSilhouetteCellCap) break;
	}
	return result;
}

bool same_silhouette_geometry(
    const std::vector<SilhouetteInstance> &first,
    const std::vector<SilhouetteInstance> &second) {
	if (first.size() != second.size()) return false;
	for (size_t index = 0; index < first.size(); ++index) {
		const SilhouetteInstance &a = first[index];
		const SilhouetteInstance &b = second[index];
		if (a.slot != b.slot ||
		    a.candidate != b.candidate ||
		    a.quadrant != b.quadrant ||
		    a.cell_key != b.cell_key ||
		    a.center.x != b.center.x ||
		    a.center.z != b.center.z ||
		    a.yaw_radians != b.yaw_radians ||
		    a.center_height != b.center_height) {
			return false;
		}
		for (size_t corner = 0; corner < a.corners.size(); ++corner) {
			const GroundCorner &ac = a.corners[corner];
			const GroundCorner &bc = b.corners[corner];
			if (ac.x != bc.x || ac.z != bc.z || ac.height != bc.height) {
				return false;
			}
		}
		for (size_t fold = 0; fold < a.fold.size(); ++fold) {
			if (a.fold[fold] != b.fold[fold]) return false;
		}
	}
	return true;
}

} // namespace

size_t Runtime::detail_cache_capacity(uint32_t source_vertex_count) {
	constexpr uint64_t kMaxSlots = 128u;
	constexpr uint64_t kIndexLimitExclusive = 0xFFFFu;
	const uint64_t vertices_per_cell =
	    36u * static_cast<uint64_t>(source_vertex_count);
	if (vertices_per_cell == 0u ||
	    vertices_per_cell >= kIndexLimitExclusive) {
		return 0u;
	}
	return static_cast<size_t>(std::min<uint64_t>(
	    kMaxSlots, (kIndexLimitExclusive - 1u) / vertices_per_cell));
}

void Runtime::reset() {
	for (auto &entries : detail_cache_) {
		entries.clear();
	}
	for (auto &entries : model_cache_) {
		for (ModelCacheEntry &entry : entries) {
			entry = ModelCacheEntry{};
		}
	}
	for (auto &index : model_cache_index_) {
		index.clear();
	}
	cached_slots_ = {};
	slot_configured_ = {};
	terrain_scene_counter_ = 0u;
	stats_ = RuntimeStats{};
}

FrameOutput Runtime::render_frame(const FrameRequest &request,
                                  const WorldSamplers &world) {
	FrameOutput output;
	stats_ = RuntimeStats{};
	// Slot changes invalidate only that definition. Runtime::reset remains the
	// explicit invalidation boundary for terrain/map/sampler changes.
	for (int slot_index = 0; slot_index < FOLIAGE_MAX_DEFS; ++slot_index) {
		const RuntimeSlot &slot = request.slots[slot_index];
		const RuntimeSlot &cached = cached_slots_[slot_index];
		const size_t detail_capacity =
		    slot.enabled ? detail_cache_capacity(slot.source_vertex_count) : 0u;
		const bool unchanged =
		    slot_configured_[slot_index] &&
		    cached.enabled == slot.enabled &&
		    cached.attrib_flags == slot.attrib_flags &&
		    cached.model_radius == slot.model_radius &&
		    cached.source_vertex_count == slot.source_vertex_count &&
		    detail_cache_[slot_index].size() == detail_capacity;
		if (unchanged) continue;

		for (const DetailCacheEntry &entry : detail_cache_[slot_index]) {
			if (entry.valid) {
				++stats_.detail.evictions;
				output.detail_evicted.push_back(CacheIdentity{
				    static_cast<uint8_t>(slot_index), entry.key, entry.revision});
			}
		}
		for (const ModelCacheEntry &entry : model_cache_[slot_index]) {
			if (entry.valid) {
				++stats_.model.evictions;
				output.model_evicted.push_back(CacheIdentity{
				    static_cast<uint8_t>(slot_index), entry.key, entry.revision});
			}
		}
		detail_cache_[slot_index].clear();
		detail_cache_[slot_index].resize(detail_capacity);
		model_cache_index_[slot_index].clear();
		if (slot.enabled) {
			model_cache_index_[slot_index].reserve(kModelCacheCapacity);
		}
		for (ModelCacheEntry &entry : model_cache_[slot_index]) {
			entry = ModelCacheEntry{};
		}
		cached_slots_[slot_index] = slot;
		slot_configured_[slot_index] = true;
	}

	++terrain_scene_counter_;
	stats_.terrain_scene_counter = terrain_scene_counter_;

	const auto detail_cell_is_visible = [](const DetailCell &cell) {
		return (cell.key & 0x80000000u) == 0u &&
		       cell.camera_distance <= kDetailLimit;
	};
	const auto find_detail_index = [](const auto &entries, uint32_t key) {
		for (size_t index = 0; index < entries.size(); ++index) {
			if (entries[index].valid && entries[index].key == key) return index;
		}
		return entries.size();
	};
	const auto lru_index = [this](const auto &entries) {
		int32_t best_age = -1;
		size_t best_index = entries.size();
		for (size_t index = 0; index < entries.size(); ++index) {
			const int32_t age = static_cast<int32_t>(
			    terrain_scene_counter_ - entries[index].last_use);
			if (age > best_age) {
				best_age = age;
				best_index = index;
			}
		}
		return best_index;
	};

	// Detail geometry is consumed before the cache update. A miss generated at
	// the update tail therefore first appears on the next terrain render.
	// [orig: PolyTrn_RenderFrame @ 0x60f0ea..0x60f10f]
	for (const DetailCell &cell : request.detail_cells) {
		if (!detail_cell_is_visible(cell)) continue;
		const float alpha = cell.camera_distance <= kDetailFadeStart
		                      ? 1.0f
		                      : std::clamp(
		                            1.0f -
		                                (cell.camera_distance - kDetailFadeStart) /
		                                    (kDetailLimit - kDetailFadeStart),
		                            0.0f,
		                            1.0f);

		for (int slot_index = 0; slot_index < FOLIAGE_MAX_DEFS; ++slot_index) {
			if (!request.slots[slot_index].enabled) continue;
			const auto &entries = detail_cache_[slot_index];
			const size_t index = find_detail_index(entries, cell.key);
			if (index == entries.size() || entries[index].instances.empty()) {
				continue;
			}
			const DetailCacheEntry &entry = entries[index];
			const auto append_submission =
			    [this, &entry, &output](DetailPass pass,
			                           uint8_t alpha_reference,
			                           float submission_alpha,
			                           bool near_secondary) {
				++stats_.detail.submissions;
				const uint64_t submission_id = ++next_submission_id_;
				for (const DetailInstance &cached_instance : entry.instances) {
					DetailInstance instance = cached_instance;
					instance.cache_revision = entry.revision;
					instance.submission_id = submission_id;
					instance.alpha = submission_alpha;
					instance.alpha_reference = alpha_reference;
					instance.pass = pass;
					instance.near_secondary = near_secondary;
					output.detail.push_back(instance);
				}
			};

			// Retail submits the near resident twice in one main-scene pass:
			// the 180-reference depth-writing HIGH draw first, then the exact
			// same cached geometry under the 8-reference no-depth-write LOW
			// draw at the SAME c6 fade with strict D3DCMP_LESS. At and beyond
			// the 33-unit switch only the primary LOW pass is submitted. The
			// 0.1 fade scale rides the whole-call reflection flag (arg_8 =
			// reflectionEnabled, pushed at 0x5c95c1/0x5c9661), which also
			// forces LOW for every patch; it never applies to the main scene.
			// [orig: Foliage_RenderFarPatches @ 0x60a171..0x60a19c pass
			// select, 0x60a497..0x60a4ae reflection fade scale,
			// 0x60a659..0x60a694 secondary setup/draw;
			// Terrain_RenderSceneWithReflection @ 0x5c95c5/0x5c9665]
			if (cell.camera_distance < kDetailPassSwitch) {
				append_submission(DetailPass::HighAlphaTest, 180u, alpha, false);
				append_submission(DetailPass::LowAlphaTest, 8u, alpha, true);
			} else {
				append_submission(DetailPass::LowAlphaTest, 8u, alpha, false);
			}
		}
	}

	// Detail update: resident lookup stops at the first key; duplicate missing
	// keys allocate once. Geometry persists until strict signed-age LRU reuse.
	// [orig: Foliage_UpdateFarCellSlots @ 0x601b30]
	for (int slot_index = 0; slot_index < FOLIAGE_MAX_DEFS; ++slot_index) {
		const RuntimeSlot &slot = request.slots[slot_index];
		auto &entries = detail_cache_[slot_index];
		if (!slot.enabled || entries.empty()) continue;

		std::vector<uint32_t> pending_keys;
		pending_keys.reserve(request.detail_cells.size());
		for (const DetailCell &cell : request.detail_cells) {
			if (!detail_cell_is_visible(cell)) continue;
			const size_t resident_index = find_detail_index(entries, cell.key);
			if (resident_index != entries.size()) {
				++stats_.detail.hits;
				entries[resident_index].last_use = terrain_scene_counter_;
				continue;
			}

			++stats_.detail.misses;
			if (std::find(pending_keys.begin(), pending_keys.end(), cell.key) ==
			    pending_keys.end()) {
				pending_keys.push_back(cell.key);
			}
		}

		for (uint32_t key : pending_keys) {
			const size_t index = lru_index(entries);
			if (index == entries.size()) continue;
			DetailCacheEntry &entry = entries[index];
			if (entry.valid) {
				++stats_.detail.evictions;
				output.detail_evicted.push_back(CacheIdentity{
				    static_cast<uint8_t>(slot_index), entry.key, entry.revision});
			}
			entry.valid = true;
			entry.key = key;
			entry.last_use = terrain_scene_counter_;
			entry.revision = ++next_cache_revision_;
			entry.instances =
			    generate_detail_cell(slot_index, slot, key, world);
			++stats_.detail.regenerations;
			output.detail_generated.push_back(CacheIdentity{
			    static_cast<uint8_t>(slot_index), entry.key, entry.revision});
		}
	}

	// Distant model cache: one fixed 1000-entry pool per definition. Hits touch
	// the resident entry on the definition's exact eight-scene refresh phase.
	//
	// Nearby anchors (clustered crouched/prone infantry) can visit one cell
	// several times in a frame. Refresh a resident key only on its first visit
	// this scene frame; later callers retain their distinct draw submissions
	// while reusing that refreshed resident. Retail's visible
	// sector-entity/occlusion walk bounds the same fanout (D-FOLIAGE-9).
	// [orig: Foliage_UpdateModelTiles @ 0x601f50]
	std::array<std::unordered_set<uint32_t>, FOLIAGE_MAX_DEFS>
	    refreshed_model_keys;
	for (const SilhouetteAnchor &anchor : request.silhouette_anchors) {
		if (!std::isfinite(anchor.position.x) ||
		    !std::isfinite(anchor.position.z) ||
		    !std::isfinite(anchor.view_depth) ||
		    !std::isfinite(anchor.camera_distance)) {
			continue;
		}
		if (anchor.view_depth < kSilhouetteDepth) continue;
		int32_t anchor_x = 0;
		int32_t anchor_z = 0;
		if (!try_to_fixed(anchor.position.x, anchor_x) ||
		    !try_to_fixed(anchor.position.z, anchor_z)) {
			continue;
		}
		const auto cells = silhouette_cells(anchor_x, anchor_z);
		const uint8_t alpha_reference =
		    silhouette_alpha_reference(anchor.camera_distance);

		for (int slot_index = 0; slot_index < FOLIAGE_MAX_DEFS; ++slot_index) {
			const RuntimeSlot &slot = request.slots[slot_index];
			if (!slot.enabled) continue;
			const bool regenerate_hit =
			    ((terrain_scene_counter_ + 2u *
			                                  static_cast<uint32_t>(slot_index)) &
			      7u) == 0u;
			auto &entries = model_cache_[slot_index];
			auto &entry_index = model_cache_index_[slot_index];

			for (const SilhouetteCell &cell : cells) {
				++stats_.model_key_lookup_steps;
				const auto resident = entry_index.find(cell.key);
				ModelCacheEntry *entry =
				    resident == entry_index.end()
				        ? nullptr
				        : &entries[resident->second];

				if (entry != nullptr) {
					++stats_.model.hits;
					entry->last_use = terrain_scene_counter_;
					const bool first_refresh =
					    regenerate_hit &&
					    refreshed_model_keys[slot_index].insert(cell.key).second;
					if (first_refresh) {
						auto refreshed_instances = generate_silhouette_cell(
						    slot_index,
						    slot,
						    cell,
						    anchor_x,
						    anchor_z,
						    world);
						++stats_.model.regenerations;
						if (!same_silhouette_geometry(
						        entry->instances, refreshed_instances)) {
							output.model_evicted.push_back(CacheIdentity{
							    static_cast<uint8_t>(slot_index),
							    entry->key,
							    entry->revision});
							entry->instances = std::move(refreshed_instances);
							entry->revision = ++next_cache_revision_;
							output.model_generated.push_back(CacheIdentity{
							    static_cast<uint8_t>(slot_index),
							    entry->key,
							    entry->revision});
						}
					}
				} else {
					++stats_.model.misses;
					const size_t index = lru_index(entries);
					if (index == entries.size()) continue;
					entry = &entries[index];
					if (entry->valid) {
						entry_index.erase(entry->key);
						++stats_.model.evictions;
						output.model_evicted.push_back(CacheIdentity{
						    static_cast<uint8_t>(slot_index),
						    entry->key,
						    entry->revision});
					}
					entry->valid = true;
					entry->key = cell.key;
					entry->last_use = terrain_scene_counter_;
					entry->revision = ++next_cache_revision_;
					entry_index[cell.key] = index;
					entry->instances = generate_silhouette_cell(
					    slot_index,
					    slot,
					    cell,
					    anchor_x,
					    anchor_z,
					    world);
					++stats_.model.regenerations;
					output.model_generated.push_back(CacheIdentity{
					    static_cast<uint8_t>(slot_index),
					    entry->key,
					    entry->revision});
				}
				if (regenerate_hit) {
					refreshed_model_keys[slot_index].insert(cell.key);
				}

				if (entry->instances.empty()) continue;
				++stats_.model.submissions;
				const uint64_t submission_id = ++next_submission_id_;
				for (const SilhouetteInstance &cached_instance :
				     entry->instances) {
					SilhouetteInstance instance = cached_instance;
					instance.cache_revision = entry->revision;
					instance.submission_id = submission_id;
					instance.quadrant = cell.quadrant;
					instance.alpha_reference = alpha_reference;
					output.silhouettes.push_back(instance);
				}
			}
		}
	}

	for (const auto &entries : detail_cache_) {
		for (const DetailCacheEntry &entry : entries) {
			if (entry.valid) ++stats_.detail.residents;
		}
	}
	for (const auto &entries : model_cache_) {
		for (const ModelCacheEntry &entry : entries) {
			if (entry.valid) ++stats_.model.residents;
		}
	}
	return output;
}

bool Runtime::build_fd_rgba_mip_chain(const uint8_t *pixels,
	                                  int width,
	                                  int height,
	                                  FdMipChain &r_chain) const {
	// [orig: Foliage_LoadDefAssets @ 0x601260;
	// GTexture_CreateFromPixelDataWithAlphaBlend]
	const auto is_power_of_two = [](int value) {
		return value > 0 && (value & (value - 1)) == 0;
	};
	if (pixels == nullptr || !is_power_of_two(width) ||
	    !is_power_of_two(height) || std::min(width, height) <= 2) {
		return false;
	}
	const size_t width_size = static_cast<size_t>(width);
	const size_t height_size = static_cast<size_t>(height);
	if (width_size > std::numeric_limits<size_t>::max() / height_size ||
	    width_size * height_size >
	        std::numeric_limits<size_t>::max() / 4u) {
		return false;
	}

	const size_t pixel_count = width_size * height_size;
	std::vector<uint8_t> base(pixels, pixels + pixel_count * 4u);
	std::vector<uint8_t> source_alpha(pixel_count);
	for (size_t index = 0; index < pixel_count; ++index) {
		source_alpha[index] = base[index * 4u + 3u];
	}

	const int x_mask = width - 1;
	const int y_mask = height - 1;
	const auto alpha_at = [&](int x, int y) -> uint32_t {
		const size_t index =
		    static_cast<size_t>(y & y_mask) * width_size +
		    static_cast<size_t>(x & x_mask);
		return source_alpha[index];
	};
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			uint32_t sum = 4u * alpha_at(x, y);
			sum += alpha_at(x - 1, y) + alpha_at(x + 1, y) +
			       alpha_at(x, y - 1) + alpha_at(x, y + 1);
			sum += 2u * (alpha_at(x - 1, y - 1) +
			             alpha_at(x + 1, y - 1) +
			             alpha_at(x - 1, y + 1) +
			             alpha_at(x + 1, y + 1));
			base[(static_cast<size_t>(y) * width_size +
			      static_cast<size_t>(x)) *
			         4u +
			     3u] = static_cast<uint8_t>(sum >> 4u);
		}
	}

	std::vector<uint8_t> gray = base;
	for (size_t index = 0; index < pixel_count; ++index) {
		gray[index * 4u + 0u] = 0x80u;
		gray[index * 4u + 1u] = 0x80u;
		gray[index * 4u + 2u] = 0x80u;
	}

	const auto downsample = [](const std::vector<uint8_t> &source,
	                           int source_width,
	                           int source_height) {
		const int target_width = std::max(1, source_width / 2);
		const int target_height = std::max(1, source_height / 2);
		std::vector<uint8_t> result(
		    static_cast<size_t>(target_width) *
		        static_cast<size_t>(target_height) * 4u,
		    0u);
		for (int y = 0; y < target_height; ++y) {
			const int y0 = y * 2;
			const int y1 = std::min(y0 + 1, source_height - 1);
			for (int x = 0; x < target_width; ++x) {
				const int x0 = x * 2;
				const int x1 = std::min(x0 + 1, source_width - 1);
				for (int channel = 0; channel < 4; ++channel) {
					const auto sample = [&](int sx, int sy) -> uint32_t {
						return source[(static_cast<size_t>(sy) *
						                   static_cast<size_t>(source_width) +
						               static_cast<size_t>(sx)) *
						                  4u +
						              static_cast<size_t>(channel)];
					};
					result[(static_cast<size_t>(y) *
					            static_cast<size_t>(target_width) +
					        static_cast<size_t>(x)) *
					           4u +
					       static_cast<size_t>(channel)] =
					    static_cast<uint8_t>((sample(x0, y0) + sample(x1, y0) +
					                          sample(x0, y1) + sample(x1, y1)) >>
					                         2u);
				}
			}
		}
		return result;
	};

	int retail_level_count = 0;
	for (int minimum = std::min(width, height); minimum > 2;
	     minimum /= 2) {
		++retail_level_count;
	}

	std::vector<uint8_t> packed;
	std::vector<uint8_t> last_output;
	int current_width = width;
	int current_height = height;
	int last_width = width;
	int last_height = height;
	for (int level = 0; level < retail_level_count; ++level) {
		const uint32_t weight = std::min(
		    256u,
		    static_cast<uint32_t>(320 * level / retail_level_count));
		const size_t current_pixels =
		    static_cast<size_t>(current_width) *
		    static_cast<size_t>(current_height);
		last_output.resize(current_pixels * 4u);
		for (size_t index = 0; index < current_pixels; ++index) {
			for (size_t channel = 0; channel < 3u; ++channel) {
				last_output[index * 4u + channel] = static_cast<uint8_t>(
				    (weight * gray[index * 4u + channel] +
				     (256u - weight) * base[index * 4u + channel]) >>
				    8u);
			}
			last_output[index * 4u + 3u] = base[index * 4u + 3u];
		}
		packed.insert(packed.end(), last_output.begin(), last_output.end());
		last_width = current_width;
		last_height = current_height;

		// Retail downsamples both source chains independently after every level.
		base = downsample(base, current_width, current_height);
		gray = downsample(gray, current_width, current_height);
		current_width = std::max(1, current_width / 2);
		current_height = std::max(1, current_height / 2);
	}

	while (last_width > 1 || last_height > 1) {
		last_output = downsample(last_output, last_width, last_height);
		last_width = std::max(1, last_width / 2);
		last_height = std::max(1, last_height / 2);
		packed.insert(packed.end(), last_output.begin(), last_output.end());
	}

	FdMipChain result;
	result.width = width;
	result.height = height;
	result.retail_level_count = retail_level_count;
	result.rgba = std::move(packed);
	r_chain = std::move(result);
	return true;
}

} // namespace opennova::foliage
