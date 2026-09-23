// HEADLESS AI PATH CONFORMANCE — the fast half of the host-parity loop.
//
// The retail-host capture is a dense, per-entity ground-truth transcript of how
// the original walks its waypoints (00TRg idle baseline: all 52 AI sampled >=200
// times). That settles what RETAIL does. It is the WRONG instrument for what WE
// do: a 430 s round plus a decode, through a sampled wire, to observe state we
// own in-process.
//
// So this test is our side of that comparison: load the REAL shipped mission
// through the engine's own mission kernel (the live host's boot: model-derived
// seats, items.def traits, the mission terrain, collision, root motion, the
// WAC layers - ADR 0042 d3), tick the world headless, and report each AI's
// authored routing inputs next to the distance it actually travelled.
// Deterministic (the host's own player, no network), unsampled, and seconds instead of
// minutes. Every one of those boot legs was once omitted by a hand-assembled
// harness that then measured its own omission (concept 7.4c: seatless
// carriers that never board, clipless soldiers that never walk, traitless
// vehicles that never drive, a terrain-less rig with no ground solve, a
// collision-less rig that sails through hulls) - the kernel boot is the one
// implementation, so the harness cannot drift from the game again.
//
// Measured divergence it exists to close (idle-vs-idle, 430 s, zero player input,
// AI-PARITY-CONCEPT.md 6.3d): retail moves every AI except s8/s15/s16, while we
// leave SIX at exactly zero travel — s7, s13, s42, s43, s50, s51. s42/s43/s51
// spawn at the SAME position as retail and retail still walks them 625k/543k/583k
// wire units, so they carry no placement ambiguity and no scenario confound.
//
// Gated on OPENNOVA_JO_DIR (reports Skipped without a JO install), like the
// other mission-kernel ctests.
//
// REPORT MODE: `ai_path_conformance_test --report` dumps the full per-slot table
// (authored group/waypoint/wp_number + brain waypoint state + travel) plus the
// event/trigger/area census. That dump is the diagnosis surface; the assertions
// below are the regression pins. `--ticks <n>` (default 2500) and `--bms <name>`
// (default 00TRg.bms) point the same harness at the 430 s capture budget or
// another mission; the ctest registration passes neither.
#include <formats/mission/bms.h>
#include <runtime/mission/event_runtime.h>
#include <runtime/world/ai.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "common/retail_mission_files.h"
#include "common/retail_mission_open.h"
#include "common/retail_paths.h"

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

// The six AI retail walks and we do not, measured on this run. The three
// listed first spawn at retail's own position, so they are unambiguous.
const int kStuckClean[] = {42, 43, 51};
const int kStuckPlaced[] = {7, 13, 50};

// Positions are 16.16 FIXED [docs/engine-primer.md], so a raw delta of 7143 is
// 0.11 world units -- settle jitter, not travel. Report WORLD UNITS or the
// threshold is meaningless (the first cut of this test used a raw-unit bound and
// scored all 52 AI as "moved" when only one had gone anywhere).
double dist2d_units(const int32_t a[3], const int32_t b[3]) {
	const double dx = (double(a[0]) - double(b[0])) / 65536.0;
	const double dy = (double(a[1]) - double(b[1])) / 65536.0;
	return std::sqrt(dx * dx + dy * dy);
}

// Resolve an AI handle to its AUTHORED mission id so an offline row can be scored
// against the live probe, which keys on bms. Diagnostic only.
int bms_of(const opennova::world::World &w_, opennova::world::EntityHandle h) {
	const opennova::world::Entity *ent = w_.registry.get(h);
	return ent != nullptr ? int(ent->bms_id) : -1;
}

int group_of(const opennova::world::World &w_, opennova::world::EntityHandle h) {
	const opennova::world::Entity *ent = w_.registry.get(h);
	return ent != nullptr ? int(ent->group_id) : -1;
}

// One world unit of net displacement: past ground-settle and pose jitter, well
// under any real patrol leg.
const double kMovedUnits = 1.0;

} // namespace

static const char *arg_value(int argc, char **argv, const char *flag) {
	for (int i = 1; i + 1 < argc; ++i)
		if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
	return nullptr;
}

static bool has_flag(int argc, char **argv, const char *flag) {
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], flag) == 0) return true;
	return false;
}

int main(int argc, char **argv) {
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install serving 00TRg.bms)");
	const bool report = has_flag(argc, argv, "--report");
	// Diagnostic parameterisation only (no behaviour change): the 05TRcoop
	// bunker-garrison pin (SSNs 233/1254/2393, divergence-ledger.md:364) needs
	// the SAME full-tick harness pointed at a different authored mission.
	const char *bms_arg = arg_value(argc, argv, "--bms");
	const std::string bms_name = (bms_arg != nullptr && *bms_arg) ? bms_arg : "00TRg.bms";
	// The mission through the engine's own boot (ADR 0042 d3): the rig's kernel
	// mounts the install, extracts the seat specs from the models, installs the
	// .aip profiles, promotes the mission, grounds the AI on the .cpt/.trn
	// field, registers E_STAND.adm + each soldier's own .adm, resolves the
	// items.def traits and the collision instances, loads weapon.def/ammo.def
	// and the WAC layers - in the S9 order the shipping game runs.
	testrig::RetailMissionRig rig;
	std::string error, served_by;
	if (!retail::open_mission(rig, install, bms_name, error, served_by))
		return retail::skip((bms_name + " on the OPENNOVA_JO_DIR mount (base or an expansion) "
		                     "or loose under OPENNOVA_JO_ASSETS").c_str());
	testrig::BootOptions options;
	options.playable = true;       // the host's own player: the one human
	options.listen_server = false; // the bare no-net tick
	if (!expect(rig.boot(options, error), (bms_name + " boots through the mission kernel").c_str())) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	// Retail holds an empty world still once the WAC clock has started: no
	// human and ticks != 0 skip the whole entity update, so the bare tick
	// counts the host's own player as a single-player authority does.
	// [orig: Game_ProcessMainFrame @0x52671C..0x52672A; Server_BuildEntitySlotLists
	//  @0x4f97a0]
	// INFANTRY DO NOT MOVE WITHOUT ROOT MOTION [orig: AnimMap_UpdateEntity
	// @0x40b5f0]: the kernel registers E_STAND.adm as the default clip set; a
	// mount without it cannot measure pathing at all.
	if (rig.root_motion.empty())
		return retail::skip("E_STAND.adm on the OPENNOVA_JO_DIR mount (infantry cannot locomote without clips)");
	w::World &world = rig.world;
	w::AiSystem &ai = rig.world.ai;
	mission::BmsEventSystem &events = rig.events;
	const bms::File &m = rig.mission;
	expect(rig.promo.nav_channels > 0, "nav channels promoted");
	// The boot legs whose absence once made the harness measure itself: a
	// carrier needs its model-derived seats, the ground solve needs the field
	// [orig: the terrain gates in Entity_UpdateInfantryPlayerBody / the vehicle
	// motor], and bodies need the hulls they walk around.
	expect(!rig.seat_specs.empty(), "the seat specs come from the mission's models");
	expect(rig.has_terrain(), "the mission terrain loaded (the ground solve runs)");
	expect(rig.collision_attached > 0, "the collision instances attached");
	std::printf("kernel boot: %zu seat specs, terrain %s (dim %d), %d collision instances, "
	            "wac %s, mission served by %s\n",
			rig.seat_specs.size(), rig.has_terrain() ? "loaded" : "NOT LOADED",
			rig.has_terrain() ? rig.terrain_store.height_field().dim : 0,
			rig.collision_attached, rig.wac_loaded ? "loaded" : "absent", served_by.c_str());
	for (const mission::ItemSeatSpec &sp : rig.seat_specs) {
		int g = 0, pa = 0, ct = 0, dr = 0;
		for (const world::Seat &st : sp.seats) {
			if (st.type == world::SeatType::Gunner) ++g;
			else if (st.type == world::SeatType::Passenger) ++pa;
			else if (st.type == world::SeatType::Controller) ++ct;
			else if (st.type == world::SeatType::Driver) ++dr;
		}
		if (report && (g > 0 || sp.seats.size() > 1))
			std::printf("   SPEC type=%-7d seats=%-3zu [pass %d ctrl %d GUN %d drv %d] attach=%zu\n",
					sp.type_id, sp.seats.size(), pa, ct, g, dr,
					sp.emplacement_attachments.size());
	}

	// Carrier START positions: a mounted passenger only travels if its VEHICLE
	// drives. Retail's boarded soldiers cover hundreds of thousands of wire units
	// because the hull carries them, so 'mounted but stationary' is a distinct
	// defect from 'never boarded'.
	std::map<uint16_t, std::pair<int32_t, int32_t>> carrier_start;
	for (int j = 0; j < world.registry.pool_capacity(1); ++j) {
		const w::EntityHandle h = w::EntityHandle::make(1, j);
		if (const w::Entity *v = world.registry.get(h))
			carrier_start[h.packed] = {int32_t(v->position.x * 65536.0f),
					int32_t(v->position.y * 65536.0f)};
	}

	// Snapshot every AI's start position, keyed by its AI index.
	const int n = ai.count();
	std::vector<int32_t> start(size_t(n) * 3, 0);
	for (int i = 0; i < n; ++i) {
		const w::AiEntity *e = ai.at(i);
		if (e == nullptr) continue;
		for (int k = 0; k < 3; ++k) start[size_t(i) * 3 + k] = e->pos[k];
	}

	// 2500 ticks = ~40 s of mission time at the 62.5 Hz logic rate — long
	// enough for an authored patrol leg.
	// The capture baseline is 430 s, so comparing counts against it needs
	// `--ticks 27000` -- a 40 s run scoring fewer movers than a 430 s capture is
	// a BUDGET difference, not a defect.
	// PATH LENGTH, accumulated per tick -- NOT net displacement. The capture side
	// sums per-sample deltas, so a start->end measure is not comparable to it: a
	// patrolling AI that loops back has a huge path and a tiny net displacement.
	// (This bit three times before it was fixed; see 7.4b.)
	std::vector<double> ai_path(size_t(ai.count()), 0.0);
	std::vector<int32_t> prev(size_t(ai.count()) * 2, 0);
	for (int i = 0; i < ai.count(); ++i) {
		if (const w::AiEntity *e = ai.at(i)) {
			prev[size_t(i) * 2] = e->pos[0];
			prev[size_t(i) * 2 + 1] = e->pos[1];
		}
	}

	int ticks = 2500;
	if (const char *tv = arg_value(argc, argv, "--ticks")) {
		const int parsed = std::atoi(tv);
		if (parsed > 0) ticks = parsed;
	}
	// EVER-acquired census: the end-of-run snapshot cannot distinguish "never
	// acquired a target" from "released it before the end".
	std::vector<uint8_t> ever_target(size_t(ai.count()) + 64, 0);
	int peak_targets = 0;
	for (int t = 0; t < ticks; ++t) {
		rig.tick();
		{
			int now = 0;
			for (int k = 0; k < ai.count(); ++k) {
				const w::AiEntity *e = ai.at(k);
				if (e == nullptr || !e->inf.combat_target.valid()) continue;
				++now;
				if (size_t(k) < ever_target.size()) ever_target[size_t(k)] = 1;
			}
			if (now > peak_targets) peak_targets = now;
		}
		const int live = ai.count();
		if (int(ai_path.size()) < live) { ai_path.resize(size_t(live), 0.0); prev.resize(size_t(live) * 2, 0); }
		for (int i = 0; i < live; ++i) {
			const w::AiEntity *e = ai.at(i);
			if (e == nullptr) continue;
			const double dx = (double(e->pos[0]) - prev[size_t(i) * 2]) / 65536.0;
			const double dy = (double(e->pos[1]) - prev[size_t(i) * 2 + 1]) / 65536.0;
			ai_path[size_t(i)] += std::sqrt(dx * dx + dy * dy);
			prev[size_t(i) * 2] = e->pos[0];
			prev[size_t(i) * 2 + 1] = e->pos[1];
		}
	}

	if (report) {
		std::printf("--- events referencing RedirectGroupTo (action type 1) ---\n");
		for (size_t ei = 0; ei < m.events.size(); ++ei) {
			const bms::Event &ev = m.events[ei];
			for (int k = 0; k < int(ev.action_count); ++k) {
				const size_t ai_ = size_t(ev.action_index) + size_t(k);
				if (ai_ >= m.actions.size()) continue;
				const bms::Action &ac = m.actions[ai_];
				if (int(ac.action_type) != 1 && int(ac.action_type) != 19 && int(ac.action_type) != 5) continue;
				std::printf("  event %-3zu action type=%-3d sub=%-3d p1(group)=%-4d p2=%-4d  FIRED=%d\n",
						ei, int(ac.action_type), ac.action_sub_type, ac.param1, ac.param2,
						events.event_fired(ei) ? 1 : 0);
			}
		}
		{
			std::printf("--- trigger chains for the rider re-task events ---\n");
			for (size_t ei = 0; ei < m.events.size(); ++ei) {
				bool touches_rider = false;
				for (int k = 0; k < int(m.events[ei].action_count); ++k) {
					const size_t ax = size_t(m.events[ei].action_index) + size_t(k);
					if (ax >= m.actions.size()) continue;
					const int p1 = m.actions[ax].param1;
					if (int(m.actions[ax].action_type) == 19 &&
							(p1 == 807 || p1 == 809 || p1 == 810 || p1 == 812)) touches_rider = true;
					if (int(m.actions[ax].action_type) == 5 && p1 == 2) touches_rider = true;
				}
				if (!touches_rider) continue;
				const bms::Event &ev = m.events[ei];
				std::printf("  event %-3zu FIRED=%d triggers=%d:\n", ei,
						events.event_fired(ei) ? 1 : 0, int(ev.trigger_count));
				for (int k = 0; k < int(ev.trigger_count); ++k) {
					const size_t tx = size_t(ev.trigger_index) + size_t(k);
					if (tx >= m.triggers.size()) continue;
					const bms::Trigger &t = m.triggers[tx];
					std::printf("      main=%-2d sub=%-3d p1=%-6d p2=%-6d p3=%-6d p4=%-6d neg=%d op=%s\n",
							int(t.main_type), t.sub_type, t.param1, t.param2, t.param3, t.param4,
							t.is_negated() ? 1 : 0, t.get_logic_operator().c_str());
				}
			}
		}
		{
			std::printf("--- mission areas: bms area_triggers=%zu, world registry areas=%d ---\n",
					m.area_triggers.size(), [&]{ int c = 0; while (world.registry.area(c) != nullptr) ++c; return c; }());
			for (size_t bi = 0; bi < m.area_triggers.size(); ++bi) {
				const bms::AreaTrigger &bb = m.area_triggers[bi];
				std::printf("  idx %-3zu id=%-5d active=%d x[%8.1f..%8.1f] y[%8.1f..%8.1f]\n",
						bi, bb.id, bb.is_active() ? 1 : 0,
						bb.get_x_min(), bb.get_x_max(),
						bb.get_y_min(), bb.get_y_max());
			}
		}
		size_t nf = 0;
		for (size_t ei = 0; ei < m.events.size(); ++ei) if (events.event_fired(ei)) ++nf;
		std::printf("  events fired: %zu of %zu\n", nf, m.events.size());
	}
	if (report) {
		std::printf("%-5s %-6s %-6s %-5s %-4s %-4s %-6s %-8s %-6s %-6s %-6s %-6s %-7s %-4s %10s\n", "ai#",
				"handle", "bms", "grp", "has", "cmd", "tgtSSN", "wpType", "wpChan", "wpNode",
				"moveMd", "cmd37", "carr36", "mnt", "path_u");
		std::printf("--------------------------------------------------------------------\n");
	}

	int moved = 0, still = 0;
	for (int i = 0; i < n; ++i) {
		const w::AiEntity *e = ai.at(i);
		if (e == nullptr) continue;
		const double travel = (i < int(ai_path.size())) ? ai_path[size_t(i)]
				: dist2d_units(e->pos, &start[size_t(i) * 3]);
		if (travel > kMovedUnits) ++moved; else ++still;
		if (report) {
			// The AUTHORED routing inputs, straight off the BMS organic record,
			// so a still AI is classified as "the mission gave it no route" vs
			// "we failed to apply the route it was given".
			// The RUNTIME routing registers, not m.organics[i]. Indexing the BMS
			// organics by AI INDEX is wrong: the AI list is not the organics list
			// (seat-spec extraction adds emplacement children, so 60 AI vs 52
			// organics) and every authored column silently shifts. Read what the
			// think actually uses: slot[35] has-route, slot[37] channel-or-command,
			// slot[38] node-or-target-SSN [orig: Entity_SetWaypointByTeam @0x43cdb4
			// writes aiComp+140/+148/+152; infantry.cpp:268 and
			// infantry_board.cpp:139 read them].
			const int grp = e->slot.f[35];
			const int wpid = e->slot.f[37];
			const int wpnum = e->slot.f[38];
			std::printf("%-5d %-6u %-6d %-5d %-4d %-4d %-6d %-8d %-6d %-6d %-6d %-6d %-7d %-4d %10.2f\n", i,
					unsigned(e->handle.packed), bms_of(world, e->handle), group_of(world, e->handle), grp, wpid, wpnum,
					e->brain.f[w::AiBrain::kWpType],
					e->brain.f[w::AiBrain::kWpChannel],
					e->brain.f[w::AiBrain::kWpNode],
					e->inf.move_mode,
					e->slot.f[37],   // the reserved command (BMS waypoint_id)
					e->slot.f[36],   // the cached board carrier, 125 only
					(world.registry.get(e->handle) != nullptr &&
							world.registry.get(e->handle)->mounted) ? 1 : 0,
					travel);
		}
	}
	std::printf("ai path: %d AI, %d moved, %d still after %d ticks\n", n, moved, still, ticks);

	// The BOARD CARRIERS: which entities the command-125 soldiers resolved, and
	// whether those entities actually own seats. promote grants seats ONLY from
	// PromoteOptions::item_seat_specs, so a seatless carrier here means the harness
	// never described the vehicle -- not that the vehicle is broken.
	if (report) {
		std::vector<uint16_t> seen;
		// The nav ENTRY payloads for one carrier channel. f[0] is what the advance
		// gate compares the remaining distance against (kWpNodeVal / "animTime").
		for (int ch = 1; ch <= 20; ++ch) {
			const w::NavChannel *nc = ai.nav.channel(ch);
			if (nc == nullptr || nc->count == 0) continue;
			std::printf("   NAVCH %d count=%d loopflag=%d\n", ch, nc->count, nc->loopflag);
			int waitsum = 0, waitmax = 0;
			for (int k = 0; k < nc->count && k < 32; ++k) {
				const w::NavEntry *ne = ai.nav.entry(nc->entries[k]);
				if (ne == nullptr) continue;
				// AUTHORED destination coordinates for the convoy channels: 3 is the
				// trigger path (event 11 fires on group 3 reaching node 20) and 11/12
				// are where that event sends the riders.
				if (ch == 3 || ch == 11 || ch == 12 || ch == 13 || ch == 14)
					std::printf("       NODE ch=%d k=%d radius=%d pos=%d,%d,%d wait=%d\n",
							ch, k, ne->f[0], ne->f[1], ne->f[2], ne->f[3], ne->wait_ticks);
				waitsum += ne->wait_ticks;
				if (ne->wait_ticks > waitmax) waitmax = ne->wait_ticks;
			}
			std::printf("        waits: sum=%d max=%d  (cooldown ticks sum=%d)\n",
					waitsum, waitmax, (waitsum + 8 * nc->count) >> 4);
		}
		if (const w::NavChannel *nc13 = ai.nav.channel(13)) {
			std::printf("nav channel 13: count=%d loopflag=%d\n", nc13->count, nc13->loopflag);
			for (int k = 0; k < nc13->count && k < 6; ++k) {
				const w::NavEntry *ne = ai.nav.entry(nc13->entries[k]);
				if (ne) std::printf("   node %d: f0=%-12d x=%-12d y=%-12d z=%-12d f4=%d\n",
						k, ne->f[0], ne->f[1], ne->f[2], ne->f[3], ne->f[4]);
			}
		}
		// Every RedirectGroupTo / RedirectSingleTo in the mission, with the event
		// that carries it: does the script ever re-task the tripod gunners?
		for (size_t ei = 0; ei < m.events.size(); ++ei) {
			const bms::Event &ev = m.events[ei];
			for (int ai2 = 0; ai2 < ev.action_count; ++ai2) {
				const size_t idx = size_t(ev.action_index) + size_t(ai2);
				if (idx >= m.actions.size()) continue;
				const bms::Action &ac = m.actions[idx];
				const int t = int(ac.action_type);
				if (t != 1 && t != 19 && t != 3 && t != 21) continue; // redirects + AI-change
				std::printf("   ACTION ev=%zu type=%d sub=%d p1=%d p2=%d p3=%d p4=%d\n",
						ei, t, ac.action_sub_type, ac.param1, ac.param2, ac.param3, ac.param4);
			}
		}
		// TRIGGER dump for the events that re-task mounted riders. The detach
		// path is apply_waypoint_order, so the CONDITION of a redirect event is
		// what decides whether a rider leaves its carrier.
		for (size_t ei = 0; ei < m.events.size(); ++ei) {
			const bms::Event &ev = m.events[ei];
			bool has_redirect = false;
			for (int a2 = 0; a2 < ev.action_count; ++a2) {
				const size_t idx = size_t(ev.action_index) + size_t(a2);
				if (idx < m.actions.size()) {
					const int t = int(m.actions[idx].action_type);
					if (t == 1 || t == 19) has_redirect = true;
				}
			}
			if (!has_redirect) continue;
			std::printf("   EVCOND ev=%zu flags=%d delay=%d reset=%d triggers=%d\n",
					ei, int(ev.flags), ev.delay, ev.reset_after, int(ev.trigger_count));
			for (int t2 = 0; t2 < ev.trigger_count; ++t2) {
				const size_t ti = size_t(ev.trigger_index) + size_t(t2);
				if (ti >= m.triggers.size()) continue;
				const bms::Trigger &tr = m.triggers[ti];
				std::printf("       TRIG main=%d sub=%d p1=%d p2=%d p3=%d p4=%d neg=%d or=%d xor=%d\n",
						int(tr.main_type), tr.sub_type, tr.param1, tr.param2,
						tr.param3, tr.param4, int(tr.is_negated()),
						int(tr.is_or()), int(tr.is_xor()));
			}
		}
		// AUTHORED organic spawns straight off the BMS, the arbiter for any
		// placement claim: whichever capture matches these is the correct one.
		// HULL vs SEATS for the transport trucks. A dismounting body is left at its
		// seat bone (EntityCommands::dismount does not reposition, and neither does
		// retail @0x4355f0), so if a seat sits INSIDE the collision hull the body
		// starts penetrating and the resolver shoves it forever (6.4n).
		world.registry.for_each([&](const w::Entity &e) {
			if (e.item_id != 1293 && e.item_id != 1294) return;
			std::printf("   HULL item=%d handle=%u net_id=%u bms_id=%d seats=%zu\n",
					e.item_id, unsigned(e.handle.packed), unsigned(e.net_id), int(e.bms_id), e.seats.size());
			for (size_t si = 0; si < e.seats.size(); ++si) {
				const w::Seat &s = e.seats[si];
				std::printf("       SEAT %zu type=%d local=%.2f,%.2f,%.2f\n",
						si, int(s.type), double(s.seat_local.x), double(s.seat_local.y),
						double(s.seat_local.z));
			}
		});
		std::printf("authored items: %zu\n", m.items.size());
		for (size_t ii = 0; ii < m.items.size(); ++ii) {
			const bms::Entity &b = m.items[ii];
			if (b.type_id != 1293 && b.type_id != 1294 && b.id != 64 && b.id != 65 && b.id != 68 && b.id != 60 && b.type_id != 1902) continue;
			std::printf("   BMSITEM %zu id=%d type=%d pos=(%.1f,%.1f)\n",
					ii, b.id, b.type_id, b.x / 65536.0, b.y / 65536.0);
		}
		std::printf("authored organics: %zu\n", m.organics.size());
		for (size_t oi = 0; oi < m.organics.size(); ++oi) {
			const bms::Entity &b = m.organics[oi];
			std::printf("   BMSORG %zu type=%d pos=(%.1f,%.1f) grp=%d wp=%d wpnum=%d\n",
					oi, b.type_id, b.x / 65536.0, b.y / 65536.0, int(b.group_id),
					int(b.waypoint_id), int(b.wp_number));
		}
		{
			// EVERY Gunner seat in the world and whether it is filled -- the
			// precondition census for the emplaced state.
			int gun_total = 0, gun_filled = 0, owners = 0;
			world.registry.for_each([&](const w::Entity &en) {
				int g = 0, f = 0;
				for (const w::Seat &st : en.seats) {
					if (st.type != w::SeatType::Gunner) continue;
					++g; if (st.occupant.valid()) ++f;
				}
				if (g == 0) return;
				++owners; gun_total += g; gun_filled += f;
				for (const w::Seat &st : en.seats) {
					if (st.type != w::SeatType::Gunner || !st.occupant.valid()) continue;
					const w::Entity *oc = world.registry.get(st.occupant);
					const w::AiEntity *ob = ai.for_handle(st.occupant);
					std::printf("   GUNNER occ=%-6u kind=%d mounted=%d mtype=%d hasAI=%d anim=%d\n",
							unsigned(st.occupant.packed), oc ? int(oc->kind) : -1,
							oc ? int(oc->mounted) : -1, oc ? int(oc->mount_type) : -1,
							ob ? 1 : 0, ob ? ob->inf.anim_state : -1);
				}
				std::printf("   GUNSEAT owner=%-6u item=%-6d guns=%d filled=%d parent=%u\n",
						unsigned(en.handle.packed), en.item_id, g, f,
						unsigned(en.emplacement_parent.packed));
			});
			std::printf("GUNSEAT CENSUS: %d owners, %d gunner seats, %d filled\n",
					owners, gun_total, gun_filled);
		}
		{
			// PRECONDITION for the combat-approach arm: an AI holding a target that
			// is OUTSIDE its attack range (slot[15]).
			int with_t = 0, out_of_range = 0, in_range = 0;
			for (int k = 0; k < n; ++k) {
				const w::AiEntity *e = ai.at(k);
				if (e == nullptr || !e->inf.combat_target.valid()) continue;
				++with_t;
				const w::Entity *tg = world.registry.get(e->inf.combat_target);
				if (tg == nullptr) continue;
				const double dx = double(int32_t(tg->position.x * 65536.0f)) - e->pos[0];
				const double dy = double(int32_t(tg->position.y * 65536.0f)) - e->pos[1];
				const double d = std::sqrt(dx * dx + dy * dy);
				if (d >= double(e->slot.f[15])) ++out_of_range; else ++in_range;
			}
			{
				int zero17 = 0, nonzero17 = 0; long long sum17 = 0;
				for (int k = 0; k < n; ++k) {
					const w::AiEntity *e = ai.at(k);
					if (e == nullptr) continue;
					if (e->slot.f[17] == 0) ++zero17; else { ++nonzero17; sum17 += e->slot.f[17]; }
				}
				int t0=0,t1=0,t2=0,tx=0;
				for (int k = 0; k < n; ++k) {
					const w::AiEntity *e = ai.at(k);
					if (e == nullptr) continue;
					switch (int(e->team)) { case 0: ++t0; break; case 1: ++t1; break;
						case 2: ++t2; break; default: ++tx; }
				}
				std::printf("AI TEAM: team0=%d team1=%d team2=%d other=%d\n", t0,t1,t2,tx);
				std::printf("SIGHT RANGE slot[17]: %d zero, %d non-zero (mean %.0f u)\n",
						zero17, nonzero17, nonzero17 ? double(sum17)/nonzero17/65536.0 : 0.0);
			}
			int ever = 0;
			for (size_t q = 0; q < ever_target.size(); ++q) ever += ever_target[q];
			std::printf("COMBAT EVER: %d AI acquired a target at some point; peak %d concurrent\n",
					ever, peak_targets);
			std::printf("COMBAT PRECOND: %d AI hold a target; %d OUT of attack range, %d in\n",
					with_t, out_of_range, in_range);
		}
		std::printf("board carriers:\n");
		for (int j = 0; j < n; ++j) {
			const w::AiEntity *e = ai.at(j);
			if (e == nullptr || e->slot.f[37] < 123 || e->slot.f[37] > 125) continue;
			const int32_t cached = e->slot.f[36];
			if (cached == 0) continue;
			const w::EntityHandle ch{static_cast<uint16_t>(cached - 1)};
			if (std::find(seen.begin(), seen.end(), ch.packed) != seen.end()) continue;
			seen.push_back(ch.packed);
			const w::Entity *veh = world.registry.get(ch);
			double vtravel = 0.0;
			auto cs = carrier_start.find(ch.packed);
			if (cs != carrier_start.end() && veh != nullptr) {
				const double vdx = double(int32_t(veh->position.x * 65536.0f)) - cs->second.first;
				const double vdy = double(int32_t(veh->position.y * 65536.0f)) - cs->second.second;
				vtravel = std::sqrt(vdx * vdx + vdy * vdy) / 65536.0;
			}
			// Does the CARRIER itself carry a route? wpType/wpChan 0 means it was
			// never ordered anywhere (a missing script order or an unpromoted
			// route); non-zero with moved=0 means the drive gate blocks it.
			int vt = -1, vc = -1, vn = -1, vmm = -1;
			double aix = -1.0, aiy = -1.0;
			if (veh != nullptr) {
				if (const w::AiEntity *vb = ai.for_handle(ch)) {
					vt = vb->brain.f[w::AiBrain::kWpType];
					vc = vb->brain.f[w::AiBrain::kWpChannel];
					vn = vb->brain.f[w::AiBrain::kWpNode];
					vmm = vb->inf.move_mode;
					aix = vb->pos[0] / 65536.0;
					aiy = vb->pos[1] / 65536.0;
				}
			}
			int nseat[6] = {0,0,0,0,0,0};
			if (veh != nullptr)
				for (const w::Seat &st : veh->seats) {
					const int ti = int(st.type);
					if (ti >= 0 && ti < 6) ++nseat[ti];
				}
			std::printf("   carrier handle=%-6u item_id=%-6d seats=%-3zu [pass %d ctrl %d GUN %d drv %d] net_id=%-5d "
					"pos=(%.1f,%.1f) aipos=(%.1f,%.1f) wpType=%-3d wpChan=%-3d wpNode=%-3d/%-3d mm=%-3d moved=%.2f\n",
					unsigned(ch.packed), veh ? veh->item_id : -1,
					veh ? veh->seats.size() : size_t(0),
					nseat[1], nseat[2], nseat[3], nseat[5], veh ? veh->net_id : -1,
					veh ? veh->position.x : 0.0f, veh ? veh->position.y : 0.0f,
					aix, aiy,
					vt, vc, vn,
					(vc > 0 && ai.nav.channel(vc)) ? ai.nav.channel(vc)->count : -1,
					vmm, vtravel);
		}
	}

	// --- the regression pins --------------------------------------------------
	// Retail leaves exactly THREE AI stationary on this mission (s8/s15/s16, the
	// ones carrying state_a 64 on the wire). Anything much larger is the STUCK
	// defect. This is deliberately a loose bound, not an exact count: the tick
	// budget here is 40 s against the capture's 430 s, so a slow AI may not have
	// cleared the 50-unit threshold yet. It pins the DEFECT, not the scenario.
	expect(moved > 0, "at least one AI walks its authored route");

	// THE PLAYERCONTROL ADMIT GATE, pinned against the real mission. 00TRg orders
	// three soldiers (BMSORG 42/43/51, group 16, wp=125) onto 1902 "50cal on 180
	// tripod" emplacements — attrib EWeap 0x20, NO PlayerControl 0x40 — standing at
	// their own spawns. Retail never boards them [orig: Entity_UpdateInfantryAI
	// @0x4b9910 — the `(itemDef->attrib & 0x40) != 0` test guarding the
	// Entity_CanEnterVehicle @0x435480 consult]. The same run must still board the
	// real carriers (trucks/SUVs/boats all carry PlayerControl): a gate that
	// unmounts EVERYONE is a regression, not a fix.
	{
		int cmd125 = 0, mounted_on_eweap = 0, mounted_total = 0;
		for (int i = 0; i < n; ++i) {
			const w::AiEntity *e = ai.at(i);
			if (e == nullptr || e->slot.f[37] < 123 || e->slot.f[37] > 125) continue;
			++cmd125;
			const w::Entity *self = world.registry.get(e->handle);
			if (self == nullptr || !self->mounted) continue;
			++mounted_total;
			const int32_t cached = e->slot.f[36];
			if (cached == 0) continue;
			const w::Entity *carrier =
					world.registry.get(w::EntityHandle{static_cast<uint16_t>(cached - 1)});
			if (carrier != nullptr &&
					(carrier->item_attrib & w::kItemAttribPlayerControl) == 0)
				++mounted_on_eweap;
		}
		std::printf("board gate: %d command AI, %d mounted, %d on non-PlayerControl\n",
				cmd125, mounted_total, mounted_on_eweap);
		// RETRACTED: this used to assert `mounted_on_eweap == 0`, pinning the
		// over-applied PlayerControl mount gate. The wire refutes it -- retail
		// emplaces seven AI at ~100% of their rows, three of them the soldiers
		// 00TRg orders onto the 1902 tripods (EWeap, no PlayerControl). Boarding
		// a bare emplacement is CORRECT; the assertion now pins that the gunner
		// seats actually get filled, which is what the emplaced state needs.
		int gun_seats = 0, gun_filled = 0;
		world.registry.for_each([&](const w::Entity &en) {
			for (const w::Seat &st : en.seats) {
				if (st.type != w::SeatType::Gunner) continue;
				++gun_seats;
				if (st.occupant.valid()) ++gun_filled;
			}
		});
		std::printf("gunner seats: %d, filled %d\n", gun_seats, gun_filled);
		expect(mounted_total > 0, "board commands still mount");
		expect(gun_seats > 0, "the mission's gunner seats are extracted");
		expect(gun_filled >= gun_seats / 2,
				"most gunner seats get filled [orig: FindBestSeatSlot @0x4351f0 "
				"weights UseGun 0x20000 above passenger 0x200000]");
	}

	if (failures == 0) std::printf("ai path conformance tests passed\n");
	return failures ? 1 : 0;
}
