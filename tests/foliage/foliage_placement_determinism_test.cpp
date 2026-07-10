#include <foliage/placement.h>

#include <cstring>
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

uint32_t float_bits(float value) {
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
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

	const auto r1 = place_cell(0, key_a, cfg, samplers);
	const auto r2 = place_cell(0, key_a, cfg, samplers);
	if (!expect(same_result(r1, r2), "same (slot, cell_key) must yield identical placements")) return 1;
	if (!expect(r1.count > 0, "permissive samplers must place at least one instance")) return 1;

	// Independent float32 golden for candidate 0 of key 0x00100020.
	// [orig: generate_foliage_instances_0 @ 0x5ffdd0]
	// seed=0xA56B1F0D; the first three already-^1-folded states expose
	// low16 draws C38E, ABFB, A257 for jitter X, jitter Z, and yaw.
	const PlacementInstance &candidate0 = r1.instances[0];
	if (!expect(candidate0.world_x_fixed == 1204223,
	            "candidate 0 fixed X matches the retail PRNG golden")) return 1;
	if (!expect(candidate0.world_z_fixed == 1952367,
	            "candidate 0 fixed Z matches the retail PRNG golden")) return 1;
	if (!expect(float_bits(candidate0.rotation_radians) == 0x407F00ACu,
	            "candidate 0 yaw matches the retail float32 golden")) return 1;

	const auto r3 = place_cell(0, key_b, cfg, samplers);
	if (!expect(!same_result(r1, r3), "different cell keys must produce different placements")) return 1;

	// Slot selection changes acceptance gates, not the deterministic PRNG stream.
	// This permissive fixture only needs to remain within the FAR capacity.
	const auto r4 = place_cell(1, key_a, cfg, samplers);
	if (!expect(r4.count >= 0 && r4.count <= FAR_CELL_CAP, "slot 1 output within bounds")) return 1;

	std::printf("OK: foliage placement is deterministic per (slot, cell_key)\n");
	return 0;
}
