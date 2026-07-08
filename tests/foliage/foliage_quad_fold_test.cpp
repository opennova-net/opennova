// The QUAD tier's witnessed corner/midpoint pairing and height fold
// [orig: Foliage_BuildPatchData @ 0x5C0240 (jodemo), retail family
// generate_foliage_instances_0 @ 0x600197; port libs/foliage/placement.cpp -
// byte-exact, untouchable]. Pins, hand-derived from the emitter code:
//
// - Corner order matches the model tier: c & 1 -> +-A (the local candidate-A
//   axis, world +X form), c & 2 -> +-B (world -Z form): c0=(-,-), c1=(+,-),
//   c2=(-,+), c3=(+,+).
// - The midpoint SAMPLE positions are NOT true edge midpoints - each mixes
//   the x of one corner pair with the z of another:
//     m0 = (x of (c0,c2), z of (c0,c1)),  m1 = (x of (c1,c3), z of (c2,c3)),
//     m2 = (x of (c0,c1), z of (c1,c2)),  m3 = (x of (c2,c3), z of (c0,c3)).
//   (At yaw 0 these degenerate to c0, c3, center, center.)
// - The HEIGHT fold pairs heights exactly like the model tier's edges:
//     D_-A = h(m0) - (h(c0)+h(c2))/2,  D_+A = h(m1) - (h(c1)+h(c3))/2,
//     D_-B = h(m2) - (h(c0)+h(c1))/2,  D_+B = h(m3) - (h(c2)+h(c3))/2,
//   and patch_control == (E_A, T_A, E_B, T_B) with E = (D_- + D_+)/2,
//   T = D_+ - E - the same fold family Foliage_GenerateModelTileInstances
//   @ 0x600980 feeds the grid-placement VS. The far-tier host consumes
//   patch_control directly as its ground-fit constants.
//
// Position literals below are float32-emulated hand computations for
// candidate 0 of cell key 0x00100020 (pack_cell_key(16u, 32u)), quad_half
// 2.0: seed 0xA56B1F0D, fracs (0xC38F, 0xABFA, 0xA256), center
// (1204225, 1952369) fixed. Corners carry a +-4 LSB libm-trig tolerance;
// the center and the midpoint-from-corner folds are exact.
#include <foliage/placement.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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

bool near_fixed(Fixed16_16 actual, Fixed16_16 pinned, const char *message) {
	if (std::abs(actual - pinned) <= 4) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s (actual %d, pinned %d)\n", message, actual, pinned);
	return false;
}

// A non-planar height field so every fold term carries signal. 16.16 fixed.
Fixed16_16 bumpy_height(Fixed16_16 wx, Fixed16_16 wz) {
	const double x = wx / 65536.0;
	const double z = wz / 65536.0;
	const double h = 2.5 * std::sin(x * 0.41) + 1.5 * std::cos(z * 0.59) + 0.0007 * x * z;
	return static_cast<Fixed16_16>(h * 65536.0);
}

} // namespace

int main() {
	// Record every height-sample position: 4 corners then 4 midpoints.
	std::vector<Fixed16_16> hx, hz;
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [&](Fixed16_16 wx, Fixed16_16 wz) -> Fixed16_16 {
		hx.push_back(wx);
		hz.push_back(wz);
		return bumpy_height(wx, wz);
	};

	PlacementConfig cfg{};
	for (int i = 0; i < FOLIAGE_MAX_DEFS; ++i) {
		cfg.quad_half_width[i] = 2.0f;
	}

	const uint32_t key = pack_cell_key(0x00100000, 0x00200000);
	if (!expect(key == 0x00100020u, "pack_cell_key(16u, 32u) == 0x00100020")) return 1;

	const auto r = place_cell(0, key, 0x00100000, 0x00200000, 0x40000000, cfg, s);
	if (!expect(r.count == FOLIAGE_CELL_CAP, "permissive cell fills to the cap")) return 1;
	// 9 samples per accepted instance: 4 corners, 4 midpoints, then the
	// centre height the port retains for the host adapter.
	if (!expect(hx.size() == static_cast<size_t>(r.count) * 9,
	            "9 height samples per instance (4 corners + 4 midpoints + centre)")) return 1;

	// --- Candidate-0 position pins (hand-computed) ---------------------------
	const PlacementInstance &inst = r.instances[0];
	if (!expect(inst.world_x_fixed == 1204225 && inst.world_z_fixed == 1952369,
	            "cand0 center fixed == (1204225, 1952369)")) {
		std::fprintf(stderr, "  actual (%d, %d)\n", inst.world_x_fixed, inst.world_z_fixed);
		return 1;
	}
	const Fixed16_16 pin_cx[4] = {1193604, 1019166, 1389284, 1214846};
	const Fixed16_16 pin_cz[4] = {1767310, 1962990, 1941748, 2137428};
	const Fixed16_16 pin_mx[4] = {1291444, 1117006, 1106385, 1302065};
	const Fixed16_16 pin_mz[4] = {1865150, 2039588, 1952369, 1952369};
	for (int c = 0; c < 4; ++c) {
		if (!near_fixed(hx[c], pin_cx[c], "corner sample x pin")) return 1;
		if (!near_fixed(hz[c], pin_cz[c], "corner sample z pin")) return 1;
	}
	for (int m = 0; m < 4; ++m) {
		if (!near_fixed(hx[4 + m], pin_mx[m], "midpoint sample x pin")) return 1;
		if (!near_fixed(hz[4 + m], pin_mz[m], "midpoint sample z pin")) return 1;
	}

	// --- The MIXED pairing, exact from the recorded corner samples -----------
	// x pairs: (0,2), (1,3), (0,1), (2,3); z pairs: (0,1), (2,3), (1,2), (0,3).
	const int x_pairs[4][2] = {{0, 2}, {1, 3}, {0, 1}, {2, 3}};
	const int z_pairs[4][2] = {{0, 1}, {2, 3}, {1, 2}, {0, 3}};
	for (size_t base = 0; base + 9 <= hx.size(); base += 9) {
		for (int m = 0; m < 4; ++m) {
			const Fixed16_16 want_x = static_cast<Fixed16_16>(
			    (hx[base + x_pairs[m][0]] + hx[base + x_pairs[m][1]]) >> 1);
			const Fixed16_16 want_z = static_cast<Fixed16_16>(
			    (hz[base + z_pairs[m][0]] + hz[base + z_pairs[m][1]]) >> 1);
			if (!expect(hx[base + 4 + m] == want_x && hz[base + 4 + m] == want_z,
			            "every midpoint samples at the witnessed MIXED pairing")) {
				std::fprintf(stderr, "  instance %zu mid %d: got (%d, %d) want (%d, %d)\n",
				             base / 8, m, hx[base + 4 + m], hz[base + 4 + m], want_x, want_z);
				return 1;
			}
		}
	}

	// --- The fold: patch_control == (E_A, T_A, E_B, T_B) ---------------------
	// Height pairing identical to the model tier: m0 -> -A (c0,c2),
	// m1 -> +A (c1,c3), m2 -> -B (c0,c1), m3 -> +B (c2,c3).
	constexpr float EPS = 1e-5f;
	for (int i = 0; i < r.count; ++i) {
		const PlacementInstance &pi = r.instances[i];
		float ch[4];
		float mh[4];
		for (int k = 0; k < 4; ++k) {
			ch[k] = static_cast<float>(pi.corner_y_fixed[k]) * FIXED_TO_FLOAT;
			mh[k] = static_cast<float>(pi.midpoint_y_fixed[k]) * FIXED_TO_FLOAT;
		}
		const float d_minus_a = mh[0] - (ch[0] + ch[2]) * 0.5f;
		const float d_plus_a = mh[1] - (ch[1] + ch[3]) * 0.5f;
		const float d_minus_b = mh[2] - (ch[0] + ch[1]) * 0.5f;
		const float d_plus_b = mh[3] - (ch[2] + ch[3]) * 0.5f;
		const float e_a = (d_minus_a + d_plus_a) * 0.5f;
		const float t_a = d_plus_a - e_a;
		const float e_b = (d_minus_b + d_plus_b) * 0.5f;
		const float t_b = d_plus_b - e_b;
		if (!expect(std::fabs(pi.patch_control[0] - e_a) < EPS &&
		                std::fabs(pi.patch_control[1] - t_a) < EPS &&
		                std::fabs(pi.patch_control[2] - e_b) < EPS &&
		                std::fabs(pi.patch_control[3] - t_b) < EPS,
		            "patch_control IS the (E_A, T_A, E_B, T_B) fold of the corner/midpoint heights")) {
			std::fprintf(stderr, "  inst %d: pc (%g %g %g %g) vs fold (%g %g %g %g)\n", i,
			             pi.patch_control[0], pi.patch_control[1], pi.patch_control[2],
			             pi.patch_control[3], e_a, t_a, e_b, t_b);
			return 1;
		}
	}

	std::printf("OK: quad-tier mixed midpoints + the (E_A, T_A, E_B, T_B) patch_control fold\n");
	return 0;
}
