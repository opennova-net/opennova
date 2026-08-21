#pragma once

#include <cstdint>

namespace opennova::world {

// IMPACT SCARS — the marks ordinary ammo leaves on what it hits.
//
// These are NOT projected-volume decals. Retail keeps a 256-slot RING per
// struck entity; each slot is a two-triangle square centred on the impact,
// aligned to the struck polygon's normal, randomly spun about it, and drawn
// with the scar table's own texture and Q16 radius
// [orig: Scar_Apply @0x5CF1B0; Scar_SpawnQuad @0x5CC830;
//  Scar_BuildRenderQuads @0x5CD830; the scar table @0x8413A8/@0x8417A8].
//
// Glass-group shattering is a SEPARATE mechanism (scar_type 2, the clipped
// geometry path) and must not be replaced with this quad — a bullet hole in
// glass is not a small scorch mark.

// The ring wraps at 256 [orig: the cursor mask @0x5CC1D0]. It is a ring, not a
// growing list: the 257th impact on one entity overwrites the first, which is
// what bounds the cost of a long firefight against one wall.
inline constexpr int kScarsPerEntity = 256;

// The two scar ids ordinary fire selects between.
inline constexpr int kScarIdNormal = 1;
inline constexpr int kScarIdGlassFallback = 18;

// Radii are Q16 from the scar table [orig: id 1 @0x8417A8, id 18 @0x841898].
inline constexpr int32_t kScarRadiusNormalQ16 = 0x2000;
inline constexpr int32_t kScarRadiusGlassFallbackQ16 = 0x1000;

// The normal scar picks one of four scorch textures; the fallback has exactly
// one [orig: Scar_TextureForId @0x5CC360].
inline constexpr int kScarNormalTextureCount = 4;

// The surface type that takes the fallback scar. It reads as a glass id but is
// the NON-glass-group case: a face flagged 15 that did not match GLASS1..GLASS4
// still gets the small bullet-hole mark rather than the scorch
// [orig: the fall-through in check_glass_impact_and_spawn_effects @0x5CF1B0].
inline constexpr int32_t kScarSurfaceGlassFallback = 15;

// Which scar an ordinary impact leaves.
inline int scar_id_for_surface(int32_t surface_type) {
	return surface_type == kScarSurfaceGlassFallback ? kScarIdGlassFallback
	                                                 : kScarIdNormal;
}

// The scar's radius, Q16.
inline int32_t scar_radius_q16(int scar_id) {
	return scar_id == kScarIdGlassFallback ? kScarRadiusGlassFallbackQ16
	                                       : kScarRadiusNormalQ16;
}

// Texture index within the id's set. The fallback id has a single texture, so
// it never draws from the PRNG — only the normal scar does
// [orig: Scar_TextureForId @0x5CC360 -> PRNG_Next16 @0x6130A0, an inclusive
//  [first,last] selection]. Callers pass a fresh PRNG word only for the normal
// case; asking for one unconditionally would advance the SHARED stream on a
// path retail leaves alone and desynchronise every other consumer of it.
inline int scar_texture_index(int scar_id, uint16_t prng_word) {
	if (scar_id == kScarIdGlassFallback) return 0;
	return static_cast<int>(prng_word) % kScarNormalTextureCount;
}

// True when this scar id needs a PRNG draw for its texture.
inline bool scar_needs_texture_roll(int scar_id) {
	return scar_id != kScarIdGlassFallback;
}

// The quad's spin about the polygon normal, as a BAM16 turn mapped to radians
// [orig: Scar_SpawnQuad @0x5CCAC2 -> PRNG_Next16 @0x6130A0]. One draw per
// scar, always — unlike the texture roll, every scar is randomly rotated.
inline float scar_spin_radians(uint16_t prng_word) {
	constexpr float kTau = 6.28318530717958647692f;
	return static_cast<float>(prng_word) * kTau / 65536.0f;
}

// The ring slot an impact lands in, given how many this entity has taken.
// Wraps rather than growing.
inline int scar_ring_slot(uint32_t impacts_so_far) {
	return static_cast<int>(impacts_so_far % static_cast<uint32_t>(kScarsPerEntity));
}

// The tangent retail builds its quad basis from: the axis the normal is LEAST
// aligned with, so the cross product never degenerates
// [orig: the |x|/|y|/|z| comparison chain in Scar_SpawnQuad].
// Returns which axis to use: 0 = X, 1 = Y, 2 = Z.
inline int scar_basis_axis(float nx, float ny, float nz) {
	const float ax = nx < 0.0f ? -nx : nx;
	const float ay = ny < 0.0f ? -ny : ny;
	const float az = nz < 0.0f ? -nz : nz;
	if (ax <= ay && ax <= az) return 0;
	if (ay <= az) return 1;
	return 2;
}

} // namespace opennova::world
