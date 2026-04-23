#include "foliage/placement.h"

#include <cmath>
#include <cstdlib>

namespace opennova::foliage {

namespace {

constexpr uint32_t PRNG_SEED_CONST = 0xA55B1EEDu;  // -1520754963 as uint32 (int_convert-verified)

inline uint32_t rol32(uint32_t x, int n) noexcept {
	n &= 31;
	return (x << n) | (x >> ((32 - n) & 31));
}

inline uint32_t prng_seed_from_cell(uint32_t cell_key) noexcept {
	// sub_5C0240 @ 0x5c02ae:
	//   dword_1549E48 = (a2 & 0x1FF01FF) + __ROL4__(-1520754963, a2 & 0x1F);
	return (cell_key & 0x1FF01FFu) + rol32(PRNG_SEED_CONST, static_cast<int>(cell_key & 0x1Fu));
}

inline uint32_t prng_advance(uint32_t state) noexcept {
	// sub_5C0240 @ 0x5c02e5..0x5c0349..0x5c039a:
	//   state = __ROL4__(state + __ROL4__(state, 11), 4);
	//   state ^= 1;
	return rol32(state + rol32(state, 11), 4) ^ 1u;
}

// Low 16 bits of the PRNG output drive the candidate frac; sub_5C0240 stores
// `v55 = (uint16_t)v7 ^ 1` and uses it as the frac for x, then repeats for y.
inline uint16_t prng_frac16(uint32_t state) noexcept {
	return static_cast<uint16_t>(state ^ 1u);
}

// Sign-extend the lower 16 bits of a cell-key field to a signed 32-bit cell index.
// Engine: `v43 = (HIWORD(a2) << 17) >> 17;` and `v38 = (a2 << 17) >> 17;`
inline int32_t sext_key_half(uint32_t half) noexcept {
	int32_t v = static_cast<int32_t>(half << 17);
	return v >> 17;
}

} // namespace

uint32_t pack_cell_key(Fixed16_16 cell_x_fixed, Fixed16_16 cell_z_fixed) noexcept {
	// Matches sub_5C1940 @ 0x5c1a4b:
	//   v13 = (v9 & 0x7FFF0000) + ((v12 >> 16) & 0x7FFF);
	// where v9 / v12 are cell-aligned fixed-point world coords.
	const uint32_t x_part = static_cast<uint32_t>(cell_x_fixed) & 0x7FFF0000u;
	const uint32_t z_part = (static_cast<uint32_t>(cell_z_fixed) >> 16) & 0x7FFFu;
	return x_part | z_part;
}

PlacementResult place_cell(int slot_index,
                           uint32_t cell_key,
                           Fixed16_16 view_center_x,
                           Fixed16_16 view_center_z,
                           int32_t view_radius,
                           const PlacementConfig &config,
                           const PlacementSamplers &samplers) noexcept {
	PlacementResult result{};
	if (slot_index < 0 || slot_index >= FOLIAGE_MAX_DEFS) {
		return result;
	}

	// Decode cell origin from the key. Engine uses the raw packed dword; the low
	// 16 bits encode the z-cell (<<16) and the upper 16 bits encode the x-cell.
	// `v43` (engine) = cell_z integer in world units, and `v38` = cell_x integer.
	// Note the asymmetry between pack/unpack here - the engine decodes the key by
	// sign-extending its HIWORD and LOWORD directly, which is the same as reading
	// the upper/lower halves of the original (x_fixed, z_fixed) truncated to 16 bits.
	const int32_t cell_z_int = sext_key_half(cell_key >> 16);  // v43
	const int32_t cell_x_int = sext_key_half(cell_key);        // v38

	const uint8_t slot_force_on =
	    (config.attrib_flags[slot_index] & FOLIAGE_ATTRIB_FORCE_ON) != 0 ? 1u : 0u;
	const float quad_half = config.quad_half_width[slot_index];

	uint32_t rng = prng_seed_from_cell(cell_key);

	for (int candidate_idx = 0; candidate_idx < FOLIAGE_CANDIDATES_PER_CELL; ++candidate_idx) {
		// Three draws per candidate: x frac, y frac, rotation angle.
		rng = prng_advance(rng);
		const uint16_t frac_x = prng_frac16(rng);
		const float cand_x = static_cast<float>(candidate_idx % FOLIAGE_CELL_GRID)
		                     * FOLIAGE_CANDIDATE_STEP
		                   + FOLIAGE_CANDIDATE_BASE
		                   + static_cast<float>(frac_x) * FOLIAGE_RNG_FRAC_SCALE;

		rng = prng_advance(rng);
		const uint16_t frac_y = prng_frac16(rng);
		const float cand_y = static_cast<float>(candidate_idx / FOLIAGE_CELL_GRID)
		                     * FOLIAGE_CANDIDATE_STEP
		                   + FOLIAGE_CANDIDATE_BASE
		                   + static_cast<float>(frac_y) * FOLIAGE_RNG_FRAC_SCALE;

		rng = prng_advance(rng);
		const uint16_t frac_rot = prng_frac16(rng);
		const float rotation = static_cast<float>(frac_rot) * FOLIAGE_ROTATION_SCALE;

		// World-fixed position. Engine @ 0x5c042d:
		//   (int)((v43 + v39) * 65536.0) - *a4  // x delta
		//   v32 = (int)((v38 - v27) * 65536.0); // z in engine's negated convention
		const Fixed16_16 world_x_fixed =
		    static_cast<Fixed16_16>((static_cast<float>(cell_z_int) + cand_x) * FIXED_SCALE);
		const Fixed16_16 world_z_fixed =
		    static_cast<Fixed16_16>((static_cast<float>(cell_x_int) - cand_y) * FIXED_SCALE);

		// L-infinity cull. Engine @ 0x5c04a5:
		//   abs(world_x - center.x) <= view_radius && abs(z_delta) <= view_radius
		const int64_t dx = static_cast<int64_t>(world_x_fixed) - static_cast<int64_t>(view_center_x);
		const int64_t dz = static_cast<int64_t>(world_z_fixed) - static_cast<int64_t>(view_center_z);
		if (std::llabs(dx) > view_radius || std::llabs(dz) > view_radius) {
			continue;
		}

		// Path-blocker check: engine short-circuits this when the slot has FORCE_ON.
		// Engine @ 0x5c04a5:
		//   (FORCE_ON) || !sub_5C6450(world_x, -world_z, 0x20000)
		if (!slot_force_on) {
			if (samplers.path_blocked && samplers.path_blocked(world_x_fixed, -world_z_fixed, 0x20000)) {
				continue;
			}
		}

		// Foliagemap slot-mask check. Engine @ 0x5c04a5:
		//   (1 << slot) & sub_5C65E0(world_x, world_z)
		const uint32_t mask = samplers.slot_mask_at
		                         ? samplers.slot_mask_at(world_x_fixed, world_z_fixed)
		                         : 0u;
		if (((1u << slot_index) & mask) == 0u) {
			continue;
		}

		// Accepted - build the instance. Engine samples 4 corners + 4 midpoints of
		// the quad for tangent/normal/curvature; we mirror that geometry.
		PlacementInstance inst{};
		inst.world_x_fixed = world_x_fixed;
		inst.world_z_fixed = world_z_fixed;
		inst.rotation_radians = rotation;

		const float cos_r = std::cos(rotation);
		const float sin_r = std::sin(rotation);

		// Corners: bit 0 picks +x/-x, bit 1 picks +y/-y of the quad half-vector.
		// Engine @ 0x5c04f7..0x5c0540.
		Fixed16_16 corner_x_fixed[4];
		Fixed16_16 corner_z_fixed[4];
		for (int c = 0; c < 4; ++c) {
			const float qx = ((c & 1) ? 1.0f : -1.0f) * quad_half;
			const float qy = ((c & 2) ? 1.0f : -1.0f) * quad_half;
			const float rotated_x = qx * cos_r + cand_x - qy * sin_r;
			const float rotated_y = qy * cos_r + qx * sin_r + cand_y;
			corner_x_fixed[c] = static_cast<Fixed16_16>(
			    (static_cast<float>(cell_z_int) + rotated_x) * FIXED_SCALE);
			corner_z_fixed[c] = static_cast<Fixed16_16>(
			    (static_cast<float>(cell_x_int) - rotated_y) * FIXED_SCALE);

			const Fixed16_16 h = samplers.height_at
			                         ? samplers.height_at(corner_x_fixed[c], corner_z_fixed[c])
			                         : 0;
			inst.corner_y_fixed[c] = h;
		}

		// Midpoints: engine @ 0x5c05ca..0x5c0694 uses asymmetric pairings rather
		// than simple edge midpoints.
		//   m0: x = (c0.x + c2.x) / 2, z = (c0.z + c1.z) / 2
		//   m1: x = (c1.x + c3.x) / 2, z = (c2.z + c3.z) / 2
		//   m2: x = (c0.x + c1.x) / 2, z = (c1.z + c2.z) / 2
		//   m3: x = (c2.x + c3.x) / 2, z = (c0.z + c3.z) / 2
		// Engine: jodemo.exe sub_5C0240@0x5C05CA-0x5C0694
		// docs/engine_spec_foliage.md 4.4.7
		const Fixed16_16 midpoint_x_fixed[4] = {
		    static_cast<Fixed16_16>((corner_x_fixed[0] + corner_x_fixed[2]) >> 1),
		    static_cast<Fixed16_16>((corner_x_fixed[1] + corner_x_fixed[3]) >> 1),
		    static_cast<Fixed16_16>((corner_x_fixed[0] + corner_x_fixed[1]) >> 1),
		    static_cast<Fixed16_16>((corner_x_fixed[2] + corner_x_fixed[3]) >> 1),
		};
		const Fixed16_16 midpoint_z_fixed[4] = {
		    static_cast<Fixed16_16>((corner_z_fixed[0] + corner_z_fixed[1]) >> 1),
		    static_cast<Fixed16_16>((corner_z_fixed[2] + corner_z_fixed[3]) >> 1),
		    static_cast<Fixed16_16>((corner_z_fixed[1] + corner_z_fixed[2]) >> 1),
		    static_cast<Fixed16_16>((corner_z_fixed[0] + corner_z_fixed[3]) >> 1),
		};
		for (int m = 0; m < 4; ++m) {
			inst.midpoint_y_fixed[m] = samplers.height_at
			                               ? samplers.height_at(midpoint_x_fixed[m], midpoint_z_fixed[m])
			                               : 0;
		}

		// Centre-point height (used as the instance's world_y). Not in the engine's
		// per-instance output (engine consumes corners only), but convenient for the
		// Godot side that positions MultiMesh instances by centre.
		const Fixed16_16 centre_x =
		    static_cast<Fixed16_16>((static_cast<float>(cell_z_int) + cand_x) * FIXED_SCALE);
		const Fixed16_16 centre_z =
		    static_cast<Fixed16_16>((static_cast<float>(cell_x_int) - cand_y) * FIXED_SCALE);
		inst.world_y_fixed = samplers.height_at ? samplers.height_at(centre_x, centre_z) : 0;

		result.instances[result.count++] = inst;
		if (result.count >= FOLIAGE_CELL_CAP) {
			break;
		}
	}

	return result;
}

} // namespace opennova::foliage
