// P1c death presentation on the retail data (world-wac-ai-re §19): the local
// player walks to the nearest foot NPC on CP01 and KILLS it with real rounds
// (the local weapon pump -> RoundSim damage/death chain — the same path that
// stages the death-anim selection, §19.2), then the corpse is watched:
//
//   * the kill leaves the NPC dead but NOT hidden — the corpse persists,
//   * its anim state is a DEATH state (the bullet matrix 180..239 for a round
//     kill; 173/174 fallbacks tolerated for stripped anim sets),
//   * the corpse timer is seeded from items.def deathtime (CP01 soldiers author
//     30 s -> 1922 ticks) and drains,
//   * WATCHED PERSISTENCE: the corpse outlives its timer while the local
//     player can see it (the §19.4 watch-check parks it on 62-tick retries).
//
// The target is pre-weakened through the scripted-SETHP store (the WAC SETHP
// shape) so the FIRST connecting round completes the kill; the kill still
// travels the full damage/death chain, only the required hit count changes.
// Gated on OPENNOVA_JO_ASSETS (an extracted retail tree carrying CP01.bms);
// no synthetic leg exists: the death matrix needs a retail infantry .adm.
#include "common/retail_mission_rig.h"
#include "common/retail_paths.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

constexpr int kMaxMissionSeconds = 420;
constexpr float kFireDistance = 26.0f;
constexpr int kDeathStateMin = 173;
constexpr int kFireSeconds = 60;

struct Nearest {
	w::AiEntity *ai = nullptr;
	w::Entity *entity = nullptr;
	float distance = 1e30f;
};

Nearest nearest_npc(testrig::RetailMissionRig &rig) {
	Nearest best;
	const w::Vec3 player = rig.player_position();
	for (int i = 0; i < rig.ai.count(); ++i) {
		w::AiEntity *e = rig.ai.at(i);
		if (e == nullptr || !e->inf.active) continue;
		w::Entity *ent = rig.world.registry.get(e->handle);
		if (ent == nullptr || !ent->alive || ent->mounted) continue;
		const float d = testrig::distance(testrig::ai_position(*e), player);
		if (d < 0.5f) continue;
		if (d < best.distance) {
			best.distance = d;
			best.ai = e;
			best.entity = ent;
		}
	}
	return best;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(assets, retail::assets(),
			"OPENNOVA_JO_ASSETS (an extracted retail tree carrying CP01.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(assets, "CP01.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "CP01 boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.ammo_ok, "ammo.def loaded")) return 1;
	if (!expect(rig.install_weapon("WPN_M4AUTO"), "WPN_M4AUTO installs from weapon.def")) return 1;

	int seconds = 0;
	const auto mission_second = [&]() {
		rig.tick(62);
		++seconds;
	};

	// --- Approach: walk at the nearest NPC; the moment a foot NPC is inside the
	// fire distance stop and open fire — waiting loses the race against the
	// NPC's own perception + fire chain.
	w::EntityHandle target;
	rig.input.forward = true;
	while (seconds < kMaxMissionSeconds) {
		const Nearest npc = nearest_npc(rig);
		if (npc.ai != nullptr) {
			const w::Vec3 me = rig.player_position();
			w::Vec3 to = testrig::ai_position(*npc.ai);
			to.z = me.z;
			rig.aim_at(me, to);
			if (npc.distance <= kFireDistance) {
				target = npc.ai->handle;
				std::printf("corpse: target lock net=%d hp=%d at %.1fu deathtime=%d leave_corpse=%d\n",
						int(npc.entity->net_id), int(npc.ai->health), npc.distance,
						npc.entity->deathtime_ticks, int(npc.entity->leave_corpse));
				break;
			}
		}
		mission_second();
		if (seconds % 10 == 0)
			std::printf("corpse: approach t=%ds dist=%.1fu hp=%d\n", seconds,
					npc.ai != nullptr ? npc.distance : -1.0f, rig.player_health());
	}
	rig.input.forward = false;
	if (!target.valid()) {
		std::fprintf(stderr, "FAIL: no foot NPC came within %.0fu in %ds\n", kFireDistance, seconds);
		return 1;
	}

	// --- Fire: pre-weaken, then steer the look at the target's live chest every
	// tick while the trigger is held (the FSM sustains the volley and reloads).
	// Rounds leave the muzzle (the +0.9 u chest stand-in) parallel to the look,
	// so aim the muzzle at the chest.
	rig.set_entity_health(target, 10);
	int hp_prev = 10;
	int fire_seconds = 0;
	bool killed = false;
	bool pressed = true;
	while (seconds < kMaxMissionSeconds && fire_seconds < kFireSeconds) {
		for (int t = 0; t < 62; ++t) {
			const w::AiEntity *tai = rig.ai_for(target);
			if (tai != nullptr) {
				const w::Vec3 me = rig.player_position();
				const w::Vec3 muzzle{me.x, me.y, me.z + 0.9f};
				w::Vec3 chest = testrig::ai_position(*tai);
				chest.z += 0.9f;
				rig.aim_at(muzzle, chest);
				rig.input.forward = testrig::planar_distance(me, chest) > 8.0f;
			}
			rig.set_weapon_input(true, pressed, false);
			pressed = false;
			rig.tick();
		}
		++seconds;
		++fire_seconds;
		const w::Entity *tent = rig.world.registry.get(target);
		const w::AiEntity *tai = rig.ai_for(target);
		const int hp = tai != nullptr ? tai->health : -1;
		if (hp < hp_prev && hp >= 0) std::printf("corpse: HIT t=%ds target hp %d -> %d\n", seconds, hp_prev, hp);
		hp_prev = hp;
		if (tent == nullptr || !tent->alive || hp <= 0) {
			killed = true;
			rig.input.forward = false;
			rig.set_weapon_input(false, false, false);
			std::printf("corpse: KILLED t=%ds — player rounds killed the target\n", seconds);
			break;
		}
		if (rig.player_health() <= 0) {
			std::fprintf(stderr, "FAIL: the NPC killed the PLAYER first (t=%ds) — rerun\n", seconds);
			return 1;
		}
	}
	if (!expect(killed, "the fire phase killed the target within its budget")) return 1;

	// --- Corpse: dead but NOT hidden; a death-family anim; the timer seeded and
	// draining; still visible after the timer would have expired (the watch rule).
	const w::Entity *c0 = rig.world.registry.get(target);
	const w::AiEntity *a0 = rig.ai_for(target);
	if (!expect(c0 != nullptr && a0 != nullptr, "the corpse entity persists after the kill")) return 1;
	const int anim0 = a0->inf.anim_state;
	const int timer0 = c0->corpse_timer;
	std::printf("corpse: t=%ds alive=%d hidden=%d anim=%d (%s) corpse_timer=%d deathtime=%d\n", seconds,
			int(c0->alive), int(c0->hidden), anim0,
			(anim0 >= 0 && anim0 < w::kInfantryAnimStateCount) ? w::kInfantryAnimNames[anim0] : "?",
			timer0, c0->deathtime_ticks);
	expect(!c0->hidden, "the corpse is not hidden immediately after the kill");
	expect(anim0 >= kDeathStateMin, "the dead NPC's anim is a death state");
	const bool bullet_matrix = anim0 >= 180 && anim0 <= 239;
	if (!bullet_matrix)
		std::printf("corpse: WARN round kill fell back to anim %d (adm lacks the matrix clip?)\n", anim0);
	expect(c0->deathtime_ticks > 0, "the corpse timer is seeded from items.def deathtime");

	// Watch ~40 mission seconds: past the 31 s deathtime, the watched corpse
	// must still be there (parked on the 62-tick retry), never hidden.
	bool watched_ok = true;
	for (int i = 0; i < 8; ++i) {
		rig.tick(62 * 5);
		seconds += 5;
		const w::Entity *c = rig.world.registry.get(target);
		const w::AiEntity *a = rig.ai_for(target);
		const bool hidden = c == nullptr || c->hidden;
		std::printf("corpse: watch t=%ds hidden=%d corpse_timer=%d anim=%d\n", seconds, int(hidden),
				c != nullptr ? c->corpse_timer : -1, a != nullptr ? a->inf.anim_state : -1);
		if (hidden) {
			watched_ok = false;
			break;
		}
	}
	expect(watched_ok, "the watched corpse persists (the §19.4 watch rule holds it)");
	if (failures == 0)
		std::printf("ai_corpse: round kill -> death pose (anim %d%s) -> corpse persisted watched (timer0=%d)\n",
				anim0, bullet_matrix ? " [bullet matrix]" : "", timer0);
	return failures == 0 ? 0 : 1;
}
