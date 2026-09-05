// 00TRg "move in to defend" host-side diagnosis on the retail data. SP is a
// listen server, so this exercises the exact run_logic_tick(is_authority)
// path a LAN host runs: a retail host delivers 00TRg's AI boat/truck defense
// waves, ours parks them (the D-NET-161 deferral tail; D-AI-11 / D-INF-2
// residuals).
//
// Mission mechanism (00TRg.mis, decoded 2026-08-06): no vehicle carries an
// authored route; the crews command-mount at spawn (waypoint_id 125 -> ride
// SSN wpnumber, authored ON their vehicles), then BMS events issue the routes:
//   event 18 (no delay) : RedirectGroupTo(5->list 5) + PatrolSpeed 30    patrol boat 60
//   event 19 (delay 15) : RedirectGroupTo(6->list 6) + PatrolSpeed 60    Zodiac 56
//   event 21 (delay 15) : RedirectGroupTo(7->list 7) (+speed on grp 6)   Zodiac 57
//   event 8  (delay 30) : RedirectGroupTo(3->list 3) + PatrolSpeed 40    trucks
//   event 20/22 (GroupAtWaypoint 6@node6 / 7@node10, delay 1): debark redirects for
//   defender groups 14/15 (-> lists 15/16) + red alert.
// Delays arm delay<<6 ticks; triggers evaluate per 64-tick quantum.
//
// Gates (the fix's acceptance bar):
//   P0 promote : every vehicle has seats + an AI brain row + a live ctrl/drvr occupant
//   P1 orders  : each vehicle brain takes its list (wp_channel) + SM state 16 post-event
//   P2 motion  : planar displacement after the kickoff; boats hold authored z (afloat)
//   P3 arrival : debark events 20/22 fire; defender groups 14/15 take their redirects
// P0 is asserted; P1-P3 are REPORTED until D-NET-161 closes (the divergence
// ledger carries the tail) — a FAIL line there is the known state, not a
// regression of this test. `--ticks N` sets the run budget (default 15000).
// Gated on OPENNOVA_JO_DIR (a retail JO install carrying 00TRg.bms).
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

constexpr int kSnapshotEveryTicks = 620;
constexpr int kBaselineTick = 400;
constexpr float kBoatAuthoredZ = 10.5f;
constexpr float kBoatZTolerance = 2.0f;
constexpr float kMinDepartDistance = 15.0f;

struct VehicleRow {
	uint16_t ssn;
	int group;
	int list;
	int kickoff_event;
	const char *family;
};
const VehicleRow kVehicles[] = {
	{60, 5, 5, 18, "boat"}, {56, 6, 6, 19, "boat"}, {57, 7, 7, 21, "boat"},
	{58, 3, 3, 8, "ground"}, {59, 3, 3, 8, "ground"}, {62, 3, 3, 8, "ground"},
	{63, 3, 3, 8, "ground"}, {1664, 3, 3, 8, "ground"},
};
const int kKickoffEvents[] = {18, 19, 21, 8};
// debark event -> {defender group, redirected-to list}
const std::map<int, std::pair<int, int>> kDebarkEvents = {{20, {14, 15}}, {22, {15, 16}}};
const int kTruckChainEvents[] = {9, 10, 11, 12, 13, 14, 15};

void gate(const char *name, bool ok, const std::string &detail, bool asserted) {
	std::printf("VERDICT %s: %s — %s%s\n", name, ok ? "PASS" : "FAIL", detail.c_str(),
			(!ok && !asserted) ? " (reported; D-NET-161)" : "");
	if (asserted) expect(ok, name);
}

bool ctrl_seat_occupied(const w::Entity &veh) {
	for (const w::Seat &s : veh.seats)
		if ((s.type == w::SeatType::Controller || s.type == w::SeatType::Driver) && s.occupant.valid())
			return true;
	return false;
}

} // namespace

int main(int argc, char **argv) {
	int run_ticks = 15000;
	for (int i = 1; i + 1 < argc; ++i)
		if (std::strcmp(argv[i], "--ticks") == 0) run_ticks = std::max(1, std::atoi(argv[i + 1]));
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
			"OPENNOVA_JO_DIR (a retail JO install carrying 00TRg.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "00TRg.bms", error)) return retail::skip(error.c_str());
	testrig::BootOptions options;
	if (!expect(rig.boot(options, error), "00TRg boots")) {
		std::fprintf(stderr, "  %s\n", error.c_str());
		return 1;
	}
	if (!expect(rig.local.has_local_player(), "the host's own player spawned")) return 1;

	// --- P0 baseline: promote results (seats, brains, crews) settle well before
	// the first event quantum can matter for the kickoffs we watch.
	while (rig.world.logic_tick < static_cast<uint32_t>(kBaselineTick)) rig.tick();
	std::map<uint16_t, int> riders_by_target;
	std::map<uint16_t, int> mounted_by_target;
	rig.world.registry.for_each([&](const w::Entity &e) {
		if (e.handle.pool() != 0 || e.waypoint_id < 123 || e.waypoint_id > 125) return;
		const uint16_t target = static_cast<uint16_t>(e.wp_number);
		++riders_by_target[target];
		if (e.mounted && e.mount_target.valid()) {
			const w::Entity *carrier = rig.world.registry.get(e.mount_target);
			if (carrier != nullptr && carrier->net_id == target) ++mounted_by_target[target];
		}
	});
	bool p0_ok = true;
	std::string p0_detail;
	for (const VehicleRow &row : kVehicles) {
		const w::Entity *veh = rig.world.registry.by_net_id(row.ssn);
		char line[256];
		if (veh == nullptr) {
			p0_ok = false;
			std::snprintf(line, sizeof(line), "ssn %u MISSING from the world; ", unsigned(row.ssn));
			p0_detail += line;
			continue;
		}
		const bool has_brain = rig.world.ai.for_handle(veh->handle) != nullptr;
		const bool ctrl = ctrl_seat_occupied(*veh);
		std::printf("P0 ssn=%u fam=%s seats=%zu brain=%d ctrl_occupied=%d riders=%d mounted=%d\n",
				unsigned(row.ssn), row.family, veh->seats.size(), int(has_brain), int(ctrl),
				riders_by_target[row.ssn], mounted_by_target[row.ssn]);
		if (veh->seats.empty() || !has_brain || !ctrl) {
			p0_ok = false;
			std::snprintf(line, sizeof(line), "ssn %u: seats=%zu brain=%d ctrl=%d; ", unsigned(row.ssn),
					veh->seats.size(), int(has_brain), int(ctrl));
			p0_detail += line;
		}
	}
	gate("P0 promote", p0_ok, p0_ok ? "all 8 crewed+brained" : p0_detail, /*asserted=*/true);

	// --- Snapshot loop: watch orders land, motion accumulate, events fire.
	std::map<int, int> kickoff_fired;
	std::map<int, int> debark_fired;
	std::map<int, std::map<uint16_t, w::Vec3>> debark_positions;
	std::map<uint16_t, w::Vec3> kickoff_pos;
	std::map<uint16_t, int> order_seen, state16_seen;
	std::map<uint16_t, float> max_boat_dz;
	int next_snapshot = static_cast<int>(rig.world.logic_tick);
	while (static_cast<int>(rig.world.logic_tick) < run_ticks) {
		while (static_cast<int>(rig.world.logic_tick) < next_snapshot) rig.tick();
		next_snapshot += kSnapshotEveryTicks;
		const int tick = static_cast<int>(rig.world.logic_tick);
		for (const int ev : kKickoffEvents) {
			if (kickoff_fired.count(ev) || !rig.events.event_fired(static_cast<size_t>(ev))) continue;
			kickoff_fired[ev] = tick;
			std::printf("t=%d event %d FIRED (kickoff)\n", tick, ev);
			for (const VehicleRow &row : kVehicles)
				if (row.kickoff_event == ev)
					if (const w::Entity *veh = rig.world.registry.by_net_id(row.ssn)) kickoff_pos[row.ssn] = veh->position;
		}
		for (const auto &kv : kDebarkEvents) {
			const int ev = kv.first;
			if (debark_fired.count(ev) || !rig.events.event_fired(static_cast<size_t>(ev))) continue;
			debark_fired[ev] = tick;
			std::printf("t=%d event %d FIRED (debark)\n", tick, ev);
			const int grp = kv.second.first;
			std::map<uint16_t, w::Vec3> positions;
			rig.world.registry.for_each([&](const w::Entity &e) {
				if (e.handle.pool() == 0 && int(e.group_id) == grp) positions[e.handle.packed] = e.position;
			});
			debark_positions[ev] = positions;
		}
		for (const int ev : kTruckChainEvents) {
			if (kickoff_fired.count(ev) || !rig.events.event_fired(static_cast<size_t>(ev))) continue;
			kickoff_fired[ev] = tick;
			std::printf("t=%d event %d FIRED (truck chain)\n", tick, ev);
		}
		std::string line;
		for (const VehicleRow &row : kVehicles) {
			const w::Entity *veh = rig.world.registry.by_net_id(row.ssn);
			if (veh == nullptr) continue;
			char buf[200];
			std::snprintf(buf, sizeof(buf), "ssn=%u pos=(%.1f,%.1f,%.2f)", unsigned(row.ssn), veh->position.x,
					veh->position.y, veh->position.z);
			line += buf;
			if (const w::AiEntity *brain = rig.world.ai.for_handle(veh->handle)) {
				const int wp_ch = brain->brain.f[w::AiBrain::kWpChannel];
				const int st = brain->brain.cur_state();
				std::snprintf(buf, sizeof(buf), " st=%d wp=%d node=%d spd=%d", st, wp_ch,
						brain->brain.f[w::AiBrain::kWpNode], brain->brain.f[w::AiBrain::kOutSpeed]);
				line += buf;
				if (wp_ch == row.list && !order_seen.count(row.ssn)) order_seen[row.ssn] = tick;
				if (st == 16 && !state16_seen.count(row.ssn)) state16_seen[row.ssn] = tick;
			}
			if (std::strcmp(row.family, "boat") == 0 && kickoff_pos.count(row.ssn)) {
				const float dz = std::fabs(veh->position.z - kBoatAuthoredZ);
				max_boat_dz[row.ssn] = std::max(max_boat_dz[row.ssn], dz);
			}
			if (kickoff_pos.count(row.ssn)) {
				std::snprintf(buf, sizeof(buf), " dep=%.1f", testrig::planar_distance(veh->position, kickoff_pos[row.ssn]));
				line += buf;
			}
			line += " | ";
		}
		std::printf("t=%d | %s\n", tick, line.c_str());
	}

	// --- Verdicts (reported).
	bool p1_ok = true, p2_ok = true;
	std::string p1_detail, p2_detail;
	for (const VehicleRow &row : kVehicles) {
		char buf[200];
		if (!kickoff_fired.count(row.kickoff_event)) {
			p1_ok = false;
			std::snprintf(buf, sizeof(buf), "ssn %u: kickoff event %d never fired; ", unsigned(row.ssn), row.kickoff_event);
			p1_detail += buf;
			continue;
		}
		if (!order_seen.count(row.ssn)) {
			p1_ok = false;
			std::snprintf(buf, sizeof(buf), "ssn %u: wp_channel never became list %d; ", unsigned(row.ssn), row.list);
			p1_detail += buf;
		} else if (!state16_seen.count(row.ssn)) {
			p1_ok = false;
			std::snprintf(buf, sizeof(buf), "ssn %u: SM state 16 never observed; ", unsigned(row.ssn));
			p1_detail += buf;
		}
		const w::Entity *veh = rig.world.registry.by_net_id(row.ssn);
		const float dep = veh != nullptr ? testrig::planar_distance(veh->position, kickoff_pos[row.ssn]) : 0.0f;
		if (dep < kMinDepartDistance) {
			p2_ok = false;
			std::snprintf(buf, sizeof(buf), "ssn %u (%s): moved %.1fu (< %.0fu); ", unsigned(row.ssn), row.family, dep,
					kMinDepartDistance);
			p2_detail += buf;
		}
		if (std::strcmp(row.family, "boat") == 0 && max_boat_dz[row.ssn] > kBoatZTolerance) {
			p2_ok = false;
			std::snprintf(buf, sizeof(buf), "ssn %u: boat left the water plane (max dz %.2fu); ", unsigned(row.ssn),
					max_boat_dz[row.ssn]);
			p2_detail += buf;
		}
	}
	gate("P1 orders", p1_ok, p1_ok ? "routes+state16 on all 8" : p1_detail, false);
	gate("P2 motion", p2_ok, p2_ok ? "all departed afloat/rolling" : p2_detail, false);

	bool p3_ok = true;
	std::string p3_detail;
	for (const auto &kv : kDebarkEvents) {
		const int ev = kv.first, grp = kv.second.first, lst = kv.second.second;
		char buf[200];
		if (!debark_fired.count(ev)) {
			p3_ok = false;
			std::snprintf(buf, sizeof(buf), "event %d never fired; ", ev);
			p3_detail += buf;
			continue;
		}
		int members = 0, redirected = 0;
		float max_walk = 0.0f;
		const std::map<uint16_t, w::Vec3> &start = debark_positions[ev];
		rig.world.registry.for_each([&](const w::Entity &e) {
			if (e.handle.pool() != 0 || int(e.group_id) != grp) return;
			++members;
			if (const w::AiEntity *brain = rig.world.ai.for_handle(e.handle))
				if (brain->brain.f[w::AiBrain::kWpChannel] == lst) ++redirected;
			const auto s = start.find(e.handle.packed);
			if (s != start.end()) max_walk = std::max(max_walk, testrig::distance(e.position, s->second));
		});
		if (members == 0 || redirected == 0) {
			p3_ok = false;
			std::snprintf(buf, sizeof(buf), "event %d: group %d redirect to list %d not taken (%d/%d); ", ev, grp, lst,
					redirected, members);
			p3_detail += buf;
		} else if (max_walk < 10.0f) {
			p3_ok = false;
			std::snprintf(buf, sizeof(buf), "event %d: group %d routed but FROZEN (max walk %.1fu); ", ev, grp, max_walk);
			p3_detail += buf;
		}
	}
	gate("P3 arrival", p3_ok, p3_ok ? "debark chain live" : p3_detail, false);

	if (failures == 0)
		std::printf("defense_00trg: P0 holds; P1-P3 %s\n",
				(p1_ok && p2_ok && p3_ok) ? "host-live" : "reported (the D-NET-161 tail)");
	return failures == 0 ? 0 : 1;
}
