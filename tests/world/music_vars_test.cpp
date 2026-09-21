// The gamemus var pump from the local player (world/music_vars.h), pinned
// where it used to live in the Godot world node (ADR 0040 ladder E0): Var7 =
// health percent from the entity's health over the body's authored max
// (100 without one), Var10 = team; no local player pumps 0 / 0.
// [orig: Entity_UpdateInfantryPlayerBody @ 0x4b40e0; @ 0x4b6315-0x4b6324;
//  @ 0x4b62fc]
#include <runtime/world/music_vars.h>

#include <runtime/audio/music_policy.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/local_player_view.h>
#include <runtime/world/world.h>

#include <cstdio>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

int main() {
	World w;
	// No local player: the shared health pair reads 0 / 100, both vars pump zero.
	CHECK(local_player_health(w) == 0);
	CHECK(local_player_max_health(w) == 100);
	{
		const auto writes = game_music_var_writes(w);
		CHECK(writes[0].slot == opennova::audio::kGameMusicHealthPctVarSlot);
		CHECK(writes[0].value == 0);
		CHECK(writes[1].slot == opennova::audio::kGameMusicTeamVarSlot);
		CHECK(writes[1].value == 0);
	}
	w.registry.configure_pool(0, 4);
	Entity seed;
	seed.kind = EntityKind::Organic;
	seed.alive = true;
	seed.health = 50;
	seed.team = 2;
	const EntityHandle local = w.registry.spawn(0, seed);
	w.cached.local_player = local;
	// An entity without a body reads the default max of 100.
	{
		const auto writes = game_music_var_writes(w);
		CHECK(writes[0].value == 50);
		CHECK(writes[1].value == 2);
	}
	// The body's authored max drives the percent; a non-positive max falls back.
	w.ai.attach(local);
	w.ai.for_handle(local)->inf.max_health = 200;
	CHECK(local_player_health(w) == 50);
	CHECK(local_player_max_health(w) == 200);
	CHECK(game_music_var_writes(w)[0].value == 25);
	w.ai.for_handle(local)->inf.max_health = 0;
	CHECK(game_music_var_writes(w)[0].value == 50);
	// Health at or above the max pins to 100.
	w.ai.for_handle(local)->inf.max_health = 40;
	CHECK(game_music_var_writes(w)[0].value == 100);

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("music_vars_test OK\n");
	return 0;
}
