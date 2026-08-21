// Impact-scar selection and ring policy.
// [orig: Impact_SpawnGlassEffectsOrScar @0x5CF1B0; Scar_AddEntry
//  @0x5CC830; Scar_TextureForId @0x5CC360; the scar table @0x8413A8/@0x8417A8]

#include <world/impact_scar.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

void test_scar_selection() {
	// Ordinary faces take the scorch; surface type 15 takes the small
	// bullet-hole fallback.
	CHECK(scar_id_for_surface(0) == kScarIdNormal, "type 0 takes the scorch");
	CHECK(scar_id_for_surface(3) == kScarIdNormal, "type 3 takes the scorch");
	CHECK(scar_id_for_surface(15) == kScarIdGlassFallback,
			"type 15 takes the fallback");
	// 14 and 16 must NOT — the fallback is one exact type, not a range.
	CHECK(scar_id_for_surface(14) == kScarIdNormal, "14 is not the fallback");
	CHECK(scar_id_for_surface(16) == kScarIdNormal, "16 is not the fallback");

	// Radii come from the scar table, and the fallback is HALF the scorch.
	CHECK(scar_radius_q16(kScarIdNormal) == 0x2000, "scorch radius");
	CHECK(scar_radius_q16(kScarIdGlassFallback) == 0x1000, "fallback radius");
	CHECK(scar_radius_q16(kScarIdGlassFallback) * 2 ==
					scar_radius_q16(kScarIdNormal),
			"the fallback mark is half the scorch");
}

// THE PRNG DISCIPLINE. The scorch picks one of four textures with a draw; the
// fallback has a single texture and takes NONE. That matters because the stream
// is SHARED — drawing unconditionally would advance it on a path retail leaves
// alone and desynchronise every other consumer.
void test_texture_roll_discipline() {
	CHECK(scar_needs_texture_roll(kScarIdNormal),
			"the scorch rolls for its texture");
	CHECK(!scar_needs_texture_roll(kScarIdGlassFallback),
			"the fallback does NOT roll — it has one texture");

	// The fallback ignores whatever word it is handed.
	CHECK(scar_texture_index(kScarIdGlassFallback, 0) == 0, "fallback index 0");
	CHECK(scar_texture_index(kScarIdGlassFallback, 65535) == 0,
			"fallback ignores the word entirely");

	// The scorch spreads across all four, and each is reachable.
	bool seen[kScarNormalTextureCount] = {};
	for (uint32_t w = 0; w < 64; ++w)
		seen[scar_texture_index(kScarIdNormal, uint16_t(w))] = true;
	for (int i = 0; i < kScarNormalTextureCount; ++i)
		CHECK(seen[i], "every scorch texture is reachable");
	CHECK(scar_texture_index(kScarIdNormal, 4) == 0, "wraps at the set size");
	CHECK(scar_texture_index(kScarIdNormal, 7) == 3, "and indexes within it");
}

// THE RING WRAPS. It is not a growing list: the 257th impact on one entity
// overwrites the first, which is what bounds a long firefight against one wall.
void test_ring_wraps() {
	CHECK(scar_ring_slot(0) == 0, "the first impact takes slot 0");
	CHECK(scar_ring_slot(255) == 255, "up to the last slot");
	CHECK(scar_ring_slot(256) == 0, "the 257th overwrites the first");
	CHECK(scar_ring_slot(257) == 1, "and the ring continues");
	CHECK(scar_ring_slot(1000) == 1000 % 256, "for any count");
	// The cache geometry the cursor lives in.
	CHECK(kScarCacheEntityBytes == 16392, "256 x 64 B slots + owner + cursor");
	CHECK(kScarCacheEntities == 128, "128 entity rings in the cache");
}

// The slot writer's gates: no scar below the water plane, none on a husk.
void test_gates() {
	CHECK(scar_allowed(0x20000, 0x10000, false), "above water, live entity");
	CHECK(!scar_allowed(0x10000, 0x10000, false), "at the water plane is NOT above it");
	CHECK(!scar_allowed(0x8000, 0x10000, false), "below water takes no scar");
	CHECK(!scar_allowed(0x20000, 0x10000, true), "a husk takes no scar");
}

// Every scar is spun about its normal — a full BAM16 turn mapped to radians.
void test_spin() {
	CHECK(scar_spin_radians(0) == 0.0f, "word 0 is no rotation");
	const float half = scar_spin_radians(32768);
	CHECK(half > 3.14f && half < 3.15f, "half the word range is half a turn");
	const float most = scar_spin_radians(65535);
	CHECK(most > 6.28f && most < 6.284f, "the top of the range is nearly a turn");
}

bool perpendicular(int32_t nx, int32_t ny, int32_t nz, const int32_t t[3]) {
	const int64_t dot = int64_t(nx) * t[0] + int64_t(ny) * t[1] + int64_t(nz) * t[2];
	const bool nonzero = t[0] != 0 || t[1] != 0 || t[2] != 0;
	return dot == 0 && nonzero;
}

// THE TANGENT LADDER, case for case. The tie |ny| == |nx| zeroes X even when
// Z is the smallest component — a "least aligned axis" pick would not.
void test_tangent_ladder() {
	int32_t t[3];
	// |ny| < |nx|, |nx| > |nz|, |ny| > |nz| -> (ny, -nx, 0)
	scar_tangent(10, 5, 1, t);
	CHECK(t[0] == 5 && t[1] == -10 && t[2] == 0, "case A: (ny, -nx, 0)");
	CHECK(perpendicular(10, 5, 1, t), "case A is perpendicular");
	// |ny| < |nx|, |nx| > |nz|, |ny| <= |nz| -> (nz, 0, -nx)
	scar_tangent(10, 1, 5, t);
	CHECK(t[0] == 5 && t[1] == 0 && t[2] == -10, "case B: (nz, 0, -nx)");
	CHECK(perpendicular(10, 1, 5, t), "case B is perpendicular");
	// |ny| < |nx|, |nx| <= |nz| -> (-nz, 0, nx)
	scar_tangent(5, 1, 10, t);
	CHECK(t[0] == -10 && t[1] == 0 && t[2] == 5, "case C: (-nz, 0, nx)");
	CHECK(perpendicular(5, 1, 10, t), "case C is perpendicular");
	// |ny| == |nx| -> (0, -nz, ny), WHATEVER |nz| is.
	scar_tangent(5, 5, 1, t);
	CHECK(t[0] == 0 && t[1] == -1 && t[2] == 5,
			"the tie zeroes X even though Z is the smallest component");
	CHECK(perpendicular(5, 5, 1, t), "the tie case is perpendicular");
	scar_tangent(-5, 5, 100, t);
	CHECK(t[0] == 0 && t[1] == -100 && t[2] == 5, "the tie compares magnitudes");
	// |ny| > |nx|, |ny| <= |nz| -> (0, -nz, ny)
	scar_tangent(1, 5, 10, t);
	CHECK(t[0] == 0 && t[1] == -10 && t[2] == 5, "case E: (0, -nz, ny)");
	CHECK(perpendicular(1, 5, 10, t), "case E is perpendicular");
	// |ny| > |nx|, |ny| > |nz|, |nx| > |nz| -> (-ny, nx, 0)
	scar_tangent(5, 10, 1, t);
	CHECK(t[0] == -10 && t[1] == 5 && t[2] == 0, "case F: (-ny, nx, 0)");
	CHECK(perpendicular(5, 10, 1, t), "case F is perpendicular");
	// |ny| > |nx|, |ny| > |nz|, |nx| <= |nz| -> (0, nz, -ny)
	scar_tangent(1, 10, 5, t);
	CHECK(t[0] == 0 && t[1] == 5 && t[2] == -10, "case G: (0, nz, -ny)");
	CHECK(perpendicular(1, 10, 5, t), "case G is perpendicular");

	// Axis-aligned normals — the common wall/floor case — never degenerate.
	scar_tangent(0x10000, 0, 0, t);
	CHECK(perpendicular(0x10000, 0, 0, t), "+X face");
	scar_tangent(0, 0x10000, 0, t);
	CHECK(perpendicular(0, 0x10000, 0, t), "+Y face");
	scar_tangent(0, 0, 0x10000, t);
	CHECK(perpendicular(0, 0, 0x10000, t), "+Z face");
	scar_tangent(-0x10000, 0, 0, t);
	CHECK(perpendicular(-0x10000, 0, 0, t), "-X face");
}

} // namespace

int main() {
	test_scar_selection();
	test_texture_roll_discipline();
	test_ring_wraps();
	test_gates();
	test_spin();
	test_tangent_ladder();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("impact_scar_test OK\n");
	return 0;
}
