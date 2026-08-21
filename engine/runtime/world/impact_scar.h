#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "world/entity.h"

namespace opennova::world {

class World;
struct ProjectileHit;

// IMPACT SCARS — the marks ordinary ammo leaves on what it hits.
//
// These are NOT projected-volume decals. Retail keeps a 256-slot RING per
// struck entity; each slot is a two-triangle square centred on the impact,
// aligned to the struck polygon's normal, randomly spun about it, and drawn
// with the scar table's own texture and Q16 radius
// [orig: the impact fall-through Impact_SpawnGlassEffectsOrScar @0x5CF1B0;
//  the slot writer Scar_AddEntry @0x5CC830; the ring renderer
//  Scar_RenderCache @0x5CD830 (ex `terrain_render_sector_userpoints` — it
//  walks the 256 slots of the same per-entity cache: position at slot-20,
//  radius at slot-8, texture index at slot-4, owner at slot+0) under
//  Scar_RenderAllCaches @0x5CDF70; g_scarTable @0x8417A8 with its 32-byte
//  texture-name strip @0x8413A8].
//
// Glass-group shattering is a SEPARATE mechanism (the GLASS1..GLASS4 userpoint
// effects tried first @0x5CF217..0x5CF278, and the hit kind 2 that never
// reaches the scar @0x5CF289) and must not be replaced with this quad — a
// bullet hole in glass is not a small scorch mark.

// The ring: 256 slots of 64 bytes behind a 4-byte owner id, with the cursor
// dword after the slots [orig: the slot pointer `cache + 4 + (cursor << 6)`
// @0x5CC993; the advance Scar_AdvanceRingCursor @0x5CC1D0 —
// `++cursor; if (cursor >= 256) cursor = 0`, a compare-and-reset, not a mask].
// It is a ring, not a growing list: the 257th impact on one entity overwrites
// the first, which is what bounds the cost of a long firefight against one
// wall.
inline constexpr int kScarsPerEntity = 256;
inline constexpr int kScarSlotBytes = 64;

// The cache holds 128 entity rings and NEVER evicts [orig: Scar_GetEntityCache
// @0x5CC4C0 — 16392-byte entries from 0x29DB3C0 up to 0x2BDB7C0, looked up
// by owner pointer; a miss with no free entry returns null and the impact
// leaves NO scar @0x5CC979..0x5CC983; alloc 0 selects the terrain cache].
inline constexpr int kScarCacheEntityBytes = 4 + kScarsPerEntity * kScarSlotBytes + 4;
inline constexpr int kScarCacheEntities = 128; // (0x2BDB7C0 - 0x29DB3C0) / 16392

// The two scar ids ordinary fire selects between [orig: @0x5CF295 — hit
// record +0x58 == 15 -> id 18, else id 1; both calls pass the spin flag 1].
inline constexpr int kScarIdNormal = 1;
inline constexpr int kScarIdGlassFallback = 18;

// Radii are Q16 from the scar table's FOURTH column [orig: the row walk in
// Scar_RadiusForId @0x5CC3B0 (ex `Terrain_GetSurfaceFriction`) — the radius
// the ring renderer scales the quad axes by (the `radius * axis` products
// @0x5CD830); id 1 row @0x8417A8 = {1, 0, 3, 0x2000}, id 18 row @0x841898 =
// {18, 27, 27, 0x1000}; a missing id yields 0x10000 @0x5CC3D3].
inline constexpr int32_t kScarRadiusNormalQ16 = 0x2000;
inline constexpr int32_t kScarRadiusGlassFallbackQ16 = 0x1000;
inline constexpr int32_t kScarRadiusUnknownQ16 = 0x10000;

// Texture selection: the table's second and third columns are an inclusive
// [first, last] range into the 32-byte name strip [orig: Scar_TextureForId @0x5CC360
// — walks the 16-byte rows {id, first, last, radius} to the -1 terminator;
// draws `first + PRNG_Next16() % (last - first + 1)` ONLY when last - first > 0
// @0x5CC393]. Id 1 spans scorch1..scorch4 (strip 0..3); id 18 is bhole1.tga
// alone (strip 27).
inline constexpr int kScarNormalTextureFirst = 0;
inline constexpr int kScarNormalTextureCount = 4;
inline constexpr int kScarGlassFallbackTextureStrip = 27;

// Which scar an ordinary impact leaves.
inline int scar_id_for_surface(int32_t surface_type) {
	return surface_type == 15 ? kScarIdGlassFallback : kScarIdNormal;
}

// The scar's radius, Q16.
inline int32_t scar_radius_q16(int scar_id) {
	return scar_id == kScarIdGlassFallback ? kScarRadiusGlassFallbackQ16
	                                       : kScarRadiusNormalQ16;
}

// Texture index within the id's set. The fallback id has a single texture, so
// it never draws from the PRNG — only the normal scar does. Callers pass a
// fresh PRNG word only for the normal case; asking for one unconditionally
// would advance the SHARED stream on a path retail leaves alone and
// desynchronise every other consumer of it.
inline int scar_texture_index(int scar_id, uint16_t prng_word) {
	if (scar_id == kScarIdGlassFallback) return 0;
	return static_cast<int>(prng_word) % kScarNormalTextureCount;
}

// True when this scar id needs a PRNG draw for its texture.
inline bool scar_needs_texture_roll(int scar_id) {
	return scar_id != kScarIdGlassFallback;
}

// THE GATES the slot writer applies before it touches the ring
// [orig: Scar_AddEntry @0x5CC830]: the hit must sit above the water
// plane (`hit_z > Env_WaterHeightFixed` @0x5CC865), the struck entity must not
// be a husk (`Flags & 4` clear @0x5CC894 — the bit Flags |= 6 sets on death),
// and the struck face must not carry flag 0x400 (@0x5CC92B). The face flag is
// the collision model's business; the two entity-level gates are here.
inline bool scar_allowed(int32_t hit_z_q16, int32_t water_z_q16, bool entity_is_husk) {
	return hit_z_q16 > water_z_q16 && !entity_is_husk;
}

// The quad's spin about the polygon normal [orig: @0x5CCB42..0x5CCB7B —
// `PRNG_Next16() << 16` taken as a SIGNED BAM32, scaled by dbl_7C3608
// (2pi / 2^32) and fed to fsin/fcos, whose results are scaled by dbl_7C3600
// (2^22) for the Q22 rotation of the tangent pair]. One draw per scar, always
// — unlike the texture roll, every scar is randomly rotated. The shift makes
// the low word zero, so the angle is exactly word * 2pi / 65536 modulo 2pi.
inline float scar_spin_radians(uint16_t prng_word) {
	constexpr float kTau = 6.28318530717958647692f;
	return static_cast<float>(prng_word) * kTau / 65536.0f;
}

// The ring slot an impact lands in, given how many this entity has taken.
// Wraps rather than growing.
inline int scar_ring_slot(uint32_t impacts_so_far) {
	return static_cast<int>(impacts_so_far % static_cast<uint32_t>(kScarsPerEntity));
}

// THE TANGENT. Retail does not pick an axis and cross against it: it builds
// the tangent DIRECTLY by zeroing one component of the normal and swapping the
// other two with one sign flip, choosing which component to zero by a
// magnitude ladder [orig: @0x5CC9DA..0x5CCA71]. Reproduced case for case,
// including the |ny| == |nx| tie, which zeroes X whatever |nz| is:
//
//   |ny| <  |nx|, |nx| >  |nz|, |ny| >  |nz|  ->  ( ny, -nx,   0)   @0x5CC9E6
//   |ny| <  |nx|, |nx| >  |nz|, |ny| <= |nz|  ->  ( nz,   0, -nx)   @0x5CC9FB
//   |ny| <  |nx|, |nx| <= |nz|                ->  (-nz,   0,  nx)   @0x5CCA48
//   |ny| == |nx|                              ->  (  0, -nz,  ny)   @0x5CCA5D
//   |ny| >  |nx|, |ny| <= |nz|                ->  (  0, -nz,  ny)   @0x5CCA5D
//   |ny| >  |nx|, |ny| >  |nz|, |nx| >  |nz|  ->  (-ny,  nx,   0)   @0x5CCA19
//   |ny| >  |nx|, |ny| >  |nz|, |nx| <= |nz|  ->  (  0,  nz, -ny)   @0x5CCA2E
//
// The writer then normalises it (the fsqrt / 65536 block @0x5CCA74..0x5CCABB;
// a zero tangent stays zero @0x5CCAC2..0x5CCACF), crosses it with the normal
// for the bitangent @0x5CCADA, and flips the tangent when the triple product
// `normal . (bitangent x tangent)` is negative @0x5CCAE6..0x5CCB37. A "least
// aligned axis" pick would choose Z for the tie above and build a different
// basis from the one retail draws.
inline void scar_tangent(int32_t nx, int32_t ny, int32_t nz, int32_t out[3]) {
	const int32_t ax = nx < 0 ? -nx : nx;
	const int32_t ay = ny < 0 ? -ny : ny;
	const int32_t az = nz < 0 ? -nz : nz;
	if (ay < ax) {
		if (ax > az) {
			if (ay > az) { out[0] = ny; out[1] = -nx; out[2] = 0; }   // @0x5CC9E6
			else { out[0] = nz; out[1] = 0; out[2] = -nx; }          // @0x5CC9FB
		} else {
			out[0] = -nz; out[1] = 0; out[2] = nx;                   // @0x5CCA48
		}
		return;
	}
	if (ay == ax || ay <= az) {
		out[0] = 0; out[1] = -nz; out[2] = ny;                       // @0x5CCA5D
		return;
	}
	if (ax > az) { out[0] = -ny; out[1] = nx; out[2] = 0; }          // @0x5CCA19
	else { out[0] = 0; out[1] = nz; out[2] = -ny; }                  // @0x5CCA2E
}

// THE SCAR TEXTURE STRIP: 32-byte names at @0x8413A8, indexed by the slot's
// texture byte. Ordinary fire reaches strips 0..3 (scorch1..4) and 27 (bhole1);
// the other strips belong to scar ids no bullet selects and are not named here.
inline constexpr int kScarTextureStripCount = 32;
inline const char *scar_texture_strip_name(int strip) {
	switch (strip) {
	case 0: return "scorch1.tga";
	case 1: return "scorch2.tga";
	case 2: return "scorch3.tga";
	case 3: return "scorch4.tga";
	case 27: return "bhole1.tga";
	default: return "";
	}
}

// ---------------------------------------------------------------------------
// THE RING CACHE — the slot layout Scar_AddEntry @0x5CC830 writes and
// Scar_RenderCache @0x5CD830 reads. Relative to the slot's owner dword
// (`cache + 60 + 64*i`): owner ptr +0 (nonzero = live), texture index -4,
// radius Q16 -8, position Q16[3] -20, axis A Q16[3] -44, axis B Q16[3] -32,
// the is-building byte +4 and the bone index byte +5 (bone matrix =
// `bones + bone << 6` at render time).
// ---------------------------------------------------------------------------
struct ScarSlot {
	int32_t pos[3] = {0, 0, 0};    // Q16 [orig: slot-20]
	int32_t axis_a[3] = {0, 0, 0}; // Q16 unit tangent [orig: slot-44]
	int32_t axis_b[3] = {0, 0, 0}; // Q16 unit bitangent [orig: slot-32]
	int32_t radius_q16 = 0;        // [orig: slot-8]
	uint8_t texture = 0;           // strip index [orig: slot-4]
	uint8_t bone = 0;              // [orig: slot+5]
	bool building = false;         // [orig: slot+4]
	bool live = false;             // [orig: slot+0 owner nonzero]
};

// One entity's ring: 256 slots behind an owner and a cursor
// [orig: the 16392-byte cache entry; the cursor dword at +0x4004].
struct ScarRing {
	EntityHandle owner;   // invalid = the terrain ring (alloc 0)
	uint64_t lease = 0;   // the owner's registry spawn id at allocation
	bool in_use = false;
	uint32_t cursor = 0;  // [orig: Scar_AdvanceRingCursor @0x5CC1D0]
	std::array<ScarSlot, kScarsPerEntity> slots;

	void clear();
	// The witnessed advance: `++cursor; if (cursor >= 256) cursor = 0`.
	void advance_cursor();
};

// The 128-ring entity cache plus the terrain ring. Rings are keyed by the
// owner's handle AND its registry spawn id (retail keys by entity pointer
// with no eviction; a reused slot therefore inherited a dead owner's ring —
// the generation lease is the same fold LightScene applies: a handle reused
// by a new spawn gets a fresh ring, never the stale one).
class ScarCache {
public:
	ScarCache();

	// Find the owner's ring, leasing a free one on a miss. Null when every
	// ring is leased to another live owner — the impact then leaves NO scar
	// [orig: Scar_GetEntityCache @0x5CC4C0, the miss @0x5CC979..0x5CC983].
	ScarRing *ring_for(EntityHandle owner, uint64_t lease);
	const ScarRing *find(EntityHandle owner) const;
	ScarRing &terrain_ring() { return terrain_; }
	const ScarRing &terrain_ring() const { return terrain_; }
	const std::vector<ScarRing> &entity_rings() const { return rings_; }

	// The death clear [orig: Scar_ClearEntriesByEntity @0x5ccec0]: every slot
	// of the owner's ring is dropped and the ring is released.
	// WITNESS PENDING: whether retail releases the ring (owner dword zeroed)
	// or only its entries — ported as a release so dead owners do not starve
	// the 128-ring cache for the rest of the mission.
	void clear_entity(EntityHandle owner);
	void reset();
	int leased_count() const;

private:
	std::vector<ScarRing> rings_; // kScarCacheEntities
	ScarRing terrain_;
};

// Build the quad basis for a Q16 unit normal: the witnessed tangent ladder,
// normalised, crossed for the bitangent, handedness-fixed, then spun about the
// normal by the PRNG word [orig: Scar_AddEntry @0x5CC9DA..0x5CCB7B].
void scar_basis(const int32_t normal_q16[3], uint16_t spin_word,
		int32_t out_a[3], int32_t out_b[3]);

// Write one scar for a round stop. `target` is the struck item/building (its
// ring), or null for the terrain ring. Applies the gates in order (water
// plane, husk, face flag 0x400), picks the scar id from the hit's surface
// byte, draws the texture word (normal scar only) and the spin word from the
// SHARED mission stream, and appends at the ring cursor. Returns true when a
// slot was written. [orig: Impact_SpawnGlassEffectsOrScar @0x5CF1B0 ->
// Scar_AddEntry @0x5CC830]
bool scar_add_entry(World &world, const ProjectileHit &hit, const Entity *target);

} // namespace opennova::world
