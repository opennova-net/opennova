// Pins the detail tier's c24.x wind sway phase: the signed ms clock x 0.003f plus the
// weather oscillator's g_EnvWaveOscRing[0] x flt_7DE9D0 (0x35CCCCCD, the float
// nearest 1/655360), folded modulo 2 pi on the clock term, and the patch's
// sector origin the sway's v0.x is measured from
// [orig: Foliage_SetupVertexShaderConstants @ 0x60074a..0x60079d;
// Terrain_CollectNearFoliagePatches @ 0x603fc1].

#include <runtime/renderer/foliage_frame.h>

#include <cmath>
#include <cstdio>

namespace {
namespace r = opennova::renderer;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

bool near(float actual, float expected, float epsilon = 1e-6f) {
	return std::fabs(actual - expected) <= epsilon;
}

// The clock term alone: 1000 ms x 0.003 = 3.0 rad — the same vector the GUT
// adapter pin holds through the dispatcher's clock override
// (foliage_runtime_adapter_test.gd, phase 3.0 at 1000 ms).
void test_clock_term() {
	CHECK(near(r::foliage_detail_wind_phase(1000, 0), 3.0f));
	CHECK(near(r::foliage_detail_wind_phase(0, 0), 0.0f));
}

// The ring term scales by 1/655360 (flt_7DE9D0 = 0x35CCCCCD @ 0x600767), not
// 1/65536: 655360 -> +1.0 rad, 65536 -> +0.1 rad, sign-propagating.
void test_ring_term_scale_and_sign() {
	CHECK(near(r::foliage_detail_wind_phase(0, 655360), 1.0f));
	CHECK(near(r::foliage_detail_wind_phase(0, 65536), 0.1f));
	CHECK(near(r::foliage_detail_wind_phase(0, -327680), -0.5f));
	CHECK(near(r::foliage_detail_wind_phase(0, 6554), 0.0100006109f));
}

// The two terms add: a live oscillator shifts the clock phase, exactly.
void test_terms_add() {
	CHECK(near(r::foliage_detail_wind_phase(1000, 655360), 4.0f));
}

// The patch's D3D world translation is its collector sector origin, FB20 << 9
// on the Godot-Z axis: the cell's Z-min floored to 512 (negative keys floor
// down), so v0.x runs 0..512 across a sector.
void test_sector_origin_of_the_cell_key() {
	CHECK(r::foliage_detail_wind_sector_origin_z(0x00100030u) == 0.0f);
	CHECK(r::foliage_detail_wind_sector_origin_z(0x00100230u) == 512.0f);
	CHECK(r::foliage_detail_wind_sector_origin_z(0x02107e30u) == -512.0f);
	CHECK(r::foliage_detail_wind_sector_origin_z(0x00107e00u) == -512.0f);
	CHECK(r::foliage_detail_wind_sector_origin_z(0x00107c00u) == -1024.0f);
}

// The 2-pi fold keeps a long session's phase in [0, 2 pi) without losing the
// sine's float precision: at 2094395 ms the raw product is 6283.185 rad and
// the folded phase must be its double-math residue, not a degraded float. The
// scale is the float flt_7DE9D4 (0x3B449BA6 = 0.0030000000260770321), not the
// double 0.003: by three hours the two part by 2.8e-4 rad, by five days by 0.011
// [orig: `fmul flt_7DE9D4` @0x600757].
void test_two_pi_fold_precision() {
	const float folded = r::foliage_detail_wind_phase(2094395, 0);
	CHECK(near(folded, 6.28293274f));
	CHECK(folded >= 0.0f && folded < 6.2831854f);
	// Three hours in: still in range, still the exact residue.
	const float three_hours = r::foliage_detail_wind_phase(10800000, 0);
	CHECK(near(three_hours, 3.89683781f));
	CHECK(three_hours >= 0.0f && three_hours < 6.2831854f);
	// Five days in (432000000 ms): the float scale's residue, 0.011 rad from the
	// double scale's 5.06579991.
	CHECK(near(r::foliage_detail_wind_phase(432000000u, 0), 5.07706519f, 1e-5f));
}

// The DWORD is read signed (`fild` with no unsigned fix-up): past 2^31 ms of
// uptime the term is (tick - 2^32) x 0.003f, folded with its sign kept, where an
// unsigned read gives the opposite phase [orig: the store @0x60070C, the read
// @0x60074A, `fmul flt_7DE9D4` @0x600757].
void test_the_clock_is_read_signed() {
	CHECK(near(r::foliage_detail_wind_phase(0x80000000u, 0), -5.79483933f, 1e-5f));
	CHECK(near(r::foliage_detail_wind_phase(0xFFFFFFFFu, 0), -0.00300000003f));
}

} // namespace

int main() {
	test_clock_term();
	test_ring_term_scale_and_sign();
	test_terms_add();
	test_sector_origin_of_the_cell_key();
	test_two_pi_fold_precision();
	test_the_clock_is_read_signed();
	if (failures == 0) std::printf("foliage_frame_wind_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
