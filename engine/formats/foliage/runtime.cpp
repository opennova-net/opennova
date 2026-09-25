#include <formats/foliage/runtime.h>

#include <base/io/fixed.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace opennova::foliage {

namespace {

constexpr uint32_t kSeedConstant = 0xA55B1EEDu;
constexpr int kGridWidth = 6;
constexpr int kCandidates = 36;
constexpr float kGridStep = 2.6f;
constexpr float kGridBase = 1.0f;
constexpr float kJitterScale = 1.8f / 65536.0f;
// flt_7CD4DC = 0x38C90FD0, retail's yaw step: slightly below the float
// nearest 2*pi/65536 (0x38C90FDB). [orig: generate_foliage_instances_0
// @ 0x5fff7a; Foliage_GenerateModelTileInstances @ 0x600aeb]
constexpr float kYawScale = 0x1.921FA0p-14f;
constexpr float kPathRange = 2.0f;
constexpr float kDetailFadeStart = 20.0f;
constexpr float kDetailPassSwitch = 33.0f;
constexpr float kDetailLimit = 42.0f;
constexpr float kSilhouetteDepth = 38.0f;
constexpr int32_t kFixedOne = io::kFp16OneInt;
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

// One candidate of the shared 6x6 stream. x_fixed/z_fixed are the Godot
// plane (z = -mission y); the MODEL tier additionally keeps its mission-y
// value, the frame its key and corner arithmetic run in.
struct Candidate {
	uint8_t index = 0;
	float local_a = 0.0f;
	float local_b = 0.0f;
	float yaw = 0.0f;
	int32_t x_fixed = 0;
	int32_t z_fixed = 0;
};

// Negating a mission-y fixed value into the Godot plane; the one
// unrepresentable input (INT32_MIN) saturates instead of overflowing.
int32_t negate_fixed(int32_t value) {
	return value == std::numeric_limits<int32_t>::min()
	         ? std::numeric_limits<int32_t>::max()
	         : -value;
}

Candidate next_candidate(int index, uint32_t &state) {
	// Both retail generators draw local A (grid column) then local B (grid
	// row) then the yaw from the one stream. The tiers place them on
	// different key frames (detail_candidate / model_candidate).
	// [orig: generate_foliage_instances_0 @ 0x5ffe88..0x5fff80;
	// Foliage_GenerateModelTileInstances @ 0x600a17..0x600af1]
	const uint16_t draw_a = next_draw(state);
	const uint16_t draw_b = next_draw(state);
	const uint16_t draw_yaw = next_draw(state);
	Candidate candidate;
	candidate.index = static_cast<uint8_t>(index);
	candidate.local_a = kGridBase +
	                    static_cast<float>(index % kGridWidth) * kGridStep +
	                    static_cast<float>(draw_a) * kJitterScale;
	candidate.local_b = kGridBase +
	                    static_cast<float>(index / kGridWidth) * kGridStep +
	                    static_cast<float>(draw_b) * kJitterScale;
	candidate.yaw = static_cast<float>(draw_yaw) * kYawScale;
	return candidate;
}

Candidate detail_candidate(uint32_t key, int index, uint32_t &state) {
	// The detail key's HIGH15 is the cell's X-min and LOW15 its Z-min on the
	// Godot plane (the collector packs PolyTrn's -camera_y sector, FB20, into
	// the low half); local A and local B both ADD. Retail's blocker, map and
	// height samples all take (keyHi + A, keyLo + B).
	// [orig: generate_foliage_instances_0 @ 0x5fff84..0x5fffa2 (placement),
	// 0x5fffb8..0x60000e (blocker), 0x600021..0x600065 (samples);
	// Terrain_CollectNearFoliagePatches @ 0x603f69..0x603f8a (key)]
	Candidate candidate = next_candidate(index, state);
	const int32_t base_x = sign_extend_15(key >> 16u);
	const int32_t base_z = sign_extend_15(key);
	candidate.x_fixed = to_fixed(static_cast<float>(base_x) + candidate.local_a);
	candidate.z_fixed = to_fixed(static_cast<float>(base_z) + candidate.local_b);
	return candidate;
}

Candidate model_candidate(uint32_t key, int index, uint32_t &state,
                          int32_t &r_mission_y_fixed) {
	// The MODEL key's HIGH15 is the cell's X-min and LOW15 its MISSION-y top
	// (y_snap + 16); the candidate is (keyHi + A, keyLo - B) in mission x/y,
	// so on the Godot plane z = B - keyLo.
	// [orig: Foliage_GenerateModelTileInstances @ 0x600af5..0x600b0c;
	// Foliage_UpdateModelTiles @ 0x601fd0..0x60205b (key)]
	Candidate candidate = next_candidate(index, state);
	const int32_t base_x = sign_extend_15(key >> 16u);
	const int32_t base_y = sign_extend_15(key);
	candidate.x_fixed = to_fixed(static_cast<float>(base_x) + candidate.local_a);
	r_mission_y_fixed =
	    to_fixed(static_cast<float>(base_y) - candidate.local_b);
	candidate.z_fixed = negate_fixed(r_mission_y_fixed);
	return candidate;
}

uint32_t pack_silhouette_key(int32_t snap_x, int32_t snap_y) {
	const uint32_t x_part = static_cast<uint32_t>(snap_x) & 0x7FFF0000u;
	const uint32_t y_part =
	    ((static_cast<uint32_t>(snap_y) + static_cast<uint32_t>(kTileSizeFixed)) >> 16u) &
	    0x7FFFu;
	return x_part | y_part;
}

struct SilhouetteCell {
	uint32_t key = 0;
	uint8_t quadrant = 0;
};

// The four cells around one anchor, keyed in the MISSION frame: quadrant bit
// 0 steps x by -8 (else +8), bit 1 steps mission y by -8 (else +8), each
// snapped to the 16-unit grid; the low half keys the mission-y cell top.
// [orig: Foliage_UpdateModelTiles @ 0x601fd0..0x60205b]
std::array<SilhouetteCell, 4> silhouette_cells(int32_t anchor_x,
                                               int32_t anchor_mission_y) {
	std::array<SilhouetteCell, 4> result{};
	for (int quadrant = 0; quadrant < 4; ++quadrant) {
		const int32_t x_offset =
		    (quadrant & 1) != 0 ? -kQuadrantOffsetFixed : kQuadrantOffsetFixed;
		const int32_t y_offset =
		    (quadrant & 2) != 0 ? -kQuadrantOffsetFixed : kQuadrantOffsetFixed;
		// Retail integer addition wraps in 32 bits. Perform it unsigned so
		// public coordinates near the fixed-point limits cannot trigger C++
		// signed-overflow UB before the tile mask is applied.
		const int32_t snap_x = static_cast<int32_t>(
		    (static_cast<uint32_t>(anchor_x) +
		     static_cast<uint32_t>(x_offset)) &
		    kTileSnapMask);
		const int32_t snap_y = static_cast<int32_t>(
		    (static_cast<uint32_t>(anchor_mission_y) +
		     static_cast<uint32_t>(y_offset)) &
		    kTileSnapMask);
		result[quadrant].key = pack_silhouette_key(snap_x, snap_y);
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
    const DetailCell &cell,
    const WorldSamplers &world) {
	const uint32_t cell_key = cell.key;
	// The detail tier expands the def model's full source geometry for every
	// accepted transform. This module returns the transforms; the render
	// binding samples terrain height per transformed source vertex.
	// [orig: generate_foliage_instances_0 @ 0x5ffdd0;
	// Terrain_CollectNearFoliagePatches @ 0x603e60]
	std::vector<DetailInstance> result;
	// Flat-sector keys remain ordinary cache residents, but their generator
	// writes zero index/vertex counts before any placement or terrain sample.
	// [orig: generate_foliage_instances_0 @ 0x5FFDD0, flag gate @ 0x5FFE05,
	// zero counts @ 0x5FFE10..0x5FFE16; Foliage_UpdateFarCellSlots @ 0x601B30]
	if ((cell_key & 0x80000000u) != 0u) return result;
	uint32_t state = seed_for_key(cell_key);
	for (int index = 0; index < kCandidates; ++index) {
		const Candidate candidate = detail_candidate(cell_key, index, state);
		if (candidate_is_blocked(slot, world, candidate)) continue;
		// The map gate takes the key's atlas halves (& 0x3FF) plus the same
		// local offsets, while the blocker takes the world position.
		// [orig: generate_foliage_instances_0 @ 0x5fff84..0x5fff9d (atlas
		// sample coordinates), 0x5fffb8..0x60000e (world blocker),
		// @ 0x600065 (Terrain_GetSurfaceTypeAtFixedPoint)]
		const uint32_t mask =
		    world.detail_foliage_mask_at
		        ? world.detail_foliage_mask_at(
		              to_fixed(static_cast<float>(cell.atlas_x) + candidate.local_a),
		              to_fixed(static_cast<float>(cell.atlas_z) + candidate.local_b))
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

	// The corner arithmetic runs in the MODEL key's mission frame: corner
	// (keyHi + A', keyLo - B') with (A', B') the rotated footprint offsets,
	// the edge midpoints are the arithmetic-shift halves of those fixed
	// coordinates, and every height sample takes mission (x, y), i.e. the
	// Godot plane (x, -y). [orig: Foliage_GenerateModelTileInstances
	// @ 0x600bd0..0x600c6a (corners), 0x600c70..0x600d32 (midpoints)]
	const float cosine = std::cos(candidate.yaw);
	const float sine = std::sin(candidate.yaw);
	const int32_t base_x = sign_extend_15(cell_key >> 16u);
	const int32_t base_y = sign_extend_15(cell_key);
	std::array<int32_t, 4> corner_x{};
	std::array<int32_t, 4> corner_y{};

	for (int corner = 0; corner < 4; ++corner) {
		const float local_a = (corner & 1) != 0 ? footprint : -footprint;
		const float local_b = (corner & 2) != 0 ? footprint : -footprint;
		const float rotated_a = candidate.local_a +
		                        local_a * cosine - local_b * sine;
		const float rotated_b = candidate.local_b +
		                        local_a * sine + local_b * cosine;
		if (!try_to_fixed(static_cast<float>(base_x) + rotated_a,
		                  corner_x[corner]) ||
		    !try_to_fixed(static_cast<float>(base_y) - rotated_b,
		                  corner_y[corner])) {
			return false;
		}
		instance.corner_local_a[corner] = rotated_a;
		instance.corner_local_b[corner] = rotated_b;
		const int32_t corner_z = negate_fixed(corner_y[corner]);
		instance.corners[corner] = {
		    from_fixed(corner_x[corner]),
		    from_fixed(corner_z),
		    sampled_height(world, corner_x[corner], corner_z),
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
		    negate_fixed(midpoint_fixed(corner_y[first], corner_y[second])));
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
    int32_t anchor_mission_y,
	const WorldSamplers &world) {
	std::vector<SilhouetteInstance> result;
	if (!std::isfinite(slot.model_radius) || slot.model_radius < 0.0f) {
		return result;
	}
	const float footprint = slot.model_radius * 0.75f;
	uint32_t state = seed_for_key(cell.key);
	for (int index = 0; index < kCandidates; ++index) {
		int32_t mission_y = 0;
		const Candidate candidate =
		    model_candidate(cell.key, index, state, mission_y);
		// The +-4u box around the anchor, in the mission frame.
		// [orig: Foliage_GenerateModelTileInstances @ 0x600b11..0x600b45]
		const int64_t dx =
		    static_cast<int64_t>(candidate.x_fixed) - anchor_x;
		const int64_t dy =
		    static_cast<int64_t>(mission_y) - anchor_mission_y;
		if (std::llabs(dx) > kAnchorRadiusFixed ||
		    std::llabs(dy) > kAnchorRadiusFixed) {
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
		if (static_cast<int>(result.size()) >= kModelTileInstanceCap) break;
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
		return cell.camera_distance <= kDetailLimit;
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

	// Detail update: resident lookup stops at the first key; duplicate missing
	// keys allocate once. Geometry persists until strict signed-age LRU reuse.
	// [orig: Foliage_UpdateFarCellSlots @ 0x601b30]
	for (int slot_index = 0; slot_index < FOLIAGE_MAX_DEFS; ++slot_index) {
		const RuntimeSlot &slot = request.slots[slot_index];
		auto &entries = detail_cache_[slot_index];
		if (!slot.enabled || entries.empty()) continue;

		std::vector<const DetailCell *> pending_cells;
		pending_cells.reserve(request.detail_cells.size());
		for (const DetailCell &cell : request.detail_cells) {
			if (!detail_cell_is_visible(cell)) continue;
			const size_t resident_index = find_detail_index(entries, cell.key);
			if (resident_index != entries.size()) {
				++stats_.detail.hits;
				entries[resident_index].last_use = terrain_scene_counter_;
				continue;
			}

			++stats_.detail.misses;
			if (std::find_if(pending_cells.begin(), pending_cells.end(),
			                 [&cell](const DetailCell *pending) {
				                 return pending->key == cell.key;
			                 }) == pending_cells.end()) {
				pending_cells.push_back(&cell);
			}
		}

		for (const DetailCell *pending : pending_cells) {
			const uint32_t key = pending->key;
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
			    generate_detail_cell(slot_index, slot, *pending, world);
			++stats_.detail.regenerations;
			output.detail_generated.push_back(CacheIdentity{
			    static_cast<uint8_t>(slot_index), entry.key, entry.revision});
		}
	}

	// The detail draw consumes the pool the update above just filled: the
	// terrain frame (PolyTrn_RenderFrame, whose tail runs the update) renders
	// before the scene core's two detail passes, so a newly collected key
	// draws in the frame it was generated.
	// [orig: Render_ProcessMainSceneFrame @ 0x5ca654 (terrain frame) then
	// @ 0x5ca8ec (Terrain_RenderSceneWithReflection); PolyTrn_RenderFrame
	// @ 0x60f0ea..0x60f10f (update); Foliage_SetupFarSlotDraw
	// @ 0x600807..0x60081c (key lookup)]
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

		// The two detail passes split patches by the collected leaf's maximum
		// height against the water: formatType XOR cameraBelowWater selects
		// the patches with some height above the water, so a patch lying
		// wholly at or below it draws in the far pass (formatType 0) while
		// the camera is above, and in the camera pass while it is below.
		// [orig: Foliage_RenderFarPatches @ 0x609df4..0x609e1b (flag),
		// @ 0x60a1a0..0x60a1c6 (node +0x28 vs water); Foliage_RenderFarPatchesPass
		// calls @ 0x5c95c5 (formatType 0) / @ 0x5c9665 (formatType 1)]
		const bool patch_below_water = request.water_height >= cell.max_height;
		const DetailWaterPass water_pass =
		    (patch_below_water != request.camera_below_water)
		        ? DetailWaterPass::FarSide
		        : DetailWaterPass::CameraSide;
		for (int slot_index = 0; slot_index < FOLIAGE_MAX_DEFS; ++slot_index) {
			if (!request.slots[slot_index].enabled) continue;
			const auto &entries = detail_cache_[slot_index];
			const size_t index = find_detail_index(entries, cell.key);
			if (index == entries.size() || entries[index].instances.empty()) {
				continue;
			}
			const DetailCacheEntry &entry = entries[index];
			const auto append_submission =
			    [this, &entry, &output, water_pass](DetailPass pass,
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
					instance.water_pass = water_pass;
					output.detail.push_back(instance);
				}
			};

			// Retail submits the near resident twice in one main-scene pass:
			// the 180-reference depth-writing HIGH draw first, then the exact
			// same cached geometry under the 8-reference no-depth-write LOW
			// draw at the SAME c6 fade with strict D3DCMP_LESS. At and beyond
			// the 33-unit switch only the primary LOW pass is submitted. The
			// thermal view (the scene core's fourth argument, the held
			// weapon's thermal byte) forces the primary LOW pass for every
			// patch and scales its c6 fade by flt_7C69F4 = 0.1 (the fmul
			// @ 0x60a4a8).
			// [orig: Foliage_RenderFarPatches @ 0x60a171..0x60a19c pass
			// select, 0x60a497..0x60a4ae thermal fade scale,
			// 0x60a659..0x60a694 secondary setup/draw;
			// Terrain_RenderSceneWithReflection @ 0x5c95c1/0x5c9661 (arg);
			// Render_ProcessMainSceneFrame @ 0x5ca2da..0x5ca2e3, 0x5ca8e3]
			if (request.thermal_view) {
				append_submission(DetailPass::LowAlphaTest, 8u, alpha * 0.1f,
				                  false);
			} else if (cell.camera_distance < kDetailPassSwitch) {
				append_submission(DetailPass::HighAlphaTest, 180u, alpha, false);
				append_submission(DetailPass::LowAlphaTest, 8u, alpha, true);
			} else {
				append_submission(DetailPass::LowAlphaTest, 8u, alpha, false);
			}
		}
	}

	// Distant model cache: one fixed 1000-entry pool per definition. Every hit
	// touches its resident entry, and on the definition's eight-scene phase
	// every visit regenerates it around THAT visit's anchor before drawing it,
	// so clustered anchors sharing a cell each redraw it around themselves.
	// [orig: Foliage_UpdateModelTiles @ 0x601f50, hit touch @ 0x60208b,
	// phase test @ 0x602085..0x60209a, regeneration @ 0x6020aa, draw
	// @ 0x6021a5]
	for (const SilhouetteAnchor &anchor : request.silhouette_anchors) {
		if (!std::isfinite(anchor.position.x) ||
		    !std::isfinite(anchor.position.z) ||
		    !std::isfinite(anchor.view_depth) ||
		    !std::isfinite(anchor.camera_distance)) {
			continue;
		}
		if (anchor.view_depth < kSilhouetteDepth) continue;
		// The anchor enters the MODEL walk as its mission (x, y) position;
		// the Godot plane's z is -y. [orig: Terrain_RenderSectorEntitiesBySide
		// @ 0x5c7e1e..0x5c7e24 (tile_coords = entity +4/+8)]
		int32_t anchor_x = 0;
		int32_t anchor_mission_y = 0;
		if (!try_to_fixed(anchor.position.x, anchor_x) ||
		    !try_to_fixed(-anchor.position.z, anchor_mission_y)) {
			continue;
		}
		const auto cells = silhouette_cells(anchor_x, anchor_mission_y);
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
					if (regenerate_hit) {
						auto refreshed_instances = generate_silhouette_cell(
						    slot_index,
						    slot,
						    cell,
						    anchor_x,
						    anchor_mission_y,
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
					    anchor_mission_y,
					    world);
					++stats_.model.regenerations;
					output.model_generated.push_back(CacheIdentity{
					    static_cast<uint8_t>(slot_index),
					    entry->key,
					    entry->revision});
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
					instance.far_side = anchor.far_side;
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
