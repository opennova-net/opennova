// Procedural per-cell foliage placement — a faithful port of the deterministic
// FAR grass/bush instancer. [orig: generate_foliage_instances_0 @ 0x5ffdd0 (retail
// Jointops) — seed 0xA55B1EED, the ROL-hash PRNG, 36 candidates/cell, the
// surface-type gate (Terrain_GetSurfaceTypeAtFixedPoint @ 0x6066d0), and the
// 0x20000 proximity spacing; byte-identical, see docs/foliage/foliage-re.md
// (PAR-R2). Originally ported from jodemo sub_5C0240/5C6450/5C65E0.]
#include "foliage/placement.h"

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

// Low 16 bits of the already-^1-folded PRNG state drive each candidate draw.
// [orig: generate_foliage_instances_0 @ 0x5ffdd0]
inline uint16_t prng_frac16(uint32_t state) noexcept {
	return static_cast<uint16_t>(state);
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
                           const PlacementConfig &config,
                           const PlacementSamplers &samplers) noexcept {
	PlacementResult result{};
	if (slot_index < 0 || slot_index >= FOLIAGE_MAX_DEFS) {
		return result;
	}
	// [orig: generate_foliage_instances_0 @ 0x5ffdd0] The packed-key sign
	// bit is the empty-cell marker; it must not decode as real coordinates.
	if ((cell_key & 0x80000000u) != 0u) {
		return result;
	}

	// Decode cell origin from the key. Engine uses the raw packed dword; the
	// HIGH half is the world-X cell base and the LOW half the world-Z cell
	// base - retail passes the HIWORD-derived coordinate as the X argument of
	// the spacing/surface queries [orig: generate_foliage_instances_0
	// @ 0x600001..0x600009]. Sign-extension mirrors the engine's
	// `(half << 17) >> 17` decode.
	const int32_t key_x_int = sext_key_half(cell_key >> 16);
	const int32_t key_z_int = sext_key_half(cell_key);

	const uint8_t slot_force_on =
	    (config.attrib_flags[slot_index] & FOLIAGE_ATTRIB_FORCE_ON) != 0 ? 1u : 0u;
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

		// World-fixed position [orig: generate_foliage_instances_0
		// @ 0x5fff7a..0x5fffb2]: world X = keyHigh + localA, world Z =
		// keyLow - localB (the local B axis runs negative world Z). The
		// retail generator applies no view cull here - the caller's collect
		// list already scoped the cell set.
		const Fixed16_16 world_x_fixed =
		    static_cast<Fixed16_16>((static_cast<float>(key_x_int) + cand_x) * FIXED_SCALE);
		const Fixed16_16 world_z_fixed =
		    static_cast<Fixed16_16>((static_cast<float>(key_z_int) - cand_y) * FIXED_SCALE);

		// Path-blocker check: retail short-circuits it when the slot has
		// FORCE_ON [orig: generate_foliage_instances_0 @ 0x5fffb6 (attrib
		// byte_2C2608C bit 0) / sub_606490(x, -z, 0x20000) @ 0x600009].
		if (!slot_force_on) {
			if (samplers.path_blocked && samplers.path_blocked(world_x_fixed, -world_z_fixed, 0x20000)) {
				continue;
			}
		}

		// FAR surface-map mask check. The raw charmap byte is already the
		// four-slot bitmask. [orig: Terrain_GetSurfaceTypeAtFixedPoint
		// @ 0x6066d0, caller @ 0x600065..0x600079]
		const uint32_t mask = samplers.slot_mask_at
		                         ? samplers.slot_mask_at(world_x_fixed, -world_z_fixed)
		                         : 0u;
		if (((1u << slot_index) & mask) == 0u) {
			continue;
		}

		// Accepted FAR candidate. Geometry-dependent terrain samples happen in
		// emit_far_mesh(), once the authored source vertices are available.
		PlacementInstance inst{};
		inst.world_x_fixed = world_x_fixed;
		inst.world_z_fixed = world_z_fixed;
		inst.rotation_radians = rotation;

		result.instances[result.count++] = inst;
	}

	return result;
}

} // namespace opennova::foliage
