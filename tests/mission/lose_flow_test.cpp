#include <runtime/world/local_player_view.h>
// P2 round outcome on the retail data: a training mission's witnessed lose
// flow, the sim half (the shell half — the MISSION FAILED screen and ESC to
// the menu — is godot/tests/game/main_game_lifecycle_test.gd). One executable,
// one ctest per mission (`--bms <name>`): 04TR's WAC carries both team legs,
// 00TRa's only `if true(bluekills) then Lose(1)` (`--victim-team 1`; the first
// mission's authored friendly-fire failure, playthrough gate 8 of
// docs/world/npc-mission-completion.md):
//
//   kill a green/blue PERSON with real player rounds (the local weapon pump
//   -> RoundSim damage/death chain) ->
//   * the kill tally lands in the sim (greenkills/bluekills, the WAC builtin
//     source) [orig: Score_TallyKillByLocalPlayer @0x4fd160],
//   * the mission WAC's `true(greenkills) -> Lose(0)` / `true(bluekills) ->
//     Lose(1)` fires [orig: WacAction_Lose @0x4ed3f0]: the "lose" effect
//     carries the witnessed Misc gametext key and the round ends winner 2
//     [orig: Server_ProcessRoundEnd @0x5164f0].
//
// REPORT MODE: `lose_flow_test --bms 00TRa.bms --events` boots the mission and
// prints its BMS event table — every event's triggers and actions by name, the
// Triggered Text each OutputText resolves to through the mission's own .bin,
// the area-trigger zones, and which events the PreMission pass fired at boot
// [orig: EventTrigger_UpdateAllWithFlag2 @0x454dc0] — the authored-sequence
// reference the mission_playthrough probe's gates are read against.
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
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying the mission).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <formats/rtxt/rtxt.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// then blue 1), or only `required_team` when the mission's WAC carries a
// single leg. The round sim's entity hits scan pool 0 only.
Victim pick_victim(testrig::RetailMissionRig &rig, const std::set<uint16_t> &blacklist, int required_team) {
	Victim best;
	const w::Vec3 player = rig.local.player_position();
	for (int i = 0; i < rig.world.ai.count(); ++i) {
		w::AiEntity *e = rig.world.ai.at(i);
		if (e == nullptr || !e->inf.active || blacklist.count(e->handle.packed)) continue;
		if (required_team >= 0 && e->team != required_team) continue;
		w::Entity *ent = rig.world.registry.get(e->handle);
		// A seated person stays a target: the rounds reach an occupant through
		// the vehicle (00TRa's two friendlies are the trucks' drivers).
		if (ent == nullptr || !ent->alive || ent->handle.pool() != 0) continue;
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

// The local fire path uses the current motor's Position + CameraOffset.
w::Vec3 local_round_origin(const testrig::RetailMissionRig &rig) {
    const w::Entity *entity = rig.world.registry.get(rig.world.cached.local_player);
    return entity != nullptr ? w::player_eye_position(*entity) : rig.local.player_position();
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
	// A seated victim sits inside its mount's hull: the LOS leg is judged
	// against everything but that hull (the round's own hit resolves the
	// occupant through it), so the mount is not a blocker here.
	const w::Entity *victim_entity = rig.world.registry.get(victim);
	const bool seated = victim_entity != nullptr && victim_entity->mounted && victim_entity->mount_target.valid();
	lof.los_clear = seated ? rig.world.collision->raycast_clear(rig.world, o, t, victim_entity->mount_target, victim)
	                       : rig.world.collision->raycast_clear(rig.world, o, t, player, victim);
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
		// A seated victim: the round stops on the mount (the occupant leg
		// resolves the damage through it).
		if (const w::Entity *ve = rig.world.registry.get(victim); ve != nullptr && ve->mounted &&
				ve->mount_target.valid() && ev.entity == ve->mount_target.packed)
			on_victim = true;
	}
	return on_victim;
}

// --- the event-table report ---------------------------------------------------------

const char *trigger_main_name(int v) {
	switch (v) {
	case 1: return "Group";
	case 2: return "Single";
	case 3: return "Event";
	case 4: return "MissionVariable";
	case 5: return "SecondTimeThrough";
	case 6: return "Teammate";
	case 7: return "Player";
	}
	return "?";
}

// The Group and Single subtype ladders share their numbering (bms.h).
const char *group_or_single_sub_name(int v) {
	switch (v) {
	case 0: return "Null";
	case 1: return "SeesGroup";
	case 2: return "HasTargetedGroup";
	case 3: return "AtRedAlert";
	case 4: return "Destroyed";
	case 5: return "Alive";
	case 6: return "HasLostMoreUnits";
	case 7: return "AtWaypoint";
	case 9: return "Intact";
	case 10: return "IsWithinArea";
	case 11: return "HoldingGroup";
	case 12: return "HasMoreUnits";
	case 13: return "HasShotGroup";
	case 14: return "AtYellowAlert";
	case 15: return "HasTargetedSingle";
	case 16: return "SeesSingle";
	case 17: return "HasShotSingle";
	case 42: return "OnTopOf";
	case 43: return "FartherThan";
	case 44: return "HasNoLOS";
	case 45: return "DoesNotSeeOrFarther";
	}
	return "?";
}

const char *player_sub_name(int v) {
	switch (v) {
	case 18: return "Berserk";
	case 19: return "FirstPerson";
	case 20: return "ThirdPerson";
	case 21: return "CockpitView";
	case 34: return "DialogDone";
	case 35: return "DialogFinished";
	case 36: return "Awol";
	case 37: return "Satchel";
	case 38: return "AttachedToSsn";
	case 39: return "OnSsn";
	case 40: return "DrivingSsn";
	case 41: return "OnGun";
	}
	return "?";
}

const char *misvar_sub_name(int v) {
	switch (v) {
	case 1: return "==";
	case 2: return "<";
	case 3: return ">";
	case 4: return "<=";
	case 5: return ">=";
	}
	return "?";
}

const char *trigger_sub_name(int main, int sub) {
	switch (main) {
	case 1:
	case 2: return group_or_single_sub_name(sub);
	case 4: return misvar_sub_name(sub);
	case 6: return sub == 1 ? "IsEnabled" : sub == 2 ? "MedicAssisting" : sub == 3 ? "Evacuating" : "?";
	case 7: return player_sub_name(sub);
	}
	return "";
}

const char *action_name(int v) {
	switch (v) {
	case 0: return "Null";
	case 1: return "RedirectGroupTo";
	case 2: return "KillGroup";
	case 3: return "ChangeGroupAI";
	case 4: return "VaporizeGroup";
	case 5: return "MisvarChange";
	case 6: return "OutputText";
	case 7: return "PlayWavList";
	case 8: return "BlueWin";
	case 9: return "RedWin";
	case 10: return "GreenWin";
	case 11: return "GroupVelocity";
	case 12: return "AreaAiRed";
	case 13: return "AreaAiBlue";
	case 14: return "SubGoalWon";
	case 15: return "SubGoalLost";
	case 16: return "ChangeGTeamAction";
	case 17: return "ChangeGroupAction";
	case 18: return "GroupTeleportAction";
	case 19: return "RedirectSingleTo";
	case 20: return "KillSingle";
	case 21: return "ChangeSingleAI";
	case 22: return "VaporizeSingle";
	case 23: return "SingleVelocity";
	case 24: return "ChangeSteamAction";
	case 25: return "SingleChangeGroup";
	case 26: return "SingleTeleportAction";
	case 27: return "ParticleEffectAction";
	case 30: return "GroupOpenDoorAction";
	case 31: return "GroupCloseDoorAction";
	case 32: return "GroupResetHasVisited";
	case 33: return "SingleResetHasVisited";
	case 34: return "ResetEvent";
	case 35: return "ShowWinSubgoal";
	case 36: return "ShowLoseSubgoal";
	case 37: return "AttachToEmplaced";
	case 38: return "SetLightState";
	case 39: return "Teammates";
	case 40: return "ShowWaypoints";
	case 41: return "ExecuteWac";
	case 42: return "SsnTargetSsnPri";
	case 43: return "SsnTargetSsnExc";
	case 44: return "SsnTargetGroupPri";
	case 45: return "SsnTargetGroupExc";
	case 46: return "GroupTargetSsnPri";
	case 47: return "GroupTargetSsnExc";
	case 48: return "GroupTargetGroupPri";
	case 49: return "GroupTargetGroupExc";
	}
	return "?";
}

// The mission's own string table (<mission>.bin beside the .bms in the mounted
// root), for the Triggered Text an OutputText action shows
// [orig: HUD_DisplayTriggeredText @0x51f190 reads "Triggered Text" ID%03i].
bool load_mission_strings(const testrig::RetailMissionRig &rig, const std::string &bms, rtxt::File &out) {
	std::string bin = bms;
	const size_t dot = bin.rfind('.');
	if (dot != std::string::npos) bin.erase(dot);
	bin += ".bin";
	std::vector<uint8_t> bytes;
	if (!rig.index.read_file(bin, bytes)) return false;
	std::string error;
	return rtxt::parse(bytes.data(), bytes.size(), out, error);
}

void print_event_table(testrig::RetailMissionRig &rig, const std::string &bms) {
	const bms::File &m = rig.mission;
	rtxt::File strings;
	const bool have_strings = load_mission_strings(rig, bms, strings);
	std::printf("--- %s: %zu events, %zu triggers, %zu actions, %zu area triggers (strings %s) ---\n", bms.c_str(),
			m.events.size(), m.triggers.size(), m.actions.size(), m.area_triggers.size(),
			have_strings ? "loaded" : "MISSING");
	for (size_t ei = 0; ei < m.events.size(); ++ei) {
		const bms::Event &ev = m.events[ei];
		const uint32_t flags = static_cast<uint32_t>(ev.flags);
		std::printf("event %-3zu flags=0x%x%s%s%s delay=%d reset=%d fired=%d\n", ei, flags,
				(flags & 1u) ? " repeat" : "", (flags & 2u) ? " pre" : "", (flags & 4u) ? " post" : "",
				ev.delay >> 22, ev.reset_after >> 22, rig.events.event_fired(ei) ? 1 : 0);
		for (int k = 0; k < int(ev.trigger_count); ++k) {
			const size_t tx = size_t(ev.trigger_index) + size_t(k);
			if (tx >= m.triggers.size()) continue;
			const bms::Trigger &t = m.triggers[tx];
			const int main = int(t.main_type);
			std::printf("    if  %s.%s(%d) p=(%d, %d, %d, %d)%s%s\n", trigger_main_name(main),
					trigger_sub_name(main, t.sub_type), t.sub_type, t.param1, t.param2, t.param3, t.param4,
					t.is_negated() ? " NOT" : "", k + 1 < int(ev.trigger_count) ? (t.is_or() ? " or" : t.is_xor() ? " xor" : " and") : "");
		}
		for (int k = 0; k < int(ev.action_count); ++k) {
			const size_t ax = size_t(ev.action_index) + size_t(k);
			if (ax >= m.actions.size()) continue;
			const bms::Action &ac = m.actions[ax];
			const int type = int(ac.action_type);
			std::printf("    do  %s(%d) sub=%d p=(%d, %d, %d, %d)", action_name(type), type, ac.action_sub_type,
					ac.param1, ac.param2, ac.param3, ac.param4);
			if (type == 6 && have_strings) {
				char key[16];
				std::snprintf(key, sizeof(key), "ID%03d", ac.param1);
				std::printf("  \"%s\"", strings.get_in_section("Triggered Text", key).c_str());
			}
			std::printf("\n");
		}
	}
	for (size_t bi = 0; bi < m.area_triggers.size(); ++bi) {
		const bms::AreaTrigger &bb = m.area_triggers[bi];
		std::printf("area %-3zu id=%-4d x[%8.1f..%8.1f] y[%8.1f..%8.1f] z[%8.1f..%8.1f]%s\n", bi, bb.id,
				bb.get_x_min(), bb.get_x_max(), bb.get_y_min(), bb.get_y_max(), bb.get_z_min(), bb.get_z_max(),
				bb.is_active() ? " MISSION_AREA" : "");
	}
}

} // namespace

int main(int argc, char **argv) {
	std::string bms = "04TR.bms";
	int victim_team = -1;
	bool events_only = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--bms") == 0 && i + 1 < argc) bms = argv[++i];
		else if (std::strcmp(argv[i], "--victim-team") == 0 && i + 1 < argc) victim_team = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--events") == 0) events_only = true;
	}
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying the training missions)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, bms, error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "the mission boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (events_only) {
		print_event_table(rig, bms);
		return 0;
	}
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;
	if (!expect(rig.wac_loaded, "the mission's WAC compiled and installed")) return 1;
	if (!expect(rig.install_weapon("WPN_M4AUTO"), "WPN_M4AUTO installs")) return 1;
	if (!expect(!rig.world.match.outcome().ended, "the round has not ended at spawn")) return 1;
	if (!expect(rig.world.collision != nullptr, "the collision world is up")) return 1;

	// NPC fire stays LIVE: the chain under test is observed through it, never
	// with the NPCs disarmed. The scenario invites retaliation (a friendly is
	// shot at 4 u in the open), so the player's entity carries the engine's own
	// no-damage flag: every NPC round still flies, connects and stamps its
	// reactions; only the damage-side zero on the player changes [orig: the
	// Flags & 0x4000000 test in Projectile_ProcessDamageOnTarget @0x4e7fb0,
	// the test @0x4e7ff6]. A victim
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
		const Victim npc = pick_victim(rig, blacklist, victim_team);
		if (npc.ai == nullptr) {
			// Every candidate is retired for now: a mission with few lose-team
			// people (00TRa has two, one of them the seated instructor) gets its
			// retired victims back after a pause — they walk, and a line of fire
			// a truck hull blocked opens again.
			run.tick(kTicksPerSecond * 10);
			if (!blacklist.empty()) {
				std::printf("lose-flow: t=%ds every candidate retired — revisiting %zu of them\n", run.seconds(),
						blacklist.size());
				blacklist.clear();
			}
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
		// Lock, then tap-fire verified shots; the victim keeps its authored
		// health and dies to the rounds' own damage.
		const int32_t tally_at_lock = player_tally(target_team);
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
	// The SP epilog: the lose cine raises the end-of-round screen within two
	// frames and the script never runs again, so the WAC's `Lose` (its chat line
	// and banner) lands exactly once however long the screen stays up
	// [orig: the WAC tick gate @0x51d8bd on g_epilog_screen_active, raised by
	//  the lose screen build @0x57450c].
	run.tick(kTicksPerSecond * 4);
	int lose_count = 0, round_end_count = 0;
	bool saw_lose = false, saw_round_end = false;
	for (const w::Effect &e : run.seen) {
		if (e.kind == "lose") ++lose_count;
		if (e.kind == "round_end") ++round_end_count;
		if (e.kind == "lose" && e.str == expected_key) saw_lose = true;
		if (e.kind == "round_end" && e.a == 2) saw_round_end = true;
	}
	expect(saw_lose, "the lose effect carries the witnessed Misc gametext key");
	expect(saw_round_end, "the round_end host effect names winner 2");
	std::printf("lose-flow: %d lose / %d round_end effect(s) over the four seconds after the end\n", lose_count,
			round_end_count);
	expect(lose_count == 1, "the WAC Lose ran exactly once: the SP end screen halts the script");
	expect(round_end_count == 1, "the round ended exactly once");
	if (failures == 0)
		std::printf("lose_flow %s: team-%d person kill -> %s -> Lose -> round end (winner 2) in %d mission seconds\n",
				bms.c_str(), target_team, green ? "greenkills" : "bluekills", run.seconds());
	return failures == 0 ? 0 : 1;
}
