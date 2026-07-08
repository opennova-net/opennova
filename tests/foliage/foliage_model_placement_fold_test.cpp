// The fold identities of the 8-sample biquadratic ground fit
// [orig: Foliage_GenerateModelTileInstances @ 0x600980 (the fold);
// Foliage_GridPlacementVS via Terrain_CreateFoliageVertexShaders @ 0x5ff630
// (the sag polynomial)]:
//   - at the 4 corners the sag contribution is 0 (the fit is exact there);
//   - at each edge midpoint the biquadratic reproduces D_edge exactly, i.e.
//     the fitted surface passes through all 8 samples.
#include <foliage/model_placement.h>

#include <cmath>
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

// A deliberately non-planar, non-separable height field so every fold term
// carries signal. Returns 16.16 fixed.
Fixed16_16 bumpy_height(Fixed16_16 wx, Fixed16_16 wz) {
	const double x = wx / 65536.0;
	const double z = wz / 65536.0;
	const double h = 3.0 * std::sin(x * 0.37) + 2.0 * std::cos(z * 0.53) + 0.11 * x * z * 0.01;
	return static_cast<Fixed16_16>(h * 65536.0);
}

} // namespace

int main() {
	PlacementSamplers s;
	s.path_blocked = [](Fixed16_16, Fixed16_16, int32_t) { return false; };
	s.slot_mask_at = [](Fixed16_16, Fixed16_16) -> uint32_t { return 0xFu; };
	s.height_at = [](Fixed16_16 wx, Fixed16_16 wz) -> Fixed16_16 { return bumpy_height(wx, wz); };

	ModelPlacementConfig cfg{};
	cfg.footprint[0] = 1.5f;

	const uint32_t key = pack_model_tile_key(0x00100000, 0x00200000);
	const auto r = generate_model_tile_instances(0, key, 0x00180000, 0x00280000,
	                                             0x40000000, cfg, s);
	if (!expect(r.count > 0, "the fold fixture places instances")) return 1;

	constexpr float EPS = 1e-4f;
	// The normalized coords of the 4 corners and the 4 edge midpoints in the
	// engine convention (x_n along A, z_n along B; corner k bit0 -> x_n,
	// bit1 -> z_n).
	const float corner_xn[4] = {0.0f, 1.0f, 0.0f, 1.0f};
	const float corner_zn[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	// Edges: -A = mid(c0,c2) at (0, .5); +A = mid(c1,c3) at (1, .5);
	//        -B = mid(c0,c1) at (.5, 0); +B = mid(c2,c3) at (.5, 1).
	const int mid_pairs[4][2] = {{0, 2}, {1, 3}, {0, 1}, {2, 3}};
	const float mid_xn[4] = {0.0f, 1.0f, 0.5f, 0.5f};
	const float mid_zn[4] = {0.5f, 0.5f, 0.0f, 1.0f};

	for (int i = 0; i < r.count; ++i) {
		const ModelInstance &inst = r.instances[i];

		// Corner identity: ground fit == the sampled corner height (sag = 0).
		for (int k = 0; k < 4; ++k) {
			const float fit = model_ground_height(inst, corner_xn[k], corner_zn[k]);
			if (!expect(std::fabs(fit - inst.corner_height[k]) < EPS,
			            "the fit is exact at the corners")) {
				std::fprintf(stderr, "  inst %d corner %d: fit %f vs sampled %f\n",
				             i, k, fit, inst.corner_height[k]);
				return 1;
			}
		}

		// Edge-midpoint identity: the biquadratic reproduces each midpoint
		// height sample (bilinear gives the corner mean; the sag adds D_edge).
		for (int m = 0; m < 4; ++m) {
			const int a = mid_pairs[m][0];
			const int b = mid_pairs[m][1];
			const Fixed16_16 mx = static_cast<Fixed16_16>(
			    (inst.corner_x_fixed[a] + inst.corner_x_fixed[b]) >> 1);
			const Fixed16_16 mz = static_cast<Fixed16_16>(
			    (inst.corner_z_fixed[a] + inst.corner_z_fixed[b]) >> 1);
			const float h_mid = static_cast<float>(bumpy_height(mx, mz)) * FIXED_TO_FLOAT;
			const float fit = model_ground_height(inst, mid_xn[m], mid_zn[m]);
			if (!expect(std::fabs(fit - h_mid) < EPS,
			            "the fit reproduces each edge-midpoint sample exactly")) {
				std::fprintf(stderr, "  inst %d edge %d: fit %f vs sampled %f\n", i, m, fit, h_mid);
				return 1;
			}
		}

		// hbase consistency: the helper equals the fit evaluated at the center.
		const float hbase = model_instance_hbase(inst);
		const float fit_center = model_ground_height(inst, 0.5f, 0.5f);
		if (!expect(std::fabs(hbase - fit_center) < EPS,
		            "model_instance_hbase == the fit at (0.5, 0.5)")) return 1;
	}

	std::printf("OK: the biquadratic fold passes through all 8 ground samples\n");
	return 0;
}
