#pragma once

#include <cstdint>

namespace opennova::world {

// THE IN-FLIGHT ROUND EFFECT's LIFECYCLE — when the `move`-row emitter
// (round +0x1CC) is spawned, re-posed, released and detached.
//
// The effect itself (effects_table tag 1) is already published to the shell.
// What lives here is the part that decides how long the EMITTER lives, which
// is where the behaviour is: a plume that lingers after impact reads
// completely differently from one that vanishes with the round.
//
// The round's OWN life is not here: +0x2AC is seeded from the ammo's max_age
// [orig: @0x4EC679], decremented each aging tick [orig: @0x4E9F37..0x4E9F4E,
//  skipped under ammo flag 0x4000 `noage` @0x4E9F2F] and retired at <= 0 by
// the Projectile_UpdatePhysics head [orig: @0x4E9DA7..0x4E9DAE] — so a
// max_age of 0 retires the round on its first tick and "never times out" is
// the noage flag. That leg is the LIVE world/round_sim.cpp port
// (`max_age_ticks`, `kAmmoFlagNoAge`, the expiry test).
//
// STAGED, NOT WIRED: the shell's fire presentation owns the emitters; these
// predicates are what it will consult.

// The emitter is spawned LAZILY [orig: the guided leg @0x4E9F58..0x4E9F94;
//  the ballistic leg @0x4EA8AE..]: an authored effect (ammo +0x70) @0x4E9F58,
// no handle yet (+0x1CC == 0) @0x4E9F63, life still nonzero (+0x2AC != 0)
// @0x4E9F70 — the decrement runs before this test, so a round retiring next
// tick spawns nothing — and NOT clipped by water, the same ClipWaterFx + z
// test as the release @0x4E9F7D..0x4E9F8E. All four must hold, so a round
// can never quietly acquire a second emitter.
inline bool round_effect_should_spawn(bool has_effect, bool has_handle,
		int32_t life_ticks, bool clipped_by_water) {
	return has_effect && !has_handle && life_ticks != 0 && !clipped_by_water;
}

// A round carrying ClipWaterFx whose z reaches the water plane RELEASES its
// emitter instead of re-posing it [orig: @0x4EA01D..0x4EA036 — `z >
//  Env_WaterHeightFixed` re-poses via CEffect_UpdateEmitterTransform
//  @0x4EA039, else ammo flags (+0x114) & 0x20000000 detaches via
//  Entity_ReleaseEffectEmitter @0x4EA031]. The release CLEARS the handle
// [orig: @0x5F7607], so a round that surfaces again SPAWNS A NEW ONE rather
// than resuming the old.
//
// That is ported as written rather than latched: latching it would suppress
// the plume for the rest of a skipping round's flight, which retail does not.
inline constexpr uint32_t kClipWaterFxFlag = 0x20000000u;

inline bool round_effect_clips_water(uint32_t ammo_flags) {
	return (ammo_flags & kClipWaterFxFlag) != 0u;
}

inline bool round_effect_should_release_for_water(uint32_t ammo_flags,
		int32_t round_z, int32_t water_z) {
	return round_effect_clips_water(ammo_flags) && round_z <= water_z;
}

// ON DEATH the round RELEASES its emitter through the same detach rather than
// destroying the live particles [orig: Projectile_ReleaseEffects @0x4E8280 —
//  @0x4E82B1..0x4E82BF calls Entity_ReleaseEffectEmitter on +0x1CC; the trail
//  channel +0x2B4 gets a final point and CEffectChannel_RequestKill
//  @0x4E8291..0x4E82AC so it drains rather than vanishes]. The detach walks
// the emitter's effect objects through their vtable+60 stop
// [orig: sub_5E5ED0 @0x5E5ED0] — it is not a destroy, which is why an RPG's
// smoke lingers and dissipates BEHIND the impact instead of being cut off at
// it.
enum class RoundEffectEnd { Detach, Destroy };

inline RoundEffectEnd round_effect_end_kind() { return RoundEffectEnd::Detach; }

} // namespace opennova::world
