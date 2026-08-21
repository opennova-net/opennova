// Impact-scar selection and ring policy.
// [orig: Scar_Apply @0x5CF1B0; Scar_SpawnQuad @0x5CC830; Scar_TextureForId
//  @0x5CC360; the scar table @0x8413A8/@0x8417A8]

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
}

// Every scar is spun about its normal — a full BAM16 turn mapped to radians.
void test_spin() {
	CHECK(scar_spin_radians(0) == 0.0f, "word 0 is no rotation");
	const float half = scar_spin_radians(32768);
	CHECK(half > 3.14f && half < 3.15f, "half the word range is half a turn");
	const float most = scar_spin_radians(65535);
	CHECK(most > 6.28f && most < 6.284f, "the top of the range is nearly a turn");
}

// The quad basis takes the axis the normal is LEAST aligned with, so the cross
// product cannot degenerate on an axis-aligned face — the common case for a
// shot into a wall or floor.
void test_basis_axis() {
	CHECK(scar_basis_axis(1.0f, 0.0f, 0.0f) != 0,
			"a +X normal must not build its basis from X");
	CHECK(scar_basis_axis(0.0f, 1.0f, 0.0f) != 1,
			"a +Y normal must not build its basis from Y");
	CHECK(scar_basis_axis(0.0f, 0.0f, 1.0f) != 2,
			"a +Z normal must not build its basis from Z");
	// Sign must not matter — a wall facing either way behaves the same.
	CHECK(scar_basis_axis(-1.0f, 0.0f, 0.0f) ==
					scar_basis_axis(1.0f, 0.0f, 0.0f),
			"the axis pick is sign-independent");
	// A tilted normal picks its smallest component.
	CHECK(scar_basis_axis(0.9f, 0.1f, 0.5f) == 1, "picks the smallest component");
}

} // namespace

int main() {
	test_scar_selection();
	test_texture_roll_discipline();
	test_ring_wraps();
	test_spin();
	test_basis_axis();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("impact_scar_test OK\n");
	return 0;
}
