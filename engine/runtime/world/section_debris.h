#pragma once

#include <cstdint>

namespace opennova::world {

// SECTION DEBRIS — the burst of effects a destroyed model section throws off
// [orig: Entity_SpawnSectionDebris @0x43F580].
//
// Retail does not emit one effect per face. It walks each section's faces at a
// FIXED STRIDE and emits at the sampled faces only, so a 40-face crate and a
// 4000-face building both cost about the same. This header carries the
// sampling policy and the throw math; the walk itself needs the collision
// model.

// The sampler targets 150 samples across the model, and the stride is carried
// in 8.8 FIXED so a model with fewer faces than that still advances by less
// than one face per step rather than stalling
// [orig: (collisionModel+0x60 << 8) / 0x96 @0x43F5B4, where +0x60 is the CFAC
//  face count].
inline constexpr int32_t kDebrisTargetSamples = 150; // 0x96

inline int64_t debris_stride_q8(int32_t face_count) {
	if (face_count <= 0) return 0;
	return (static_cast<int64_t>(face_count) << 8) / kDebrisTargetSamples;
}

// Model-local face vertices are Q8; the centroid averages the three and shifts
// back to 16.16 [orig: the /3 then << 8 @0x43F5D0..0x43F60C].
//
// The DIVIDE COMES FIRST. Averaging in Q8 and then shifting is not the same as
// shifting and then averaging: the former truncates a third of a Q8 unit per
// axis, which is what retail's output carries.
inline int32_t debris_centroid_axis(int32_t v0, int32_t v1, int32_t v2) {
	return ((v0 + v1 + v2) / 3) << 8;
}

// THE INTEGER DISTANCE APPROXIMATION used for the throw pitch
// [orig: @0x43F61E..0x43F6A2]: max + (min >> 4) * 5, i.e. roughly
// max + 0.3125 * min. It is not a hypotenuse and must not be replaced with
// one — the pitch it feeds would change for every off-axis fragment.
inline int32_t debris_distance_approx(int32_t dx, int32_t dy) {
	const int32_t adx = dx < 0 ? -dx : dx;
	const int32_t ady = dy < 0 ? -dy : dy;
	return ady < adx ? adx + ((ady >> 4) * 5) : ady + ((adx >> 4) * 5);
}

// With NO recorded blast the fragment is thrown radially out from the entity
// at a fixed rise, rather than at some computed angle
// [orig: the constant pitch 0x2CFFFFD3 @0x43F5E9] — about 63.3 degrees in
// BAM32.
inline constexpr int32_t kDebrisDefaultPitchBam = 0x2CFFFFD3;

// Material 17 is FOLIAGE. It takes the foliage effect and spawns OWNED by the
// entity; every other material takes the wood effect, unowned
// [orig: the material test and the g_fx_TreeFoliageExp @0x2C25BF0 /
//  g_fx_TreeWoodExp @0x2C25BF4 pick @0x43F6D8..0x43F6F4].
//
// The ownership difference is not cosmetic: an owned effect dies with its
// entity, an unowned one outlives it.
inline constexpr int32_t kDebrisFoliageMaterial = 17;

inline bool debris_is_foliage(int32_t material) {
	return material == kDebrisFoliageMaterial;
}

// Foliage debris is owned by the spawning entity; wood debris is not.
inline bool debris_is_owned(int32_t material) {
	return debris_is_foliage(material);
}

} // namespace opennova::world
