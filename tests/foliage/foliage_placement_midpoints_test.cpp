#include <foliage/placement.h>

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using namespace opennova;
using namespace opennova::foliage;

namespace {

struct SampleCall {
	Fixed16_16 x = 0;
	Fixed16_16 z = 0;
};

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

SampleCall midpoint(const SampleCall &a, const SampleCall &b) {
	return {
	    static_cast<Fixed16_16>((a.x + b.x) >> 1),
	    static_cast<Fixed16_16>((a.z + b.z) >> 1),
	};
}

float fixed_to_float(Fixed16_16 value) {
	return static_cast<float>(value) * FIXED_TO_FLOAT;
}

bool near(float a, float b, float eps = 1.0e-5f) {
	return std::fabs(a - b) <= eps;
}

} // namespace

int main() {
	PlacementConfig config{};
	for (int i = 0; i < FOLIAGE_MAX_DEFS; ++i) {
		config.attrib_flags[i] = FOLIAGE_ATTRIB_FORCE_ON;
		config.quad_half_width[i] = 2.0f;
	}

	std::vector<SampleCall> calls;
	PlacementSamplers samplers;
	samplers.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	samplers.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	samplers.height_at = [&calls](Fixed16_16 x, Fixed16_16 z) -> Fixed16_16 {
		calls.push_back({x, z});
		return static_cast<Fixed16_16>((x >> 5) + (z >> 7));
	};

	const uint32_t cell_key = pack_cell_key(0x00100000, 0x00200000);
	const PlacementResult result =
	    place_cell(0, cell_key, 0x00180000, 0x00180000, 0x40000000, config, samplers);

	if (!expect(result.count > 0, "placement should accept at least one candidate")) return 1;
	if (!expect(calls.size() >= 9, "first accepted instance should emit 4 corners + 4 midpoints + centre")) return 1;

	const SampleCall c0 = calls[0];
	const SampleCall c1 = calls[1];
	const SampleCall c2 = calls[2];
	const SampleCall c3 = calls[3];
	const SampleCall m0 = calls[4];
	const SampleCall m1 = calls[5];
	const SampleCall m2 = calls[6];
	const SampleCall m3 = calls[7];

	const SampleCall expected_midpoints[4] = {
	    {static_cast<Fixed16_16>((c0.x + c2.x) >> 1), static_cast<Fixed16_16>((c0.z + c1.z) >> 1)},
	    {static_cast<Fixed16_16>((c1.x + c3.x) >> 1), static_cast<Fixed16_16>((c2.z + c3.z) >> 1)},
	    {static_cast<Fixed16_16>((c0.x + c1.x) >> 1), static_cast<Fixed16_16>((c1.z + c2.z) >> 1)},
	    {static_cast<Fixed16_16>((c2.x + c3.x) >> 1), static_cast<Fixed16_16>((c0.z + c3.z) >> 1)},
	};
	const SampleCall actual_midpoints[4] = {m0, m1, m2, m3};
	for (int i = 0; i < 4; ++i) {
		if (!expect(actual_midpoints[i].x == expected_midpoints[i].x &&
		                actual_midpoints[i].z == expected_midpoints[i].z,
		            "midpoint sample coordinates should follow the engine's asymmetric pairing")) {
			return 1;
		}
	}

	const SampleCall legacy_edge_midpoints[4] = {
	    midpoint(c0, c1),
	    midpoint(c2, c3),
	    midpoint(c0, c2),
	    midpoint(c1, c3),
	};
	bool differs_from_legacy = false;
	for (int i = 0; i < 4; ++i) {
		if (actual_midpoints[i].x != legacy_edge_midpoints[i].x ||
		    actual_midpoints[i].z != legacy_edge_midpoints[i].z) {
			differs_from_legacy = true;
			break;
		}
	}
	if (!expect(differs_from_legacy, "midpoint layout should not collapse back to the old edge-midpoint approximation")) return 1;

	const PlacementInstance &inst = result.instances[0];
	const float corner0 = fixed_to_float(inst.corner_y_fixed[0]);
	const float corner1 = fixed_to_float(inst.corner_y_fixed[1]);
	const float corner2 = fixed_to_float(inst.corner_y_fixed[2]);
	const float corner3 = fixed_to_float(inst.corner_y_fixed[3]);
	const float hm0 = fixed_to_float(inst.midpoint_y_fixed[0]);
	const float hm1 = fixed_to_float(inst.midpoint_y_fixed[1]);
	const float hm2 = fixed_to_float(inst.midpoint_y_fixed[2]);
	const float hm3 = fixed_to_float(inst.midpoint_y_fixed[3]);
	const float edge_bottom = hm1 - (corner3 + corner1) * 0.5f;
	const float edge_right = hm3 - (corner3 + corner2) * 0.5f;
	float expected_control[4] = {
	    (hm0 - (corner2 + corner0) * 0.5f + edge_bottom) * 0.5f,
	    0.0f,
	    (hm2 - (corner1 + corner0) * 0.5f + edge_right) * 0.5f,
	    0.0f,
	};
	expected_control[1] = edge_bottom - expected_control[0];
	expected_control[3] = edge_right - expected_control[2];
	for (int i = 0; i < 4; ++i) {
		if (!expect(near(inst.patch_control[i], expected_control[i]),
		            "patch_control should match Foliage_BuildPatchData curvature writes")) {
			return 1;
		}
	}

	std::printf("OK: foliage midpoint samples and patch controls follow Foliage_BuildPatchData\n");
	return 0;
}
