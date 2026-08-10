#pragma once

// The gamemus var-pump projections from the local player body update [orig:
// Entity_UpdateInfantryPlayerBody @ 0x4b40e0, gate entity ==
// g_local_player_entity @ 0x4b6234; the full var map is
// docs/audio/mus-sbf-re.md §Game music driving].

namespace opennova::world {

// Var7 = health percent: integer cur*100/max, pinned to 100 when max <= cur
// [orig: @ 0x4b6315..0x4b6324].
constexpr int music_health_percent(int cur, int max) {
	return max > cur ? cur * 100 / max : 100;
}

static_assert(music_health_percent(50, 100) == 50);
static_assert(music_health_percent(100, 100) == 100);
static_assert(music_health_percent(120, 100) == 100);
static_assert(music_health_percent(1, 3) == 33);

}  // namespace opennova::world
