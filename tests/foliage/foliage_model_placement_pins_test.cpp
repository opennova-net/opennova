// Pins the model-tier generator against hand-computed values (float32
// arithmetic emulated off-line, independent of the production code) for the
// reference tile snapped at (0x00100000, 0x00200000):
//   key      = (0x00100000 & 0x7FFF0000) | (((0x00200000 + 0x100000) >> 16) & 0x7FFF)
//            = 0x00100030 (keyHigh = 16 units, keyLow = 48 = 32 + 16)
//   seed     = (key & 0x1FF01FF) + ROL32(0xA55B1EED, key & 0x1F = 16) = 0x1EFDA58B
//   cand 0   : draws (jitterA, jitterB, yaw) = (0xE821, 0x635D, 0xC4D8)
//              localA = 1 + 0*2.6 + 0xE821*1.8/65536 = 2.6321564
//              localB = 1 + 0*2.6 + 0x635D*1.8/65536 = 1.698648
//              yaw    = 0xC4D8 * 2*pi/65536         = 4.8312726
//              center = ((16 + localA)*65536, (48 - localB)*65536)
//                     = (1221077, 3034405) fixed
// Corner / midpoint pins below use footprint F = 1.5 (bound radius 2.0 *
// the 0.75 footprint scale). Corner positions involve libm cos/sin, so they
// carry a +-4 fixed-LSB tolerance; the center pins are exact.
#include <foliage/model_placement.h>

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

} // namespace

int main() {
	// Key packing pin (incl. the +16u Z bias).
	const uint32_t key = pack_model_tile_key(0x00100000, 0x00200000);
	if (!expect(key == 0x00100030u, "pack_model_tile_key(16u, 32u) == 0x00100030")) return 1;

	// Record every height-sample position: 8 per accepted instance
	// (4 corners then the 4 edge midpoints -A, +A, -B, +B).
	std::vector<Fixed16_16> hx, hz;
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [&](Fixed16_16 wx, Fixed16_16 wz) -> Fixed16_16 {
		hx.push_back(wx);
		hz.push_back(wz);
		return 0;
	};

	ModelPlacementConfig cfg{};
	cfg.footprint[0] = 1.5f;  // = 2.0 bound radius * MODEL_FOOTPRINT_SCALE

	const auto r = generate_model_tile_instances(0, key, 0x00180000, 0x00280000,
	                                             0x40000000, cfg, s);
	if (!expect(r.count == MODEL_TILE_CAP, "permissive reference tile caps at 21")) return 1;
	if (!expect(hx.size() == static_cast<size_t>(r.count) * 8,
	            "exactly 8 height samples per accepted instance (4 corners + 4 midpoints)")) return 1;

	const ModelInstance &inst = r.instances[0];  // candidate 0

	// Center: pure float add/mul, exact.
	if (!expect(inst.center_x_fixed == 1221077, "cand0 center x fixed == 1221077")) {
		std::fprintf(stderr, "  actual %d\n", inst.center_x_fixed);
		return 1;
	}
	if (!expect(inst.center_z_fixed == 3034405, "cand0 center z fixed == 3034405")) {
		std::fprintf(stderr, "  actual %d\n", inst.center_z_fixed);
		return 1;
	}
	if (!expect(std::fabs(inst.yaw_radians - 4.8312726f) < 1e-5f, "cand0 yaw == 4.8312726")) return 1;

	// Corner world positions (k order; +-4 LSB libm tolerance).
	const Fixed16_16 pin_cx[4] = {1111807, 1135126, 1307027, 1330346};
	const Fixed16_16 pin_cz[4] = {2948454, 3143674, 2925136, 3120356};
	for (int k = 0; k < 4; ++k) {
		if (!near_fixed(inst.corner_x_fixed[k], pin_cx[k], "corner x pin")) return 1;
		if (!near_fixed(inst.corner_z_fixed[k], pin_cz[k], "corner z pin")) return 1;
	}

	// Height SAMPLE positions: calls 0..3 = the corners, 4..7 = the edge
	// midpoints at the integer midpoint of the fixed corner coords.
	for (int k = 0; k < 4; ++k) {
		if (!expect(hx[k] == inst.corner_x_fixed[k] && hz[k] == inst.corner_z_fixed[k],
		            "corner heights sample at the emitted corner positions")) return 1;
	}
	const Fixed16_16 pin_mx[4] = {1209417, 1232736, 1123466, 1318686};
	const Fixed16_16 pin_mz[4] = {2936795, 3132015, 3046064, 3022746};
	for (int m = 0; m < 4; ++m) {
		if (!near_fixed(hx[4 + m], pin_mx[m], "midpoint x sample pin")) return 1;
		if (!near_fixed(hz[4 + m], pin_mz[m], "midpoint z sample pin")) return 1;
	}
	// The midpoint sample positions are exactly the >>1 fold of the emitted corners.
	const int mid_pairs[4][2] = {{0, 2}, {1, 3}, {0, 1}, {2, 3}};
	for (int m = 0; m < 4; ++m) {
		const int a = mid_pairs[m][0];
		const int b = mid_pairs[m][1];
		if (!expect(hx[4 + m] == ((inst.corner_x_fixed[a] + inst.corner_x_fixed[b]) >> 1) &&
		                hz[4 + m] == ((inst.corner_z_fixed[a] + inst.corner_z_fixed[b]) >> 1),
		            "midpoints sample at ((c_a + c_b) >> 1) of the fixed corner coords")) return 1;
	}

	// The witnessed alpha-ref curve [orig: Terrain_RenderSectorEntitiesBySide
	// @ 0x5c7d50]: clamp(int(4096 / (dist_units + 1)), 8, 128).
	if (!expect(model_alpha_ref(0) == 128, "alpha_ref(0) clamps to 128")) return 1;
	if (!expect(model_alpha_ref(31) == 128, "alpha_ref(31) == 128 (4096/32, clamp boundary)")) return 1;
	if (!expect(model_alpha_ref(63) == 64, "alpha_ref(63) == 64 (exact mid value)")) return 1;
	if (!expect(model_alpha_ref(100) == 40, "alpha_ref(100) == 40 (int truncation)")) return 1;
	if (!expect(model_alpha_ref(511) == 8, "alpha_ref(511) == 8 (4096/512)")) return 1;
	if (!expect(model_alpha_ref(512) == 8, "alpha_ref(512) clamps to 8")) return 1;
	if (!expect(model_alpha_ref(100000) == 8, "alpha_ref(far) clamps to 8")) return 1;

	std::printf("OK: model placement pins (key, PRNG positions, corners, midpoints, alpha ref)\n");
	return 0;
}
