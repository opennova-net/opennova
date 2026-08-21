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
// pool-1 item/vehicle plus one SHARED ring for everything else; each slot is
// a two-triangle square centred on the impact, aligned to the struck polygon's
// normal, randomly spun about it, and drawn with the scar table's own texture
// and Q16 radius
// [orig: the impact processor AmmoDef_ProcessImpactEffect @0x40a24e..0x40a264
//  -> Impact_SpawnGlassEffectsOrScar @0x5CF1B0 (kind = the ammo's `scar_type`
//  word +0x76); the slot writer Scar_AddEntry @0x5CC830; the ring renderer
//  Scar_RenderCache @0x5CD830 under Scar_RenderAllCaches @0x5CDF70;
//  g_scarTable @0x8417A8 with its 32-byte texture-name strip @0x8413A8].
//
// The GLASS userpoint leg is a SEPARATE mechanism: when the struck model has
// a GLASS userpoint of the 24-row surface-material table @0x841980 within the
// row's radius of the hit, Terrain_SpawnSurfaceEffectsAtUserPoints @0x5CEA90
// takes the projected-decal path (scar_project_decal_onto_entity @0x5CE4A0)
// plus the four effects rolled on the table's MAIN probability column, and the
// ring scar is SKIPPED. RESIDUAL (the one open item of this port): that
// table's rows are not witnessed in full, so the port cannot detect the
// userpoint match and writes the ring scar on such a hit instead.

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
// leaves NO scar @0x5CC979..0x5CC983]. The shared ring (alloc 0) is the
// separate block at 0x2BDB7C0.
inline constexpr int kScarCacheEntityBytes = 4 + kScarsPerEntity * kScarSlotBytes + 4;
inline constexpr int kScarCacheEntities = 128; // (0x2BDB7C0 - 0x29DB3C0) / 16392

// THE AMMO KIND — `scar_type` [orig: Impact_SpawnGlassEffectsOrScar gates
// `kind != 0` at entry, and kind 2 skips the ring fallback @0x5cf289]: 0 = no
// mark at all, 1 = the ordinary ring scar, 2 = glass-only (the decal leg
// without the fallback). Shipped ammo.def authors 0, 1 and 2.
inline constexpr int kScarKindNone = 0;
inline constexpr int kScarKindGlassOnly = 2;
inline bool scar_kind_takes_ring_scar(int scar_type) {
	return scar_type != kScarKindNone && scar_type != kScarKindGlassOnly;
}

// THE DEF GATE [orig: @0x5cf1f7 — `!(Flags & 1) && (def+92 == 1 ||
// !(def+84 & 0x10000000))`]: a vehicle def (type 1) always takes a scar; any
// other def only without the NoScar attribute (def.h DEF_ITEM_ATTRIB_NOSCAR).
inline constexpr uint32_t kScarGateEntityFlag = 0x1u;
inline constexpr uint32_t kItemAttribNoScar = 0x10000000u;
inline constexpr int kScarItemTypeVehicle = 1;
inline constexpr int kScarItemTypeBuilding = 5;
inline bool scar_entity_allowed(uint32_t entity_flags, int item_type, uint32_t item_attrib) {
	if ((entity_flags & kScarGateEntityFlag) != 0u) return false;
	return item_type == kScarItemTypeVehicle || (item_attrib & kItemAttribNoScar) == 0u;
}

// RING SELECTION [orig: Scar_AddEntry @0x5cc873..0x5cc88c — `isEntityLocal =
// Pool_GetIndexFromPtr(1, entity) >= 0 || (def+84 byte0 & 0x80)`]: a pool-1
// item/vehicle (or a def with attrib bit 0x80) owns a per-entity ring whose
// slots live in SECTION-LOCAL space, so they follow the moving carrier;
// buildings (pool 2) and persons (pool 0) write the shared WORLD ring in world
// space with the rotated normal.
inline constexpr uint32_t kItemAttribScarEntityLocal = 0x80u;
inline bool scar_uses_entity_ring(int pool, uint32_t item_attrib) {
	return pool == 1 || (item_attrib & kItemAttribScarEntityLocal) != 0u;
}

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

// The quad's spin about the polygon normal [orig: @0x5CCB50..0x5CCC65 —
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
// The writer then normalises it (the fsqrt / 65536 block @0x5CCA74..0x5CCAD7),
// crosses it with the normal for the bitangent @0x5CCADA, and flips the
// tangent when the triple product `normal . (bitangent x tangent)` is negative
// @0x5CCAE6..0x5CCB37. A "least aligned axis" pick would choose Z for the tie
// above and build a different basis from the one retail draws.
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

// THE SCAR TEXTURE STRIP: 32-byte entries at @0x8413A8 {name[16], tex@16,
// effect@20, modeId@24, loadFlags@28}, indexed by the slot's texture byte
// [orig: Scar_LoadTextures @0x5CC2E0]. Ordinary fire reaches strips 0..3
// (scorch1..4, mode 0, flags 0) and 27 (bhole1, mode 1, flags 0); the other
// strips belong to the glass/wood hole ids no bullet selects and are not
// named here.
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
// THE RING CACHE — the 64-byte slot Scar_AddEntry @0x5CC830 writes and
// Scar_RenderCache @0x5CD830 reads: normal @0, tangent @12, bitangent @24,
// position @36 (all Q16[3]), radius @48, texture strip @52, owner @56,
// isBuilding @60, bone (the struck section index) @61. A per-entity ring's
// slots are SECTION-LOCAL (the renderer transforms them through the model
// callback's bone matrix); the shared ring's are world space.
// ---------------------------------------------------------------------------
struct ScarSlot {
	int32_t normal[3] = {0, 0, 0}; // Q16 [orig: @0]
	int32_t axis_a[3] = {0, 0, 0}; // Q16 unit tangent [orig: @12]
	int32_t axis_b[3] = {0, 0, 0}; // Q16 unit bitangent [orig: @24]
	int32_t pos[3] = {0, 0, 0};    // Q16 [orig: @36]
	int32_t radius_q16 = 0;        // [orig: @48]
	uint8_t texture = 0;           // strip index [orig: @52]
	EntityHandle owner;            // the struck entity [orig: @56]
	bool building = false;         // def type 5 [orig: @60]
	uint8_t bone = 0;              // the struck section index [orig: @61]
	bool live = false;             // [orig: owner nonzero]
};

// One ring: 256 slots behind an owner and a cursor
// [orig: the 16392-byte cache entry; the cursor dword at +0x4004].
struct ScarRing {
	EntityHandle owner;   // invalid = the shared world ring (alloc 0)
	uint64_t lease = 0;   // the owner's registry spawn id at allocation
	bool in_use = false;
	uint32_t cursor = 0;  // [orig: Scar_AdvanceRingCursor @0x5CC1D0]
	std::array<ScarSlot, kScarsPerEntity> slots;

	void clear();
	// The witnessed advance: `++cursor; if (cursor >= 256) cursor = 0`.
	void advance_cursor();
};

// The 128-ring entity cache plus the shared world ring. Entity rings are
// keyed by the owner's handle AND its registry spawn id (retail keys by entity
// pointer with no eviction; a reused slot therefore inherited a dead owner's
// ring — the generation lease is the same fold LightScene applies: a handle
// reused by a new spawn gets a fresh ring, never the stale one).
class ScarCache {
public:
	ScarCache();

	// Find the owner's ring, leasing a free one on a miss. Null when every
	// ring is leased to another live owner — the impact then leaves NO scar
	// [orig: Scar_GetEntityCache @0x5CC4C0, the miss @0x5CC979..0x5CC983].
	ScarRing *ring_for(EntityHandle owner, uint64_t lease);
	const ScarRing *find(EntityHandle owner) const;
	// The shared ring buildings and persons write [orig: dword_2BDB7C0, alloc 0].
	ScarRing &world_ring() { return world_; }
	const ScarRing &world_ring() const { return world_; }
	const std::vector<ScarRing> &entity_rings() const { return rings_; }

	// The death clear [orig: Scar_ClearEntriesByEntity @0x5ccec0 — zeroes the
	// owner dword of every shared-ring slot this entity wrote (256 slots at
	// 0x2BDB7FC, stride 64) and memsets the entity's own ring, releasing it;
	// called from Entity_Destroy @0x43e8e4 and Entity_AttachToVehicle @0x43c155].
	void clear_entity(EntityHandle owner);
	void reset();
	int leased_count() const;

private:
	std::vector<ScarRing> rings_; // kScarCacheEntities
	ScarRing world_;
};

// Build the quad basis for a Q16 unit normal: the witnessed tangent ladder,
// normalised, crossed for the bitangent, handedness-fixed, then spun about the
// normal by the PRNG word [orig: Scar_AddEntry @0x5CC9DA..0x5CCC65].
void scar_basis(const int32_t normal_q16[3], uint16_t spin_word,
		int32_t out_a[3], int32_t out_b[3]);

// Write one scar for a round stop on `target` with the ammo's `scar_type`.
// Applies the kind and def gates, the water/husk/face gates, selects the ring
// by pool, stores the slot in the ring's frame (section-local through the
// entity's live section matrix for entity rings, world space for the shared
// ring), draws the spin word and then — for the normal scar only — the texture
// word from the SHARED mission stream, and appends at the ring cursor. Returns
// true when a slot was written. [orig: Impact_SpawnGlassEffectsOrScar @0x5CF1B0
// -> Scar_AddEntry @0x5CC830]
bool scar_add_entry(World &world, const ProjectileHit &hit, const Entity &target,
		int scar_type);

} // namespace opennova::world
