#include <foliage/model_placement.h>

#include <cstdio>
#include <cstring>

using namespace opennova;
using namespace opennova::foliage;

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

PlacementSamplers make_permissive_samplers() {
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16 wx, Fixed16_16 wz) -> Fixed16_16 {
		// Deterministic non-flat height so folds carry data.
		return (wx >> 4) - (wz >> 5);
	};
	return s;
}

bool same_instance(const ModelInstance &a, const ModelInstance &b) {
	if (a.center_x_fixed != b.center_x_fixed || a.center_z_fixed != b.center_z_fixed ||
	    a.yaw_radians != b.yaw_radians) {
		return false;
	}
	for (int k = 0; k < 4; ++k) {
		if (a.corner_x_fixed[k] != b.corner_x_fixed[k] ||
		    a.corner_z_fixed[k] != b.corner_z_fixed[k] ||
		    a.corner_height[k] != b.corner_height[k]) {
			return false;
		}
	}
	return a.fold_e_a == b.fold_e_a && a.fold_t_a == b.fold_t_a &&
	       a.fold_e_b == b.fold_e_b && a.fold_t_b == b.fold_t_b;
}

bool same_result(const ModelTileResult &a, const ModelTileResult &b) {
	if (a.count != b.count) {
		return false;
	}
	for (int i = 0; i < a.count; ++i) {
		if (!same_instance(a.instances[i], b.instances[i])) {
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	ModelPlacementConfig cfg{};
	for (int i = 0; i < FOLIAGE_MAX_DEFS; ++i) {
		cfg.footprint[i] = 1.5f;
	}
	auto samplers = make_permissive_samplers();

	const uint32_t key_a = pack_model_tile_key(0x00100000, 0x00200000);
	const uint32_t key_b = pack_model_tile_key(0x00200000, 0x00100000);

	const int32_t huge_radius = 0x40000000;
	const Fixed16_16 anchor_x = 0x00180000;
	const Fixed16_16 anchor_z = 0x00280000;

	const auto r1 = generate_model_tile_instances(0, key_a, anchor_x, anchor_z, huge_radius, cfg, samplers);
	const auto r2 = generate_model_tile_instances(0, key_a, anchor_x, anchor_z, huge_radius, cfg, samplers);
	if (!expect(same_result(r1, r2), "same (slot, tile_key) must yield identical model placements")) return 1;
	if (!expect(r1.count > 0, "permissive samplers + huge radius must place instances")) return 1;

	const auto r3 = generate_model_tile_instances(0, key_b, anchor_x, anchor_z, huge_radius, cfg, samplers);
	if (!expect(!same_result(r1, r3), "different tile keys must produce different placements")) return 1;

	// Slot only affects gating (mask bit / forceon / footprint), not the PRNG:
	// with permissive samplers and equal footprints the candidate stream matches.
	const auto r4 = generate_model_tile_instances(1, key_a, anchor_x, anchor_z, huge_radius, cfg, samplers);
	if (!expect(same_result(r1, r4),
	            "the PRNG is keyed by tile only; equal-config slots see the same candidates")) return 1;

	// Out-of-range slot yields empty.
	const auto r5 = generate_model_tile_instances(4, key_a, anchor_x, anchor_z, huge_radius, cfg, samplers);
	if (!expect(r5.count == 0, "invalid slot must be empty")) return 1;

	std::printf("OK: model tile placement is deterministic per (slot, tile_key)\n");
	return 0;
}
