// The in-flight round effect's lifecycle: lazy spawn, the water release, the
// detach-on-death, and the max-age countdown.
// [orig: the spawn @0x4E9F58 / @0x4EA8AE; the water gate @0x4EA01D..0x4EA036;
//  Projectile_ReleaseEffects @0x4E8280]

#include <world/round_move_effect.h>

#include <cstdio>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c, m)                                                            \
	do {                                                                       \
		if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; }       \
	} while (0)

// Spawn is lazy and needs all three conditions — a round with no effect, one
// already emitting, or one out of life spawns nothing.
void test_lazy_spawn() {
	CHECK(round_effect_should_spawn(true, false, 100), "a fresh round spawns");
	CHECK(!round_effect_should_spawn(false, false, 100),
			"no effect authored, no emitter");
	CHECK(!round_effect_should_spawn(true, true, 100),
			"an existing handle is not replaced");
	CHECK(!round_effect_should_spawn(true, false, 0),
			"a round with no life left spawns nothing");
}

// THE WATER RELEASE IS NOT LATCHED. A round that dips and surfaces spawns a
// NEW emitter rather than resuming the old one — latching would suppress the
// plume for the rest of a skipping round's flight.
void test_water_release_is_not_latched() {
	const uint32_t clips = kClipWaterFxFlag;
	const uint32_t plain = 0u;

	// Only rounds carrying the flag care about water at all.
	CHECK(round_effect_clips_water(clips), "the flag is recognised");
	CHECK(!round_effect_clips_water(plain), "and absent means no water gate");
	CHECK(!round_effect_should_release_for_water(plain, -100, 0),
			"a round without the flag ignores the water plane entirely");

	// At or below the plane releases; above does not.
	CHECK(round_effect_should_release_for_water(clips, -10, 0),
			"below the water releases");
	CHECK(round_effect_should_release_for_water(clips, 0, 0),
			"exactly at the plane releases too");
	CHECK(!round_effect_should_release_for_water(clips, 10, 0),
			"above the plane keeps emitting");

	// Surfacing makes the spawn condition true again, because the release
	// cleared the handle — that is the un-latched behaviour.
	CHECK(round_effect_should_spawn(true, false, 50),
			"after a release the round can spawn a fresh emitter");
}

// ON DEATH THE EMITTER DETACHES rather than being destroyed, so the plume
// lingers and dissipates BEHIND the impact instead of being cut off mid-air.
void test_death_detaches_rather_than_destroys() {
	CHECK(round_effect_end_kind() == RoundEffectEnd::Detach,
			"a dying round DETACHES its emitter");
	CHECK(round_effect_end_kind() != RoundEffectEnd::Release,
			"it does not release/destroy the live particles");
}

// A zero max_age means the round never times out on its own.
void test_life_countdown() {
	CHECK(round_life_seed(120) == 120, "life seeds from max_age");
	CHECK(round_life_expires(120), "a positive max_age expires");
	CHECK(!round_life_expires(0), "a zero max_age never times out");

	// A timing round counts down and floors at zero.
	CHECK(round_life_step(2, true) == 1, "it counts down");
	CHECK(round_life_step(1, true) == 0, "to zero");
	CHECK(round_life_step(0, true) == 0, "and stays there");

	// A non-timing round holds its value forever rather than draining.
	CHECK(round_life_step(50, false) == 50, "a non-expiring round holds");
	CHECK(round_life_step(0, false) == 0, "including at zero");
}

} // namespace

int main() {
	test_lazy_spawn();
	test_water_release_is_not_latched();
	test_death_detaches_rather_than_destroys();
	test_life_countdown();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("round_move_effect_test OK\n");
	return 0;
}
