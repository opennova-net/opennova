#include <runtime/world/music_vars.h>

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

namespace opennova::world {

// Re-drive the gamemus vars from the local player each frame, the way the
// original does from the local player's body update [orig:
// Entity_UpdateInfantryPlayerBody @ 0x4b40e0, gate entity ==
// g_local_player_entity @ 0x4b6234; full map docs/audio/mus-sbf-re.md §Game
// music driving]. Pumped here: Var7 = health % (cur*100/max, 100 when max <=
// cur [orig: @ 0x4b6315-0x4b6324]) and Var10 = team [orig: @ 0x4b62fc]. The
// max comes from the body's authored profile (100 when there is no body or
// no authored value), the current health and the team from the entity.
// Witnessed-but-unpumped seams (the shipped gamemus reads none of them --
// docs/audio/mus-sbf-re.md (D-MUS-VARPUMP)): Var2 view pitch (the original
// writes raw engine angle units, unwitnessed conversion), Var5/Var6 threat
// distance / threat-targets-me (Entity_FindNearestThreat @ 0x4b0990
// unported), Var3/Var4 (low-confidence), Var8 game type (retail scoring-mode
// ids not yet mapped to our sessions).
std::array<MusicVarWrite, 2> game_music_var_writes(const World &world) {
	int health = 0;
	int max_health = 100;
	int team = 0;
	if (world.cached.local_player.valid()) {
		if (const Entity *e = world.registry.get(world.cached.local_player)) {
			health = e->health;
			team = static_cast<int>(e->team);
		}
		const AiEntity *body = world.ai.for_handle(world.cached.local_player);
		if (body != nullptr && body->inf.max_health > 0) max_health = body->inf.max_health;
	}
	return {MusicVarWrite{audio::kGameMusicHealthPctVarSlot, music_health_percent(health, max_health)},
			MusicVarWrite{audio::kGameMusicTeamVarSlot, team}};
}

} // namespace opennova::world
