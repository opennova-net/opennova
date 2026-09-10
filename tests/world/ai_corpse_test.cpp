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
// This is NOT a firefight-balance test: the incidental exchange is pinned by
// scripted stores (guard hold on the target, per-tick player top-up), so the
// player cannot lose the race and no assertion rides on it.
// Gated on OPENNOVA_JO_ASSETS (an extracted retail tree carrying CP01.bms);
// no synthetic leg exists: the death matrix needs a retail infantry .adm.
#include "common/retail_mission_files.h"
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
	const w::Vec3 player = rig.local.player_position();
	for (int i = 0; i < rig.world.ai.count(); ++i) {
		w::AiEntity *e = rig.world.ai.at(i);
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
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
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
	rig.local.input.forward = true;
	while (seconds < kMaxMissionSeconds) {
		const Nearest npc = nearest_npc(rig);
		if (npc.ai != nullptr) {
			const w::Vec3 me = rig.local.player_position();
			w::Vec3 to = testrig::ai_position(*npc.ai);
			to.z = me.z;
			rig.local.aim_at(me, to);
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
					npc.ai != nullptr ? npc.distance : -1.0f, rig.local.player_health());
	}
	rig.local.input.forward = false;
	if (!target.valid()) {
		std::fprintf(stderr, "FAIL: no foot NPC came within %.0fu in %ds\n", kFireDistance, seconds);
		return 1;
	}

	// --- Fire: pre-weaken, then steer the look at the target's live chest every
	// tick while the trigger is held (the FSM sustains the volley and reloads).
	// Rounds leave the muzzle (the +0.9 u chest stand-in) parallel to the look,
	// so aim the muzzle at the chest.
	//
	// The ambient firefight is incidental to this test (the subject is the
	// corpse chain), and since the 2026-09-01 guard-bit fix landed the authored
	// group guard-clear on engine_flags, the formerly frozen guards patrol
	// faithfully — the target evades and the pack can win the incidental race.
	// Two scripted (WAC-shaped) stores pin the scenario without touching the
	// kill chain: ssnguard holds the target on its post (the guard bit routes
	// it into the stationary mounted-fire tail), and the player is topped up
	// through the same SETHP store each tick.
	{
		const w::Entity *tent0 = rig.world.registry.get(target);
		if (tent0 != nullptr && tent0->net_id != 0)
			rig.world.commands.set_ssn_guard(tent0->net_id, true);
	}
	rig.world.commands.set_entity_health(target, 10);
	int hp_prev = 10;
	int fire_seconds = 0;
	bool killed = false;
	// One press edge per second: a held trigger sustains a volley only until
	// the magazine runs dry, and the auto-reload does NOT resume it without a
	// fresh press (the witnessed deferred-refire rounds gate). The approach
	// speed is the kit's weight band (a 59.7 u kit runs at run_2), so the first
	// magazine can be spent on the move before the 8 u standing fire.
	bool pressed = true;
	while (seconds < kMaxMissionSeconds && fire_seconds < kFireSeconds) {
		pressed = true;
		for (int t = 0; t < 62; ++t) {
			const w::AiEntity *tai = rig.world.ai.for_handle(target);
			if (tai != nullptr) {
				const w::Vec3 me = rig.local.player_position();
				const w::Vec3 muzzle{me.x, me.y, me.z + 0.9f};
				w::Vec3 chest = testrig::ai_position(*tai);
				chest.z += 0.9f;
				rig.local.aim_at(muzzle, chest);
				rig.local.input.forward = testrig::planar_distance(me, chest) > 8.0f;
			}
			rig.local.set_weapon_input(true, pressed, false);
			pressed = false;
			if (rig.world.cached.local_player.valid())
				rig.world.commands.set_entity_health(rig.world.cached.local_player, 150);
			rig.tick();
		}
		++seconds;
		++fire_seconds;
		const w::Entity *tent = rig.world.registry.get(target);
		const w::AiEntity *tai = rig.world.ai.for_handle(target);
		const int hp = tai != nullptr ? tai->health : -1;
		if (hp < hp_prev && hp >= 0) std::printf("corpse: HIT t=%ds target hp %d -> %d\n", seconds, hp_prev, hp);
		hp_prev = hp;
		if (tent == nullptr || !tent->alive || hp <= 0) {
			killed = true;
			rig.local.input.forward = false;
			rig.local.set_weapon_input(false, false, false);
			// Release the scripted guard hold: the corpse watch is about an
			// ordinary body, and the mounted-bit tail must not keep owning it.
			if (tent != nullptr && tent->net_id != 0)
				rig.world.commands.set_ssn_guard(tent->net_id, false);
			std::printf("corpse: KILLED t=%ds — player rounds killed the target\n", seconds);
			break;
		}
	}
	if (!expect(killed, "the fire phase killed the target within its budget")) return 1;

	// Walk up to the body before the watch: the guard died on its authored
	// post, which can keep hard cover between the watcher and the 0.9 u watch
	// ray. The §19.4 rule is about a SEEN corpse — stand where it is seen.
	{
		int walk_seconds = 0;
		while (walk_seconds < 20) {
			const w::Entity *c = rig.world.registry.get(target);
			if (c == nullptr) break;
			const w::Vec3 me = rig.local.player_position();
			if (testrig::planar_distance(me, c->position) <= 3.0f) break;
			w::Vec3 to = c->position;
			to.z = me.z;
			rig.local.aim_at(me, to);
			rig.local.input.forward = true;
			rig.tick(62);
			++seconds;
			++walk_seconds;
		}
		rig.local.input.forward = false;
	}

	// --- Corpse: dead but NOT hidden; a death-family anim; the timer seeded and
	// draining; still visible after the timer would have expired (the watch rule).
	const w::Entity *c0 = rig.world.registry.get(target);
	const w::AiEntity *a0 = rig.world.ai.for_handle(target);
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
		const w::AiEntity *a = rig.world.ai.for_handle(target);
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
