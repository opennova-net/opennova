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
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying 04TR.bms).
#include "common/retail_mission_rig.h"
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

constexpr int kMaxMissionSeconds = 420;
constexpr float kFireDistance = 26.0f;
constexpr uint32_t kIndestructibleFlag = 0x4000000u; // [orig: @0x4e7ff6]

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
	const w::Vec3 player = rig.player_position();
	for (int i = 0; i < rig.ai.count(); ++i) {
		w::AiEntity *e = rig.ai.at(i);
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
	if (!expect(rig.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.wac_loaded, "04TR's WAC compiled and installed")) return 1;
	if (!expect(rig.install_weapon("WPN_M4AUTO"), "WPN_M4AUTO installs")) return 1;
	if (!expect(!rig.world.match.outcome().ended, "the round has not ended at spawn")) return 1;

	std::vector<w::Effect> seen;
	const auto keep_effects = [&]() {
		for (const w::Effect &e : rig.drain_effects())
			if (e.kind == "lose" || e.kind == "win" || e.kind == "round_end") {
				std::printf("lose-flow: effect %s a=%d str=%s\n", e.kind.c_str(), e.a, e.str.c_str());
				seen.push_back(e);
			}
	};
	int seconds = 0;
	const auto tick = [&]() {
		rig.tick();
		keep_effects();
	};
	const auto mission_second = [&]() {
		for (int t = 0; t < 62; ++t) tick();
		++seconds;
	};

	std::set<uint16_t> blacklist;
	int target_team = -1;
	bool killed = false;
	while (seconds < kMaxMissionSeconds && !killed) {
		// Approach the current best candidate; out of reach (mission geography
		// defeats straight-line walking) the victim is brought to the player —
		// the outcome loop is under test, not nav.
		w::EntityHandle target;
		rig.input.forward = true;
		while (seconds < kMaxMissionSeconds) {
			const Victim npc = pick_victim(rig, blacklist);
			if (npc.ai == nullptr) {
				mission_second();
				continue;
			}
			const w::Vec3 me = rig.player_position();
			w::Vec3 to = testrig::ai_position(*npc.ai);
			to.z = me.z;
			rig.aim_at(me, to);
			if (npc.distance <= kFireDistance) {
				target = npc.ai->handle;
				target_team = npc.ai->team;
				std::printf("lose-flow: target lock net=%d team=%d flags=0x%x at %.1fu\n", int(npc.entity->net_id),
						target_team, unsigned(npc.entity->engine_flags), npc.distance);
				break;
			}
			mission_second();
			if (seconds % 5 == 0) {
				// Out of reach (mission geography defeats straight-line walking):
				// bring the player to the victim's open ground, 5 u away and
				// facing it. The outcome loop is under test, not nav.
				const w::Vec3 victim = testrig::ai_position(*npc.ai);
				const w::Vec3 pp = rig.player_position();
				const double away = std::atan2(pp.y - victim.y, pp.x - victim.x);
				const w::Vec3 spot{victim.x + 5.0f * float(std::cos(away)), victim.y + 5.0f * float(std::sin(away)), victim.z};
				const double facing_engine_deg = std::atan2(victim.y - spot.y, victim.x - spot.x) * 180.0 / 3.14159265358979323846;
				rig.teleport_local_player(spot, 90.0 - facing_engine_deg, 0.0);
				std::printf("lose-flow: teleport the player 5 u from net=%d team=%d\n", int(npc.entity->net_id),
						int(npc.ai->team));
			}
			if (seconds % 10 == 0)
				std::printf("lose-flow: approach t=%ds dist=%.1fu team=%d\n", seconds, npc.distance, int(npc.ai->team));
		}
		rig.input.forward = false;
		if (!target.valid()) break;

		// Fire at the locked target (pre-weakened through the SETHP store so the
		// first connecting round completes the kill).
		rig.set_entity_health(target, 10);
		int hp_prev = 10;
		int fire_seconds = 0;
		bool pressed = true;
		while (seconds < kMaxMissionSeconds) {
			for (int t = 0; t < 62; ++t) {
				if (const w::AiEntity *tai = rig.ai_for(target)) {
					const w::Vec3 me = rig.player_position();
					const w::Vec3 muzzle{me.x, me.y, me.z + 0.9f};
					w::Vec3 chest = testrig::ai_position(*tai);
					chest.z += 0.9f;
					rig.aim_at(muzzle, chest);
					rig.input.forward = testrig::planar_distance(me, chest) > 8.0f;
				}
				rig.set_weapon_input(true, pressed, false);
				pressed = false;
				tick();
			}
			++seconds;
			++fire_seconds;
			const w::Entity *tent = rig.world.registry.get(target);
			const w::AiEntity *tai = rig.ai_for(target);
			const int hp = tai != nullptr ? tai->health : -1;
			{
				const w::RoundSim &rs = rig.world.round_sim;
				const int idx = (rs.debug_trail_next - 1 + w::RoundSim::kDebugTrailCap) % w::RoundSim::kDebugTrailCap;
				const w::RoundDebugEvent &last = rs.debug_trail[idx];
				std::printf("lose-flow: fire t=%ds target hp=%d dist=%.1f clip=%d fired_serial=%u active_rounds=%d trail=%d last(kind=%d entity=%u tick=%u)\n",
						seconds, hp, tai != nullptr ? testrig::distance(testrig::ai_position(*tai), rig.player_position()) : -1.0f,
						rig.weapon.slot.clip, unsigned(rig.weapon.fired_serial), rs.active_count, rs.debug_trail_count,
						int(last.kind), unsigned(last.entity), unsigned(last.tick));
			}
			if (hp < hp_prev && hp >= 0) std::printf("lose-flow: HIT t=%ds target hp %d -> %d\n", seconds, hp_prev, hp);
			hp_prev = hp;
			if (tent == nullptr || !tent->alive || hp <= 0) {
				killed = true;
				rig.input.forward = false;
				rig.set_weapon_input(false, false, false);
				std::printf("lose-flow: KILLED t=%ds team-%d person down\n", seconds, target_team);
				break;
			}
			if (rig.player_health() <= 0) {
				std::fprintf(stderr, "FAIL: the player died first (t=%ds) — rerun\n", seconds);
				return 1;
			}
			if (fire_seconds >= 15) {
				std::printf("lose-flow: stall on net=%d hp=%d — retargeting\n", tent != nullptr ? int(tent->net_id) : -1, hp);
				blacklist.insert(target.packed);
				rig.set_weapon_input(false, false, false);
				break;
			}
		}
	}
	if (!expect(killed, "a candidate victim was killed before the mission budget")) return 1;

	// --- Outcome: the tally, the WAC lose, the round end. The WAC VM runs every
	// 62nd tick, so the lose lands within a mission-second or two.
	const bool green = target_team == 0;
	const char *expected_key = green ? "STRMISC_KILLEDGREEN" : "STRMISC_KILLEDBLUE";
	bool ended = false;
	for (int i = 0; i < 10 && !ended; ++i) {
		mission_second();
		const w::MatchOutcome &oc = rig.world.match.outcome();
		std::printf("lose-flow: outcome t=%ds ended=%d winner=%d greenkills=%d bluekills=%d\n", seconds, int(oc.ended),
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
	for (const w::Effect &e : seen) {
		if (e.kind == "lose" && e.str == expected_key) saw_lose = true;
		if (e.kind == "round_end" && e.a == 2) saw_round_end = true;
	}
	expect(saw_lose, "the lose effect carries the witnessed Misc gametext key");
	expect(saw_round_end, "the round_end host effect names winner 2");
	if (failures == 0)
		std::printf("lose_flow_04tr: team-%d person kill -> %s -> Lose -> round end (winner 2)\n", target_team,
				green ? "greenkills" : "bluekills");
	return failures == 0 ? 0 : 1;
}
