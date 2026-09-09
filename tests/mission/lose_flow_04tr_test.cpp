// P2 round outcome on the retail data: the 04TR training mission's witnessed
// lose flow, the sim half (the shell half — the MISSION FAILED screen and ESC
// to the menu — is godot/tests/game/main_game_lifecycle_test.gd):
//
//   kill a green/blue PERSON with real player rounds (the local weapon pump
//   -> RoundSim damage/death chain) ->
//   * the kill tally lands in the sim (greenkills/bluekills, the WAC builtin
//     source) [orig: Score_TallyKillByLocalPlayer @0x4fd160],
//   * 04TR.WAC's `true(greenkills) -> Lose(0)` / `true(bluekills) -> Lose(1)`
//     fires [orig: WacAction_Lose @0x4ed3f0]: the "lose" effect carries the
//     witnessed Misc gametext key and the round ends winner 2
//     [orig: Server_ProcessRoundEnd @0x5164f0].
//
// The kill is STAGED, never strayed into. The player is teleported onto open
// ground a few units from the locked victim, at a bearing where the engine's
// own line-of-fire queries agree the shot connects — the LOS raycast from the
// local pump's round origin [orig: Physics_RaycastTerrainAndSectors
// @0x539910], the victim's posed section walk the round's pool-0 leg runs
// [orig: Physics_RaycastAgainstBoneSections @0x4e4670], and no bystander's
// sections across the ray — then tap-fires at the victim's widest posed hit
// sphere and reads the round's stop event back from the RoundSim trail. A kill
// counts only when the crediting round stopped on the locked victim.
//
// Why staged: the earlier shape (walk or teleport near the victim, hold the
// trigger at head height) never hit its locked target on any engine — the
// player spawns on a raised structure whose deck stopped every round, and the
// aim point sat at the top of the head sphere from an origin 0.1 u below the
// real one, so the rounds skimmed over the head. Its passes came from STRAY
// rounds into passing squads, and vanished once the org1 motors gained
// retail's airborne Flags mirror [orig: Entity_UpdateInfantryAI @0x4b9910 —
// the terrain slide block's Flags & 0x90A000 gate @0x4ba89b skips an
// airborne body], which shifted one squad's timing by a few ticks.
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying 04TR.bms).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

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

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxMissionSeconds = 420;
constexpr int kTicksPerSecond = 62;
constexpr uint32_t kIndestructibleFlag = 0x4000000u; // [orig: @0x4e7ff6]
// The staged shot: open ground this far from the victim, the bearings tried
// around it, the ticks the motor gets to re-ground the teleported body, and
// the teleport lift (a person's Position is its pelvis, ~1 u over its feet).
constexpr float kStageDistance = 4.0f;
constexpr float kRestageDistance = 8.0f; // the victim walked out of the staged envelope
constexpr int kStageBearings = 8;
constexpr int kStageSettleTicks = 16;
constexpr float kTeleportLift = 1.0f;
// Per victim: verified shots and mission seconds before it is retargeted.
constexpr int kShotsPerVictim = 8;
constexpr int kSecondsPerVictim = 15;
constexpr int kShotResolveTicks = 12; // a rifle round crosses 4 u inside one tick
constexpr int kTapRetryTicks = 8;     // the FSM was mid-cycle / reloading

struct Victim {
	w::AiEntity *ai = nullptr;
	w::Entity *entity = nullptr;
	float distance = 1e30f;
	int rank = 99;
};

// Nearest live pool-0 person NPC, preferring a LOSE-triggering team (green 0,
// then blue 1). The round sim's entity hits scan pool 0 only.
Victim pick_victim(testrig::RetailMissionRig &rig, const std::set<uint16_t> &blacklist) {
	Victim best;
	const w::Vec3 player = rig.local.player_position();
	for (int i = 0; i < rig.world.ai.count(); ++i) {
		w::AiEntity *e = rig.world.ai.at(i);
		if (e == nullptr || !e->inf.active || blacklist.count(e->handle.packed)) continue;
		w::Entity *ent = rig.world.registry.get(e->handle);
		if (ent == nullptr || !ent->alive || ent->mounted || ent->handle.pool() != 0) continue;
		if ((ent->engine_flags & kIndestructibleFlag) != 0) continue; // scripted, rounds never damage it
		const float d = testrig::distance(testrig::ai_position(*e), player);
		if (d < 0.5f) continue;
		const int team = e->team;
		const int rank = team == 0 ? 0 : (team == 1 ? 1 : 2);
		if (rank < best.rank || (rank == best.rank && d < best.distance)) {
			best.rank = rank;
			best.distance = d;
			best.ai = e;
			best.entity = ent;
		}
	}
	return best;
}

// The rig plus the tick counter the mission budget reads and the outcome
// effects the round end is judged on.
struct Run {
	testrig::RetailMissionRig &rig;
	std::vector<w::Effect> seen;
	int ticks = 0;
	void tick() {
		rig.tick();
		++ticks;
		for (const w::Effect &e : rig.drain_effects())
			if (e.kind == "lose" || e.kind == "win" || e.kind == "round_end") {
				std::printf("lose-flow: effect %s a=%d str=%s\n", e.kind.c_str(), e.a, e.str.c_str());
				seen.push_back(e);
			}
	}
	void tick(int count) {
		for (int i = 0; i < count; ++i) tick();
	}
	int seconds() const { return ticks / kTicksPerSecond; }
};

// Where the local pump spawns the player's rounds: the embedder-fed eye
// sample when one is valid, else Position + 1.0 u — this bare kernel never
// feeds one (player_weapon.cpp, the WeaponAction_Fire @0x542c5e spawn leg).
w::Vec3 local_round_origin(const testrig::RetailMissionRig &rig) {
	const w::LocalPlayerWeapon &wpn = rig.local.weapon;
	if (wpn.eye_valid) return w::Vec3{wpn.eye_mission[0], wpn.eye_mission[1], wpn.eye_mission[2]};
	const w::Vec3 p = rig.local.player_position();
	return w::Vec3{p.x, p.y, p.z + 1.0f};
}

// The victim's widest posed hit sphere — the same posed sections the round's
// person leg walks, at their retail effective radius — as the aim point.
bool victim_torso(testrig::RetailMissionRig &rig, const w::AiEntity &ai, w::Vec3 &out) {
	const w::Vec3 vp = testrig::ai_position(ai);
	const int32_t anchor[3] = {w::to_fixed(vp.x), w::to_fixed(vp.y), w::to_fixed(vp.z)};
	int32_t best_radius = 0;
	for (const w::CollisionWorld::DebugPersonSection &sec :
			rig.world.collision->debug_person_sections(rig.world, anchor, w::to_fixed(2.0f), 16)) {
		if (sec.handle != ai.handle || sec.masked || sec.radius <= best_radius) continue;
		best_radius = sec.radius;
		out = w::Vec3{sec.center[0] / 65536.0f, sec.center[1] / 65536.0f, sec.center[2] / 65536.0f};
	}
	return best_radius > 0;
}

// The engine's own verdicts on a shot from `origin` at `torso`: the LOS
// raycast (terrain + the pool 2/1 sectors, shooter and victim excluded), the
// victim's section walk with the ray carried 1 u past the torso so the sphere
// centers project inside the segment, and no other person's sections across
// it (the round's person leg stops on the first qualifying body, corpses
// included).
struct LineOfFire {
	bool los_clear = false;
	bool reaches_victim = false;
	bool bystander = false;
	bool ok() const { return los_clear && reaches_victim && !bystander; }
};

void fixed3(const w::Vec3 &v, int32_t out[3]) {
	out[0] = w::to_fixed(v.x);
	out[1] = w::to_fixed(v.y);
	out[2] = w::to_fixed(v.z);
}

LineOfFire line_of_fire(testrig::RetailMissionRig &rig, const w::Vec3 &origin, const w::Vec3 &torso,
		w::EntityHandle victim) {
	LineOfFire lof;
	const float len = testrig::distance(origin, torso);
	if (len <= 0.01f) return lof;
	const w::Vec3 beyond{torso.x + (torso.x - origin.x) / len, torso.y + (torso.y - origin.y) / len,
			torso.z + (torso.z - origin.z) / len};
	int32_t o[3], t[3], b[3];
	fixed3(origin, o);
	fixed3(torso, t);
	fixed3(beyond, b);
	const w::EntityHandle player = rig.world.cached.local_player;
	lof.los_clear = rig.world.collision->raycast_clear(rig.world, o, t, player, victim);
	w::PersonSectionHit hit;
	lof.reaches_victim = rig.world.collision->raycast_person_sections(rig.world, victim, o, b, 0, hit);
	for (int i = 0; i < rig.world.ai.count(); ++i) {
		const w::AiEntity *e = rig.world.ai.at(i);
		if (e == nullptr || !e->inf.active || e->handle == victim || e->handle == player) continue;
		if (e->handle.pool() != 0) continue;
		if (rig.world.collision->raycast_person_sections(rig.world, e->handle, o, t, 0, hit)) {
			lof.bystander = true;
			break;
		}
	}
	return lof;
}

// Put the player on open ground kStageDistance from the victim, facing it, at
// the first bearing where the line of fire holds once the motor has
// re-grounded the body. `torso` receives the verified aim point.
bool stage_shot(Run &run, w::EntityHandle victim, w::Vec3 &torso) {
	testrig::RetailMissionRig &rig = run.rig;
	const w::AiEntity *ai = rig.world.ai.for_handle(victim);
	if (ai == nullptr) return false;
	const w::Vec3 vp0 = testrig::ai_position(*ai);
	const w::Vec3 pp0 = rig.local.player_position();
	const double base = std::atan2(pp0.y - vp0.y, pp0.x - vp0.x);
	for (int k = 0; k < kStageBearings; ++k) {
		ai = rig.world.ai.for_handle(victim);
		if (ai == nullptr || !ai->inf.active) return false;
		// Re-read the victim per attempt: it keeps walking.
		const w::Vec3 vp = testrig::ai_position(*ai);
		const double away = base + k * (2.0 * kPi / kStageBearings);
		w::Vec3 spot{vp.x + kStageDistance * float(std::cos(away)), vp.y + kStageDistance * float(std::sin(away)), 0.0f};
		spot.z = rig.ground_height(spot.x, spot.y) + kTeleportLift;
		const double facing_deg = std::atan2(vp.y - spot.y, vp.x - spot.x) * 180.0 / kPi;
		rig.local.teleport_local_player(spot, 90.0 - facing_deg, 0.0);
		run.tick(kStageSettleTicks);
		ai = rig.world.ai.for_handle(victim);
		if (ai == nullptr || !ai->inf.active) return false;
		if (!victim_torso(rig, *ai, torso)) return false;
		const w::Vec3 origin = local_round_origin(rig);
		const LineOfFire lof = line_of_fire(rig, origin, torso, victim);
		const w::Vec3 pp = rig.local.player_position();
		std::printf("lose-flow: stage bearing %d: player=(%.1f,%.1f,%.1f) torso=(%.1f,%.1f,%.1f) at %.1fu los=%d reaches=%d bystander=%d\n",
				k, pp.x, pp.y, pp.z, torso.x, torso.y, torso.z, testrig::distance(origin, torso), int(lof.los_clear),
				int(lof.reaches_victim), int(lof.bystander));
		if (lof.ok()) return true;
	}
	return false;
}

// One tap of the trigger aimed at `torso` from the pump's own origin. False
// when the FSM took no shot (mid-cycle, reloading); true once a round left.
bool tap_fire(Run &run, const w::Vec3 &torso) {
	testrig::RetailMissionRig &rig = run.rig;
	rig.local.aim_at(local_round_origin(rig), torso);
	const uint32_t serial_before = rig.local.weapon.fired_serial;
	rig.local.set_weapon_input(true, true, false);
	run.tick();
	rig.local.set_weapon_input(false, false, false);
	return rig.local.weapon.fired_serial != serial_before;
}

// The player's rounds that stopped at or after `fire_tick`, read back from
// the RoundSim trail: `newest` receives the latest such stop (its tick stays
// below fire_tick while none landed); true when one of them stopped on
// `victim`'s posed sections. Every player stop in the window counts, so an
// earlier miss still in flight can neither mask nor forge this shot's verdict.
bool scan_stops(const testrig::RetailMissionRig &rig, uint32_t fire_tick, w::EntityHandle victim,
		w::RoundDebugEvent &newest) {
	const w::RoundSim &rs = rig.world.round_sim;
	bool on_victim = false;
	for (int k = 1; k <= rs.debug_trail_count; ++k) {
		const int idx = (rs.debug_trail_next - k + w::RoundSim::kDebugTrailCap) % w::RoundSim::kDebugTrailCap;
		const w::RoundDebugEvent &ev = rs.debug_trail[idx];
		if (ev.shooter != rig.world.cached.local_player.packed || ev.tick < fire_tick) continue;
		if (newest.tick < fire_tick || ev.tick > newest.tick) newest = ev;
		if (ev.kind == w::RoundDebugEvent::kOrganic && ev.entity == victim.packed) on_victim = true;
	}
	return on_victim;
}

} // namespace

int main() {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying 04TR.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "04TR.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "04TR boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.wac_loaded, "04TR's WAC compiled and installed")) return 1;
	if (!expect(rig.install_weapon("WPN_M4AUTO"), "WPN_M4AUTO installs")) return 1;
	if (!expect(!rig.world.match.outcome().ended, "the round has not ended at spawn")) return 1;
	if (!expect(rig.world.collision != nullptr, "the collision world is up")) return 1;

	// NPC fire stays LIVE: the chain under test is observed through it, never
	// with the NPCs disarmed. The scenario invites retaliation (a friendly is
	// shot at 4 u in the open), so the player's entity carries the engine's own
	// no-damage flag: every NPC round still flies, connects and stamps its
	// reactions; only the damage-side zero on the player changes [orig: the
	// Flags & 0x4000000 test in Entity_ApplyWeaponDamage @0x4e7ff6]. A victim
	// that falls to that fire is retargeted, not counted (see the kill leg).
	if (w::Entity *me = rig.world.registry.get(rig.world.cached.local_player))
		me->engine_flags |= kIndestructibleFlag;

	Run run{rig};
	const w::EntityHandle player = rig.world.cached.local_player;
	std::set<uint16_t> blacklist;
	int target_team = -1;
	bool killed = false;
	// The by-player bucket the WAC predicate reads for the locked victim's team
	// [orig: Score_TallyKillByLocalPlayer @0x4fd160 — victim+354 0 = green,
	//  1 = blue, else the enemy count].
	const auto player_tally = [&](int team) -> int32_t {
		const w::MissionKillStats &ks = rig.world.kill_stats;
		return team == 0 ? ks.greenkills_by_player
				: team == 1 ? ks.bluekills_by_player : ks.enemy_kills_by_player;
	};
	while (run.seconds() < kMaxMissionSeconds && !killed) {
		const Victim npc = pick_victim(rig, blacklist);
		if (npc.ai == nullptr) {
			run.tick(kTicksPerSecond);
			continue;
		}
		const w::EntityHandle target = npc.ai->handle;
		target_team = npc.ai->team;
		const int victim_net_id = int(npc.entity->net_id);
		w::Vec3 torso;
		if (!stage_shot(run, target, torso)) {
			std::printf("lose-flow: net=%d team=%d could not be staged (no open line of fire) — retargeting\n",
					victim_net_id, target_team);
			blacklist.insert(target.packed);
			continue;
		}
		// Lock: pre-weaken through the SETHP store so the first connecting
		// round completes the kill, then tap-fire verified shots.
		const int32_t tally_at_lock = player_tally(target_team);
		rig.world.commands.set_entity_health(target, 10);
		if (const w::Entity *locked = rig.world.registry.get(target))
			std::printf("lose-flow: target lock net=%d team=%d flags=0x%x\n", victim_net_id, target_team,
					unsigned(locked->engine_flags));

		int shots = 0;
		bool round_on_victim = false; // one of the player's rounds stopped on the locked victim
		const int lock_ticks = run.ticks;
		while (run.seconds() < kMaxMissionSeconds && shots < kShotsPerVictim &&
				run.ticks - lock_ticks < kSecondsPerVictim * kTicksPerSecond) {
			const w::AiEntity *tai = rig.world.ai.for_handle(target);
			const w::Entity *tent = rig.world.registry.get(target);
			if (tai == nullptr || tent == nullptr || !tent->alive || tent->health <= 0) break;
			// Re-verify before every tap (the victim walks); re-stage when the
			// line of fire no longer holds or the victim left the envelope.
			if (!victim_torso(rig, *tai, torso)) break;
			w::Vec3 origin = local_round_origin(rig);
			if (testrig::distance(origin, torso) > kRestageDistance || !line_of_fire(rig, origin, torso, target).ok()) {
				if (!stage_shot(run, target, torso)) break;
				origin = local_round_origin(rig);
			}
			if (!tap_fire(run, torso)) {
				run.tick(kTapRetryTicks);
				continue;
			}
			++shots;
			const uint32_t fire_tick = rig.world.logic_tick;
			w::RoundDebugEvent stop;
			stop.tick = fire_tick > 0 ? fire_tick - 1 : 0; // "none landed yet"
			bool on_victim = false;
			for (int t = 0; t < kShotResolveTicks && !on_victim; ++t) {
				run.tick();
				on_victim = scan_stops(rig, fire_tick, target, stop);
			}
			round_on_victim = round_on_victim || on_victim;
			tent = rig.world.registry.get(target);
			const bool stopped = stop.tick >= fire_tick;
			const int hp = tent != nullptr ? tent->health : -1;
			std::printf("lose-flow: shot %d t=%ds stop=%s kind=%d entity=0x%x hit=(%.1f,%.1f,%.1f) on_victim=%d target hp=%d alive=%d\n",
					shots, run.seconds(), stopped ? "yes" : "NO", stopped ? int(stop.kind) : -1,
					stopped ? unsigned(stop.entity) : 0u, stop.hit.x, stop.hit.y, stop.hit.z, int(on_victim), hp,
					tent != nullptr ? int(tent->alive) : 0);
			if (tent == nullptr || !tent->alive || hp <= 0) {
				// Only a kill the host credits to the local player feeds the WAC
				// (the by-player bucket, drained from the death record on the
				// listen frame); a victim that fell to live NPC fire lands in the
				// by-others family instead and is retargeted, never counted
				// [orig: Score_TallyKillByLocalPlayer @0x4fd160 vs
				//  Score_TallyKillByOthers @0x4fd300].
				for (int t = 0; t < kTicksPerSecond && player_tally(target_team) == tally_at_lock; ++t) run.tick();
				const bool credited = player_tally(target_team) > tally_at_lock;
				if (credited && round_on_victim) {
					killed = true;
					std::printf("lose-flow: KILLED t=%ds team-%d person net=%d down to the staged round\n", run.seconds(),
							target_team, victim_net_id);
				} else if (credited) {
					// A credit without the aimed round on the victim is the stray
					// dependency this test exists to refuse.
					expect(false, "the kill credit came from a round that did not stop on the locked victim");
					return 1;
				} else {
					std::printf("lose-flow: net=%d fell to another shooter (tally unchanged) — retargeting\n", victim_net_id);
					blacklist.insert(target.packed);
				}
				break;
			}
			if (on_victim) std::printf("lose-flow: HIT net=%d survived at hp=%d — firing again\n", victim_net_id, hp);
		}
		if (!killed && !blacklist.count(target.packed)) {
			std::printf("lose-flow: stall on net=%d after %d shots — retargeting\n", victim_net_id, shots);
			blacklist.insert(target.packed);
		}
		rig.local.set_weapon_input(false, false, false);
	}
	if (!expect(killed, "a candidate victim was killed by the staged round before the mission budget")) return 1;

	// --- Outcome: the tally, the WAC lose, the round end. The WAC VM runs every
	// 62nd tick, so the lose lands within a mission-second or two.
	const bool green = target_team == 0;
	const char *expected_key = green ? "STRMISC_KILLEDGREEN" : "STRMISC_KILLEDBLUE";
	bool ended = false;
	for (int i = 0; i < 10 && !ended; ++i) {
		run.tick(kTicksPerSecond);
		const w::MatchOutcome &oc = rig.world.match.outcome();
		std::printf("lose-flow: outcome t=%ds ended=%d winner=%d greenkills=%d bluekills=%d\n", run.seconds(), int(oc.ended),
				oc.winner_team, rig.world.kill_stats.greenkills_by_player, rig.world.kill_stats.bluekills_by_player);
		if (oc.ended) {
			ended = true;
			expect(oc.winner_team == 2, "the round ended with winner 2 (lose)");
			expect((green ? rig.world.kill_stats.greenkills_by_player : rig.world.kill_stats.bluekills_by_player) >= 1,
					"the kill tally counted the person");
		}
	}
	if (!expect(ended, "the round ended after killing a lose-team person")) return 1;
	bool saw_lose = false, saw_round_end = false;
	for (const w::Effect &e : run.seen) {
		if (e.kind == "lose" && e.str == expected_key) saw_lose = true;
		if (e.kind == "round_end" && e.a == 2) saw_round_end = true;
	}
	expect(saw_lose, "the lose effect carries the witnessed Misc gametext key");
	expect(saw_round_end, "the round_end host effect names winner 2");
	if (failures == 0)
		std::printf("lose_flow_04tr: team-%d person kill -> %s -> Lose -> round end (winner 2) in %d mission seconds\n",
				target_team, green ? "greenkills" : "bluekills", run.seconds());
	return failures == 0 ? 0 : 1;
}
