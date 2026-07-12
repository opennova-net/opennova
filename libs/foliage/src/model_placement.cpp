// The foliage MODEL tier - per-tile 3DI-cluster placement, a faithful port of
// the retail generator. [orig: Foliage_GenerateModelTileInstances @ 0x600980
// (Jointops.exe) - seed 0xA55B1EED, the ROL-hash PRNG with the ^1 fold, 36
// candidates/tile, the fixed +-4u Chebyshev range gate, the 0x20000 path
// spacing, the foliage-map mask gate, the rotated footprint corners, the
// edge-midpoint samples, and the (E_A, T_A, E_B, T_B) sag fold; tile keys per
// Foliage_UpdateModelTiles @ 0x601f50. Witness record:
// docs/foliage/foliage-re.md §The model tier.]
#include "foliage/model_placement.h"

#include <cmath>
#include <cstdlib>

namespace opennova::foliage {

namespace {

constexpr uint32_t MODEL_PRNG_SEED_CONST = 0xA55B1EEDu;  // shared with the FAR tier

inline uint32_t rol32(uint32_t x, int n) noexcept {
	n &= 31;
	return (x << n) | (x >> ((32 - n) & 31));
}

inline uint32_t model_prng_seed(uint32_t tile_key) noexcept {
	// [orig: Foliage_GenerateModelTileInstances @ 0x600980]
	//   state = (key & 0x1FF01FF) + __ROL4__(0xA55B1EED, key & 0x1F);
	return (tile_key & 0x1FF01FFu) + rol32(MODEL_PRNG_SEED_CONST, static_cast<int>(tile_key & 0x1Fu));
}

inline uint32_t model_prng_step(uint32_t state) noexcept {
	// [orig: Foliage_GenerateModelTileInstances @ 0x600980]
	//   state = __ROL4__(state + __ROL4__(state, 11), 4) ^ 1; value = (u16)state
	return rol32(state + rol32(state, 11), 4) ^ 1u;
}

// Sign-extend the lower 15 bits of a packed key half to a signed cell/unit
// index (same decode family as the FAR tier's pack_cell_key consumers).
inline int32_t sext_key_half(uint32_t half) noexcept {
	int32_t v = static_cast<int32_t>(half << 17);
	return v >> 17;
}

} // namespace

uint32_t pack_model_tile_key(Fixed16_16 snap_x_fixed, Fixed16_16 snap_z_fixed) noexcept {
	// [orig: Foliage_UpdateModelTiles @ 0x601f50]
	//   key = (snapX & 0x7FFF0000) | (((snapZ + 0x100000) >> 16) & 0x7FFF)
	// The +16u on the Z half compensates the negated local B axis; the tile
	// covers [snap, snap+16]^2.
	const uint32_t x_part = static_cast<uint32_t>(snap_x_fixed) & 0x7FFF0000u;
	const uint32_t z_part =
	    (static_cast<uint32_t>(snap_z_fixed + MODEL_TILE_SIZE) >> 16) & 0x7FFFu;
	return x_part | z_part;
}

std::array<ModelTileRef, 4> model_quadrant_tiles(Fixed16_16 anchor_x_fixed,
                                                 Fixed16_16 anchor_z_fixed) noexcept {
	// [orig: Foliage_UpdateModelTiles @ 0x601f50] - the 4 quadrant tiles are
	// the 16u cells (snap mask 0xFFF00000) overlapping anchor +-0x80000 on
	// each axis. Quadrant sign layout mirrors the quad dispatcher's walk.
	std::array<ModelTileRef, 4> out{};
	for (int quad = 0; quad < 4; ++quad) {
		const Fixed16_16 x_off = (quad & 1) ? -MODEL_QUADRANT_OFFSET : MODEL_QUADRANT_OFFSET;
		const Fixed16_16 z_off = (quad & 2) ? -MODEL_QUADRANT_OFFSET : MODEL_QUADRANT_OFFSET;
		const Fixed16_16 snap_x = (anchor_x_fixed + x_off) & MODEL_TILE_SNAP_MASK;
		const Fixed16_16 snap_z = (anchor_z_fixed + z_off) & MODEL_TILE_SNAP_MASK;
		out[quad].key = pack_model_tile_key(snap_x, snap_z);
		out[quad].snap_x_fixed = snap_x;
		out[quad].snap_z_fixed = snap_z;
	}
	return out;
}

ModelTileResult generate_model_tile_instances(int slot_index,
                                              uint32_t tile_key,
                                              Fixed16_16 anchor_x_fixed,
                                              Fixed16_16 anchor_z_fixed,
                                              int32_t candidate_radius_fixed,
                                              const ModelPlacementConfig &config,
                                              const PlacementSamplers &samplers) noexcept {
	ModelTileResult result{};
	if (slot_index < 0 || slot_index >= FOLIAGE_MAX_DEFS) {
		return result;
	}

	// Decode the tile bases from the key: keyHigh = snapX in world units,
	// keyLow = snapZ units + 16 (the packed +0x100000 bias).
	const int32_t key_high_units = sext_key_half(tile_key >> 16);
	const int32_t key_low_units = sext_key_half(tile_key);

	const bool slot_force_on =
	    (config.attrib_flags[slot_index] & FOLIAGE_ATTRIB_FORCE_ON) != 0;
	const float footprint = config.footprint[slot_index];

	uint32_t rng = model_prng_seed(tile_key);

	for (int candidate_idx = 0; candidate_idx < MODEL_CANDIDATES_PER_TILE; ++candidate_idx) {
		// Three draws per candidate: jitter A, jitter B, yaw. value = (u16)state.
		rng = model_prng_step(rng);
		const uint16_t draw_a = static_cast<uint16_t>(rng);
		const float local_a = MODEL_CANDIDATE_MARGIN
		                    + static_cast<float>(candidate_idx % MODEL_TILE_GRID) * MODEL_CANDIDATE_STEP
		                    + static_cast<float>(draw_a) * MODEL_RNG_FRAC_SCALE;

		rng = model_prng_step(rng);
		const uint16_t draw_b = static_cast<uint16_t>(rng);
		const float local_b = MODEL_CANDIDATE_MARGIN
		                    + static_cast<float>(candidate_idx / MODEL_TILE_GRID) * MODEL_CANDIDATE_STEP
		                    + static_cast<float>(draw_b) * MODEL_RNG_FRAC_SCALE;

		rng = model_prng_step(rng);
		const uint16_t draw_yaw = static_cast<uint16_t>(rng);
		const float yaw = static_cast<float>(draw_yaw) * MODEL_YAW_SCALE;

		// Axis form [orig: Foliage_GenerateModelTileInstances @ 0x600980]:
		//   worldX = (keyHigh + localA) * 65536
		//   worldZ = (keyLow  - localB) * 65536  - local B runs NEGATIVE world Z.
		const Fixed16_16 world_x_fixed = static_cast<Fixed16_16>(
		    (static_cast<float>(key_high_units) + local_a) * FIXED_SCALE);
		const Fixed16_16 world_z_fixed = static_cast<Fixed16_16>(
		    (static_cast<float>(key_low_units) - local_b) * FIXED_SCALE);

		// Gate 1: per-axis Chebyshev range around the anchor entity.
		const int64_t dx = static_cast<int64_t>(world_x_fixed) - static_cast<int64_t>(anchor_x_fixed);
		const int64_t dz = static_cast<int64_t>(world_z_fixed) - static_cast<int64_t>(anchor_z_fixed);
		if (std::llabs(dx) > candidate_radius_fixed || std::llabs(dz) > candidate_radius_fixed) {
			continue;
		}

		// Gate 2: path/spacing reject, skipped on FORCE_ON (attrib bit 0,
		// byte_2C2608C family; record byte +532).
		if (!slot_force_on) {
			if (samplers.path_blocked &&
			    samplers.path_blocked(world_x_fixed, -world_z_fixed, MODEL_PATH_SPACING)) {
				continue;
			}
		}

		// Gate 3: foliage-map mask - the model tier gates on the FOLIAGEMAP
		// byte [orig: Foliage_SampleFoliageMapMask @ 0x606620], not the FAR
		// tier's raw charmap surface mask.
		const uint32_t mask = samplers.slot_mask_at
		                          ? samplers.slot_mask_at(world_x_fixed, world_z_fixed)
		                          : 0u;
		if (((1u << slot_index) & mask) == 0u) {
			continue;
		}

		// Accepted - footprint corners + the 8-sample ground fit.
		ModelInstance inst{};
		inst.center_x_fixed = world_x_fixed;
		inst.center_z_fixed = world_z_fixed;
		inst.yaw_radians = yaw;

		const float cos_y = std::cos(yaw);
		const float sin_y = std::sin(yaw);

		// Corners k = 0..3 at (A, B) = (k&1 ? +F : -F, k&2 ? +F : -F);
		// corner local = (LA + A*cos - B*sin, LB + A*sin + B*cos); corner
		// world via the axis form above.
		for (int k = 0; k < 4; ++k) {
			const float fa = ((k & 1) ? 1.0f : -1.0f) * footprint;
			const float fb = ((k & 2) ? 1.0f : -1.0f) * footprint;
			const float la = local_a + fa * cos_y - fb * sin_y;
			const float lb = local_b + fa * sin_y + fb * cos_y;
			inst.corner_x_fixed[k] = static_cast<Fixed16_16>(
			    (static_cast<float>(key_high_units) + la) * FIXED_SCALE);
			inst.corner_z_fixed[k] = static_cast<Fixed16_16>(
			    (static_cast<float>(key_low_units) - lb) * FIXED_SCALE);
			const Fixed16_16 h = samplers.height_at
			                         ? samplers.height_at(inst.corner_x_fixed[k], inst.corner_z_fixed[k])
			                         : 0;
			inst.corner_height[k] = static_cast<float>(h) * FIXED_TO_FLOAT;
		}

		// Edge midpoints, sampled at the integer midpoint of the FIXED corner
		// coords ((x1+x2)>>1): mid(c0,c2) = -A edge, mid(c1,c3) = +A,
		// mid(c0,c1) = -B, mid(c2,c3) = +B.
		auto sample_mid = [&samplers, &inst](int a, int b) -> float {
			const Fixed16_16 mx = static_cast<Fixed16_16>(
			    (inst.corner_x_fixed[a] + inst.corner_x_fixed[b]) >> 1);
			const Fixed16_16 mz = static_cast<Fixed16_16>(
			    (inst.corner_z_fixed[a] + inst.corner_z_fixed[b]) >> 1);
			const Fixed16_16 h = samplers.height_at ? samplers.height_at(mx, mz) : 0;
			return static_cast<float>(h) * FIXED_TO_FLOAT;
		};
		const float h_mid_minus_a = sample_mid(0, 2);
		const float h_mid_plus_a = sample_mid(1, 3);
		const float h_mid_minus_b = sample_mid(0, 1);
		const float h_mid_plus_b = sample_mid(2, 3);

		// Sag per edge: D_edge = h_mid - (h_cornerA + h_cornerB)/2; fold:
		// E_A = (D_-A + D_+A)/2, T_A = D_+A - E_A (same for B).
		const float d_minus_a = h_mid_minus_a - (inst.corner_height[0] + inst.corner_height[2]) * 0.5f;
		const float d_plus_a = h_mid_plus_a - (inst.corner_height[1] + inst.corner_height[3]) * 0.5f;
		const float d_minus_b = h_mid_minus_b - (inst.corner_height[0] + inst.corner_height[1]) * 0.5f;
		const float d_plus_b = h_mid_plus_b - (inst.corner_height[2] + inst.corner_height[3]) * 0.5f;
		inst.fold_e_a = (d_minus_a + d_plus_a) * 0.5f;
		inst.fold_t_a = d_plus_a - inst.fold_e_a;
		inst.fold_e_b = (d_minus_b + d_plus_b) * 0.5f;
		inst.fold_t_b = d_plus_b - inst.fold_e_b;

		result.instances[result.count++] = inst;
		if (result.count >= MODEL_TILE_CAP) {
			break;
		}
	}

	return result;
}

int model_alpha_ref(int32_t dist_units) noexcept {
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7d50]
	//   ref = clamp(int(4096.0 / ((dist >> 16) + 1)), 8, 128); flt_7C6F90 = 4096.0
	const float ref = MODEL_ALPHA_REF_NUMERATOR / static_cast<float>(dist_units + 1);
	int v = static_cast<int>(ref);
	if (v < MODEL_ALPHA_REF_MIN) {
		v = MODEL_ALPHA_REF_MIN;
	}
	if (v > MODEL_ALPHA_REF_MAX) {
		v = MODEL_ALPHA_REF_MAX;
	}
	return v;
}

float model_instance_hbase(const ModelInstance &inst) noexcept {
	// Bilinear at (0.5, 0.5) = corner average; sag(0, 0) = E_A + E_B.
	return (inst.corner_height[0] + inst.corner_height[1] + inst.corner_height[2] +
	        inst.corner_height[3]) * 0.25f
	     + inst.fold_e_a + inst.fold_e_b;
}

float model_ground_height(const ModelInstance &inst, float x_n, float z_n) noexcept {
	// [orig: Foliage_GridPlacementVS, Terrain_CreateFoliageVertexShaders
	// @ 0x5ff630] - bilinear corner weights + the biquadratic sag.
	const float w0 = (1.0f - x_n) * (1.0f - z_n);
	const float w1 = x_n * (1.0f - z_n);
	const float w2 = (1.0f - x_n) * z_n;
	const float w3 = x_n * z_n;
	const float ground = w0 * inst.corner_height[0] + w1 * inst.corner_height[1] +
	                     w2 * inst.corner_height[2] + w3 * inst.corner_height[3];
	const float u = 2.0f * x_n - 1.0f;
	const float v = 2.0f * z_n - 1.0f;
	const float sag = (1.0f - v * v) * (inst.fold_e_a + u * inst.fold_t_a) +
	                  (1.0f - u * u) * (inst.fold_e_b + v * inst.fold_t_b);
	return ground + sag;
}

} // namespace opennova::foliage
