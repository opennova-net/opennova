#pragma once

#include <cstdint>

namespace opennova::world {

// THE IN-FLIGHT ROUND EFFECT's LIFECYCLE — when the `move`-row emitter is
// spawned, re-posed, released and detached.
//
// The effect itself (effects_table tag 1) is already published to the shell.
// What lives here is the part that decides how long it lives, which is where
// the behaviour is: a plume that lingers after impact reads completely
// differently from one that vanishes with the round.

// The emitter is spawned LAZILY — once per live round, only when there is an
// effect to spawn, no handle yet, and the round still has life left
// [orig: the guarded spawn @0x4E9F58 (guided) / @0x4EA8AE (ballistic)].
inline bool round_effect_should_spawn(bool has_effect, bool has_handle,
		int32_t life_ticks) {
	return has_effect && !has_handle && life_ticks != 0;
}

// A round carrying ClipWaterFx whose z reaches the water plane RELEASES its
// emitter instead of re-posing it [orig: @0x4EA01D..0x4EA036 against
// Env_WaterHeightFixed]. The release CLEARS the handle [orig: @0x5F7607], so a
// round that surfaces again SPAWNS A NEW ONE rather than resuming the old.
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

// ON DEATH the round DETACHES its emitter rather than destroying it
// [orig: Projectile_ReleaseEffects @0x4E8280 — @0x4E82B1..0x4E82BF]. Detach
// stops new emission and lets the particles already alive finish out, which is
// why an RPG's smoke lingers and dissipates BEHIND the impact instead of
// disappearing at it. Destroying the group would cut the plume off mid-air.
enum class RoundEffectEnd { Detach, Release };

inline RoundEffectEnd round_effect_end_kind() { return RoundEffectEnd::Detach; }

// The round's life in ticks is seeded from the ammo def's max_age
// [orig: the seed @0x4EC679; the parse @0x40A86C..0x40A893] and counted down
// each tick. A zero max_age means the round never times out on its own.
inline int32_t round_life_seed(int32_t max_age_ticks) {
	return max_age_ticks;
}

inline bool round_life_expires(int32_t max_age_ticks) {
	return max_age_ticks != 0;
}

// One tick of the countdown. A round with no timeout holds its life value.
inline int32_t round_life_step(int32_t life_ticks, bool expires) {
	if (!expires) return life_ticks;
	return life_ticks > 0 ? life_ticks - 1 : 0;
}

} // namespace opennova::world
