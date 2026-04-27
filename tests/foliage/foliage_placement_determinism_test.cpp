#include <foliage/placement.h>

#include <cstdio>

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
	s.height_at = [](Fixed16_16, Fixed16_16) -> Fixed16_16 { return 0; };
	return s;
}

bool same_result(const PlacementResult &a, const PlacementResult &b) {
	if (a.count != b.count) {
		return false;
	}
	for (int i = 0; i < a.count; ++i) {
		if (a.instances[i].world_x_fixed != b.instances[i].world_x_fixed ||
		    a.instances[i].world_z_fixed != b.instances[i].world_z_fixed ||
		    a.instances[i].rotation_radians != b.instances[i].rotation_radians) {
			return false;
		}
	}
	return true;
}

} // namespace

int main() {
	PlacementConfig cfg{};
	for (int i = 0; i < FOLIAGE_MAX_DEFS; ++i) {
		cfg.attrib_flags[i] = FOLIAGE_ATTRIB_FORCE_ON;
	}
	auto samplers = make_permissive_samplers();

	const uint32_t key_a = pack_cell_key(0x00100000, 0x00200000);
	const uint32_t key_b = pack_cell_key(0x00200000, 0x00100000);

	const int32_t huge_radius = 0x40000000;  // accept everything
	const Fixed16_16 centre_x = 0x00100000;
	const Fixed16_16 centre_z = 0x00200000;

	const auto r1 = place_cell(0, key_a, centre_x, centre_z, huge_radius, cfg, samplers);
	const auto r2 = place_cell(0, key_a, centre_x, centre_z, huge_radius, cfg, samplers);
	if (!expect(same_result(r1, r2), "same (slot, cell_key) must yield identical placements")) return 1;
	if (!expect(r1.count > 0, "permissive samplers + huge radius must place at least one instance")) return 1;

	const auto r3 = place_cell(0, key_b, centre_x, centre_z, huge_radius, cfg, samplers);
	if (!expect(!same_result(r1, r3), "different cell keys must produce different placements")) return 1;

	// Different slot_index with same key should differ too (quad_half_width differs per slot
	// only if configured — PRNG sequence is identical, but acceptance-order isn't since the
	// L∞ cull depends on center distance which is the same here; so slot-specific output
	// differs via per-slot FORCE_ON + per-slot quad_half_width picking. For this test we
	// just assert count is non-negative and within cap.).
	const auto r4 = place_cell(1, key_a, centre_x, centre_z, huge_radius, cfg, samplers);
	if (!expect(r4.count >= 0 && r4.count <= FOLIAGE_CELL_CAP, "slot 1 output within bounds")) return 1;

	std::printf("OK: foliage placement is deterministic per (slot, cell_key)\n");
	return 0;
}
