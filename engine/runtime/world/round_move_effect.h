#pragma once

#include <cstdint>

#include <runtime/world/ammo_table.h>

namespace opennova::world {

// THE IN-FLIGHT ROUND EFFECT's LIFECYCLE — when the `move`-row emitter
// (round +0x1CC) is spawned and when the water plane releases it. The round
// simulation consults these predicates every flight tick and publishes the
// result as `LiveRound::move_effect_live`; the shell's fire presentation owns
// the emitter itself (spawn, re-pose, and the detach-on-death — the latter is
// the live `throwable_present_pass.gd` leg: Projectile_ReleaseEffects
// @0x4E8280 -> Entity_ReleaseEffectEmitter @0x5F75D0 stops emission and lets
// the live particles drain).
//
// The round's OWN life is not here: +0x2AC is seeded from the ammo's max_age
// [orig: @0x4EC679], decremented each aging tick [orig: @0x4E9F37..0x4E9F4E,
//  skipped under ammo flag 0x4000 `noage` @0x4E9F2F] and retired at <= 0 by
// the Projectile_UpdatePhysics head [orig: @0x4E9DA7..0x4E9DAE] — so a
// max_age of 0 retires the round on its first tick and "never times out" is
// the noage flag. That leg is the live world/round_sim.cpp port
// (`max_age_ticks`, `kAmmoFlagNoAge`, the expiry test).

// The emitter is spawned LAZILY [orig: the `useownmove` leg @0x4E9F58..
//  0x4E9F94; the ballistic leg @0x4EA8AE..0x4EA8D3]: an authored effect
// (ammo +0x70) @0x4E9F58, no handle yet (+0x1CC == 0) @0x4E9F63, life still
// nonzero (+0x2AC != 0) @0x4E9F70 — on the useownmove leg the decrement runs
// before this test, so a round retiring next tick spawns nothing; the
// ballistic leg reads it BEFORE its own decrement @0x4EAA7F — and, on the
// useownmove leg ONLY, NOT clipped by water, the same ClipWaterFx + z test as
// the release @0x4E9F7D..0x4E9F8E. The ballistic test has no water term. All
// four must hold, so a round can never quietly acquire a second emitter.
inline bool round_effect_should_spawn(bool has_effect, bool has_handle,
		int32_t life_ticks, bool clipped_by_water) {
	return has_effect && !has_handle && life_ticks != 0 && !clipped_by_water;
}

inline bool round_effect_clips_water(uint32_t ammo_flags) {
	return (ammo_flags & kAmmoFlagClipWaterFx) != 0u;
}

// A `useownmove` round carrying ClipWaterFx whose z reaches the water plane
// RELEASES its emitter instead of re-posing it [orig: @0x4EA01D..0x4EA036 —
//  `z > Env_WaterHeightFixed` re-poses via CEffect_UpdateEmitterTransform
//  @0x4EA039, else ammo flags (+0x114) & 0x20000000 detaches via
//  Entity_ReleaseEffectEmitter @0x4EA031; the ballistic leg's handle branch
//  @0x4EA963..0x4EA96D only re-poses]. The release CLEARS the handle
// [orig: @0x5F7607], so a round that surfaces again SPAWNS A NEW ONE rather
// than resuming the old.
//
// That is ported as written rather than latched: latching it would suppress
// the plume for the rest of a skipping round's flight, which retail does not.
inline bool round_effect_should_release_for_water(uint32_t ammo_flags,
		int32_t round_z, int32_t water_z) {
	return round_effect_clips_water(ammo_flags) && round_z <= water_z;
}

} // namespace opennova::world
