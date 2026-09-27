#pragma once

// The gamemus var-pump projections from the local player body update [orig:
// Entity_UpdateInfantryPlayerBody @ 0x4b40e0, gate entity ==
// g_LocalPlayerEntity @ 0x4b6234; the full var map is
// docs/audio/mus-sbf-re.md §Game music driving].

#include <runtime/audio/music_policy.h>

#include <array>

namespace opennova::world {

class World;

// Var7 = health percent: integer cur*100/max, pinned to 100 when max <= cur
// [orig: @ 0x4b6315..0x4b6324].
constexpr int music_health_percent(int cur, int max) {
	return max > cur ? cur * 100 / max : 100;
}

static_assert(music_health_percent(50, 100) == 50);
static_assert(music_health_percent(100, 100) == 100);
static_assert(music_health_percent(120, 100) == 100);
static_assert(music_health_percent(1, 3) == 33);

// One gamemus var write: the VM var slot and its new value.
struct MusicVarWrite {
	int slot = 0;
	int value = 0;
};

// The per-frame pump from the local player (music_vars.cpp): Var7 health
// percent and Var10 team, in that order; without a local player the health
// reads 0 and the team 0. The embedder relays each write to its music VM.
std::array<MusicVarWrite, 2> game_music_var_writes(const World &world);

}  // namespace opennova::world
