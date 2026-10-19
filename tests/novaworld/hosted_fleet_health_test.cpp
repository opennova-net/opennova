// The fleet's health (apps/novaworld_common/hosted/model/fleet_health.h) over hand-made rows:
// each rule just under and just over its bound, the level, the subject, the admin page it
// links (the site's paths, CONTRACT "Attention links") and its rule's name (a server whose
// data cannot be fetched start after start among them); stale rows a
// warn "unknown", never healthy (nor in the heartbeat's least free disk); a machine's
// outdated by each image pair; the order (danger, warn, info, each oldest first, a finding
// with no time after the rest) and the heartbeat's gauges; the static addresses' rules (a
// release stuck, addresses held with the add-on off). Pure functions: no database.

#include "hosted/model/fleet_health.h"

#include "common/test_expect.h"

#include <cstdio>
#include <string>
#include <vector>

namespace hosted = opennova::novaworld_server::hosted;
using hosted::Finding;
using hosted::FleetMachineState;
using hosted::HealthLevel;
using hosted::State;

namespace {

constexpr int64_t kNow = 1792324800; // 2026-10-18T12:00:00Z

hosted::HealthMachine machine(int64_t id, FleetMachineState state, int64_t in_state_s) {
	hosted::HealthMachine m;
	m.row.id = id;
	m.row.state = state;
	m.row.kind = hosted::MachineKind::Shared;
	m.state_since = kNow - in_state_s;
	if (state == FleetMachineState::Ready || state == FleetMachineState::Draining) {
		m.last_report = kNow - 2;
		hosted::MachineStats s;
		s.cpu_percent = 20;
		s.memory_used_bytes = 1ull << 30;
		s.memory_total_bytes = 4ull << 30;
		s.disk_free_bytes = 20ull << 30;
		s.disk_total_bytes = 40ull << 30;
		m.row.stats = s;
		m.stats_at = kNow - 2;
	}
	return m;
}

hosted::HealthServer server(int64_t id, State state, int64_t in_state_s, int64_t machine_id = 0) {
	hosted::HealthServer s;
	s.id = id;
	s.name = "Smoke " + std::to_string(id);
	s.state = state;
	s.state_since = kNow - in_state_s;
	s.machine_id = machine_id;
	return s;
}

// The inputs of a healthy fleet: a lobby that takes clients, nothing else.
hosted::HealthInputs healthy() {
	hosted::HealthInputs in;
	in.now = kNow;
	in.lobbies_live = 1;
	return in;
}

std::vector<Finding> of(const std::vector<Finding> &all, const std::string &source) {
	std::vector<Finding> out;
	for (const Finding &f : all)
		if (f.source == source)
			out.push_back(f);
	return out;
}

// The one finding about `subject` among `found`, or none.
const Finding *about(const std::vector<Finding> &found, const std::string &subject) {
	for (const Finding &f : found)
		if (f.subject == subject)
			return &f;
	return nullptr;
}

int test_healthy() {
	hosted::HealthInputs in = healthy();
	in.machines.push_back(machine(1, FleetMachineState::Ready, 3600));
	hosted::HealthServer s = server(10, State::Running, 3600, 1);
	s.tick = hosted::TickHealth{ 1.2, 4.5, 9.0, 3 };
	in.servers.push_back(s);
	in.servers.push_back(server(11, State::Booting, hosted::kStalledStartS, 1));
	TEST_EXPECT(hosted::fleet_health(in, {}).empty());
	const hosted::HealthGauges g = hosted::health_gauges(in, {});
	TEST_EXPECT(
			g.danger == 0 && g.warn == 0 && g.machines_stuck == 0 && g.disk_free_min_percent == 50);
	return 0;
}

int test_stalled_start() {
	hosted::HealthInputs in = healthy();
	in.machines.push_back(machine(1, FleetMachineState::Ready, 3600));
	for (const State state :
			{ State::Requested, State::Provisioning, State::Booting, State::Listing })
		in.servers.push_back(server(static_cast<int64_t>(in.servers.size()) + 10, state,
				hosted::kStalledStartS + 1, 1));
	// Running and stopping servers are no start.
	in.servers.push_back(server(20, State::Stopping, 3600, 1));
	auto found = of(hosted::fleet_health(in, {}), "stalled_start");
	TEST_EXPECT(found.size() == 4);
	TEST_EXPECT(found[0].level == HealthLevel::Warn && found[0].subject == "Server 10 (Smoke 10)" &&
			found[0].link == "/admin/fleet/10" &&
			found[0].since == kNow - hosted::kStalledStartS - 1);
	TEST_EXPECT(found[2].text == "Booting for 15 min: a start takes under 15 min.");
	// Held for its machine's static address, it says so.
	in.machines[0].address_wait = true;
	found = of(hosted::fleet_health(in, {}), "stalled_start");
	TEST_EXPECT(found.size() == 4 &&
			found[2].text ==
					"Booting for 15 min, waiting for its static address to reach its machine 1.");
	return 0;
}

int test_stuck_machines() {
	hosted::HealthInputs in = healthy();
	struct Case {
		FleetMachineState state;
		int64_t bound;
	};
	const Case cases[] = { { FleetMachineState::Launching, hosted::kStuckBootS },
		{ FleetMachineState::Booting, hosted::kStuckBootS },
		{ FleetMachineState::Draining, hosted::kStuckDrainS },
		{ FleetMachineState::Ending, hosted::kStuckEndS },
		{ FleetMachineState::Lost, hosted::kStuckLostS } };
	int64_t id = 1;
	for (const Case &c : cases) {
		in.machines.push_back(machine(id++, c.state, c.bound)); // at its bound: not yet
		in.machines.push_back(machine(id++, c.state, c.bound + 1));
	}
	// A ready machine is never stuck, however long.
	in.machines.push_back(machine(id++, FleetMachineState::Ready, 30 * 24 * 3600));
	const auto all = hosted::fleet_health(in, {});
	const auto found = of(all, "stuck_machine");
	TEST_EXPECT(found.size() == 5);
	for (const Finding &f : found) {
		TEST_EXPECT(f.level == HealthLevel::Warn);
		TEST_EXPECT(f.link ==
				"/admin/fleet/machines/" + f.subject.substr(8, f.subject.find(' ', 8) - 8));
	}
	// The oldest first: the draining one, then the launching and the booting ones.
	TEST_EXPECT(found[0].subject == "Machine 6 (shared)" &&
			found[0].text ==
					"Draining for 60 min (over 60 min): its servers have not moved off it.");
	const Finding *booting = about(found, "Machine 4 (shared)");
	TEST_EXPECT(booting != nullptr &&
			booting->text == "Booting for 20 min (over 20 min): its agent has not reported." &&
			booting->link == "/admin/fleet/machines/4");
	// The brake holds a lost machine: a danger of its own, not a stuck one.
	in.machines[9].row.lost_held_since_ms = (kNow - 60) * 1000;
	const auto held = of(hosted::fleet_health(in, {}), "lost_held");
	TEST_EXPECT(held.size() == 1 && held[0].level == HealthLevel::Danger &&
			held[0].link == "/admin/fleet/machines/10" && held[0].since == kNow - 60);
	TEST_EXPECT(of(hosted::fleet_health(in, {}), "stuck_machine").size() == 4);
	// Moving state over and over.
	hosted::HealthInputs flap = healthy();
	flap.machines.push_back(machine(7, FleetMachineState::Ready, 60));
	flap.machines[0].moves_last_hour = hosted::kMovesPerHour;
	TEST_EXPECT(of(hosted::fleet_health(flap, {}), "flapping_machine").empty());
	flap.machines[0].moves_last_hour = hosted::kMovesPerHour + 1;
	const auto flapping = hosted::fleet_health(flap, {});
	TEST_EXPECT(of(flapping, "flapping_machine").size() == 1);
	TEST_EXPECT(hosted::health_gauges(flap, flapping).machines_stuck == 1);
	TEST_EXPECT(hosted::health_gauges(in, all).machines_stuck == 5);
	return 0;
}

int test_protocol_and_images() {
	hosted::HealthInputs in = healthy();
	in.machines.push_back(machine(1, FleetMachineState::Ready, 3600));
	in.machines[0].row.agent_protocol = hosted::kAgentProtocol + 1;
	in.machines[0].mismatch_since = kNow - 120;
	auto all = hosted::fleet_health(in, {});
	const auto mismatch = of(all, "protocol_mismatch");
	TEST_EXPECT(mismatch.size() == 1 && mismatch[0].level == HealthLevel::Danger &&
			mismatch[0].since == kNow - 120 && mismatch[0].link == "/admin/fleet/machines/1");

	// The release's images against each machine's.
	hosted::ReleaseImages release;
	release.image_id = "ami-0new";
	release.agent_image = "ghcr.io/opennova-net/opennova-hostd@sha256:"
						  "1111111111111111111111111111111111111111111111111111111111111111";
	hosted::FleetMachineRow m;
	TEST_EXPECT(!hosted::machine_outdated(m, release)); // neither known: unknown
	m.image_id = "ami-0new";
	TEST_EXPECT(hosted::machine_outdated(m, release) == false);
	m.agent_image = "ghcr.io/opennova-net/opennova-hostd@sha256:"
					"2222222222222222222222222222222222222222222222222222222222222222";
	TEST_EXPECT(hosted::machine_outdated(m, release) == true);
	m.agent_image = release.agent_image;
	m.image_id = "ami-0old";
	TEST_EXPECT(hosted::machine_outdated(m, release) == true);
	TEST_EXPECT(!hosted::machine_outdated(m, {})); // the release pins none here

	hosted::HealthInputs fleet = healthy();
	for (int64_t id = 1; id <= 3; ++id) {
		fleet.machines.push_back(machine(id, FleetMachineState::Ready, 3600));
		fleet.machines.back().row.image_id = id == 3 ? "ami-0new" : "ami-0old";
		fleet.machines.back().row.agent_image = release.agent_image;
	}
	fleet.machines[1].row.agent_image =
			"ghcr.io/opennova-net/opennova-hostd@sha256:" + std::string(64, '2');
	all = hosted::fleet_health(fleet, release);
	const auto agent = of(all, "outdated_agent");
	TEST_EXPECT(agent.size() == 1 && agent[0].level == HealthLevel::Warn &&
			agent[0].link == "/admin/fleet/machines/2" &&
			agent[0].text ==
					"Its hostd runs sha256:222222222222, not the release's sha256:111111111111: "
					"app "
					"deploy rolls hostd (fleet status).");
	const auto image = of(all, "outdated_image");
	TEST_EXPECT(image.size() == 1 && image[0].level == HealthLevel::Info &&
			image[0].link == "/admin/fleet?tab=roll" &&
			image[0].text ==
					"Machines 1, 2 run another image than the launch templates' ami-0new: a fleet "
					"roll replaces them.");
	return 0;
}

int test_pressure_and_ticks() {
	hosted::HealthInputs in = healthy();
	in.machines.push_back(machine(1, FleetMachineState::Ready, 3600));
	hosted::MachineStats &s = *in.machines[0].row.stats;
	s.cpu_percent = hosted::kCpuBusyPercent;
	s.memory_used_bytes = s.memory_total_bytes * 9 / 10;
	s.disk_free_bytes = s.disk_total_bytes * 15 / 100;
	TEST_EXPECT(of(hosted::fleet_health(in, {}), "machine_pressure").empty());
	s.cpu_percent = 92;
	s.memory_used_bytes = s.memory_total_bytes * 95 / 100;
	s.disk_free_bytes = s.disk_total_bytes / 10;
	auto found = of(hosted::fleet_health(in, {}), "machine_pressure");
	TEST_EXPECT(found.size() == 1 && found[0].link == "/admin/fleet/machines/1" &&
			found[0].text == "CPU 92 %, memory 95 % of 4.0 GiB, disk 10 % free of 40.0 GiB.");
	TEST_EXPECT(hosted::health_gauges(in, {}).disk_free_min_percent == 10);

	// Late ticks on its servers, and a running server with none reported.
	hosted::HealthInputs ticks = healthy();
	ticks.machines.push_back(machine(2, FleetMachineState::Ready, 3600));
	hosted::HealthServer on_time = server(30, State::Running, 3600, 2);
	on_time.tick = hosted::TickHealth{ 1.0, 5.0, 12.0, hosted::kLateTicksPerMinute };
	hosted::HealthServer late = server(31, State::Running, 3600, 2);
	late.tick = hosted::TickHealth{ 3.0, 40.0, 90.0, 120 };
	ticks.servers = { on_time, late, server(32, State::Running, hosted::kStaleStatsS, 2),
		server(33, State::Running, hosted::kStaleStatsS + 1, 2) };
	const auto all = hosted::fleet_health(ticks, {});
	found = of(all, "late_ticks");
	TEST_EXPECT(found.size() == 1 && found[0].subject == "Machine 2 (shared)" &&
			found[0].link == "/admin/fleet/machines/2" &&
			found[0].text ==
					"Server 31 (120 late) ran over 1 % of the last minute's ticks late: the "
					"machine is "
					"short of CPU.");
	found = of(all, "unknown");
	TEST_EXPECT(found.size() == 1 && found[0].level == HealthLevel::Warn &&
			found[0].text ==
					"The tick health of server 33 is unknown: no fresh stats file in its agent's "
					"report.");
	return 0;
}

int test_stale_reports() {
	hosted::HealthInputs in = healthy();
	// Silent past the bound and not marked lost: unknown, no pressure judged on old use.
	in.machines.push_back(machine(1, FleetMachineState::Ready, 3600));
	in.machines[0].last_report = kNow - hosted::kStaleReportS - 1;
	in.machines[0].row.stats->cpu_percent = 99;
	// Its use not reported lately (one just ready has a while to report it).
	in.machines.push_back(machine(2, FleetMachineState::Ready, 3600));
	in.machines[1].stats_at = kNow - hosted::kStaleStatsS - 1;
	in.machines.push_back(machine(3, FleetMachineState::Ready, 60));
	in.machines[2].row.stats.reset();
	in.machines[2].stats_at = 0;
	in.machines.push_back(machine(4, FleetMachineState::Draining, 3600));
	in.machines[3].row.stats.reset();
	in.machines[3].stats_at = 0;
	const auto all = hosted::fleet_health(in, {});
	TEST_EXPECT(of(all, "machine_pressure").empty());
	const auto found = of(all, "unknown");
	TEST_EXPECT(found.size() == 3);
	// The oldest first: machine 2's use (5 min), machine 1's silence (3 min), then the
	// draining one that never reported its use (no time).
	TEST_EXPECT(found[0].link == "/admin/fleet/machines/2" && found[0].level == HealthLevel::Warn &&
			found[0].text == "Its use is unknown: its agent last reported it 5 min ago.");
	TEST_EXPECT(found[1].link == "/admin/fleet/machines/1" &&
			found[1].text ==
					"Its last report is 3 min old and it is not marked lost: its state is unknown "
					"(is "
					"the fleet program passing?).");
	TEST_EXPECT(found[2].link == "/admin/fleet/machines/4" &&
			found[2].text == "Its use is unknown: its agent has not reported it." &&
			found[2].since == 0);
	// Nor does the heartbeat's least free disk take their old use (the machine-disk
	// alarm would read a figure from before the disk filled): unset with no use
	// fresh, else the fresh machines' alone.
	in.machines[0].row.stats->disk_free_bytes = in.machines[0].row.stats->disk_total_bytes / 20;
	in.machines[1].row.stats->disk_free_bytes = in.machines[1].row.stats->disk_total_bytes / 20;
	TEST_EXPECT(!hosted::health_gauges(in, all).disk_free_min_percent);
	in.machines.push_back(machine(5, FleetMachineState::Ready, 3600));
	in.machines[4].row.stats->disk_free_bytes = in.machines[4].row.stats->disk_total_bytes * 3 / 10;
	TEST_EXPECT(hosted::health_gauges(in, all).disk_free_min_percent == 30);
	in.machines[4].stats_at = kNow - hosted::kStaleStatsS;
	TEST_EXPECT(hosted::health_gauges(in, all).disk_free_min_percent == 30);
	in.machines[4].stats_at = kNow - hosted::kStaleStatsS - 1;
	TEST_EXPECT(!hosted::health_gauges(in, all).disk_free_min_percent);
	return 0;
}

int test_launches_lobby_failures() {
	hosted::HealthInputs in = healthy();
	hosted::HeldLaunches held;
	held.kind = hosted::MachineKind::Own;
	held.backoff.failures = 2;
	held.backoff.not_before_ms = (kNow + 600) * 1000;
	held.machine_id = 9;
	held.cause = "the launch failed (VcpuLimitExceeded)";
	held.failed_at = kNow - 30;
	in.held.push_back(held);
	in.lobbies_live = 0;
	in.lobby_last_seen = kNow - 400;
	in.failed.push_back({ 41, "Broken", "its admin link stopped answering", kNow - 3600 });
	in.failed_addresses.push_back({ 5, "smoke", "AddressLimitExceeded", kNow - 7200 });
	in.orphans.push_back({ "eipalloc-0abc", "203.0.113.9", kNow - 100 });
	in.sold = hosted::Capacity{ 30, 2, 1 };
	in.sellable = hosted::Capacity{ 24, 2, 0 };
	const auto all = hosted::fleet_health(in, {});

	const auto launches = of(all, "launches_held");
	TEST_EXPECT(launches.size() == 1 && launches[0].subject == "Own launches" &&
			launches[0].link == "/admin/fleet" && launches[0].since == kNow - 30 &&
			launches[0].text ==
					"Held for another 10 min after 2 failed launches in a row; the last, machine "
					"9: the "
					"launch failed (VcpuLimitExceeded).");
	const auto lobby = of(all, "no_lobby");
	TEST_EXPECT(lobby.size() == 1 && lobby[0].level == HealthLevel::Danger &&
			lobby[0].link == "/admin/infrastructure/programs/lobby" &&
			lobby[0].since == kNow - 400);
	const auto failed = of(all, "failed_server");
	TEST_EXPECT(failed.size() == 1 && failed[0].subject == "Server 41 (Broken)" &&
			failed[0].link == "/admin/fleet/41/logs" &&
			failed[0].text == "Failed: its admin link stopped answering.");
	const auto address = of(all, "failed_address");
	TEST_EXPECT(address.size() == 1 && address[0].link == "/admin/addresses" &&
			address[0].subject == "Address smoke");
	const auto orphan = of(all, "orphan_address");
	TEST_EXPECT(orphan.size() == 1 && orphan[0].link == "/admin/addresses" &&
			orphan[0].subject == "Address 203.0.113.9");
	const auto oversold = of(all, "oversold");
	TEST_EXPECT(oversold.size() == 1 && oversold[0].link == "/admin/billing" &&
			oversold[0].text ==
					"Sold past what the fleet holds: shared places 30 of 24, static addresses 1 of "
					"0. "
					"No new subscription of it sells until it is back under.");
	// Nothing sold past the fleet, no finding; with hosting off nothing is judged.
	in.sold = hosted::Capacity{ 24, 2, 0 };
	TEST_EXPECT(of(hosted::fleet_health(in, {}), "oversold").empty());
	in.sold.reset();
	TEST_EXPECT(of(hosted::fleet_health(in, {}), "oversold").empty());
	return 0;
}

// A server whose data its machines cannot fetch, start after start (no restart is
// counted for it, so it never fails): three in a row, or a run of them past a
// quarter of an hour, is a warn on the server naming its data set, its trees and
// hostd's last reason; past an hour, a danger.
int test_data_unavailable() {
	hosted::HealthInputs in = healthy();
	in.servers.push_back(server(51, State::Provisioning, 30));
	hosted::DataFailures d;
	d.count = 2;
	d.since = kNow - hosted::kDataFailingS;
	d.reason = "its data could not be fetched: aws s3 sync exited 1: fatal error: An error "
			   "occurred (AccessDenied)";
	d.data_set = "Joint Operations";
	d.base_tree = 41;
	in.servers[0].data_failures = d;
	// Two, within the quarter hour: nothing yet.
	TEST_EXPECT(of(hosted::fleet_health(in, {}), "data_unavailable").empty());
	// The third.
	in.servers[0].data_failures->count = 3;
	auto found = of(hosted::fleet_health(in, {}), "data_unavailable");
	TEST_EXPECT(found.size() == 1 && found[0].level == HealthLevel::Warn &&
			found[0].subject == "Server 51 (Smoke 51)" && found[0].link == "/admin/fleet/51" &&
			found[0].since == kNow - hosted::kDataFailingS);
	TEST_EXPECT(found[0].text ==
			"Its data could not be fetched at its last 3 starts, the first 15 min ago (Joint "
			"Operations, tree 41); hostd's last reason: aws s3 sync exited 1: fatal error: An "
			"error occurred (AccessDenied).");
	// One, past the quarter hour (the start after it still under way), with an
	// expansion's tree.
	in.servers[0].data_failures->count = 1;
	in.servers[0].data_failures->since = kNow - hosted::kDataFailingS - 60;
	in.servers[0].data_failures->expansion_tree = 42;
	found = of(hosted::fleet_health(in, {}), "data_unavailable");
	TEST_EXPECT(found.size() == 1 && found[0].level == HealthLevel::Warn &&
			found[0].text.rfind("Its data could not be fetched at its last start, 16 min ago "
								"(Joint Operations, trees 41 and 42); hostd's last reason: ",
					0) == 0);
	// Past an hour: a danger; a data set gone and no reason recorded say so.
	in.servers[0].data_failures->since = kNow - hosted::kDataFailingDangerS - 1;
	in.servers[0].data_failures->data_set.clear();
	in.servers[0].data_failures->reason = "its data could not be fetched";
	found = of(hosted::fleet_health(in, {}), "data_unavailable");
	TEST_EXPECT(found.size() == 1 && found[0].level == HealthLevel::Danger &&
			found[0].text ==
					"Its data could not be fetched at its last start, 60 min ago (its data set is "
					"gone, trees 41 and 42).");
	TEST_EXPECT(hosted::health_gauges(in, hosted::fleet_health(in, {})).danger == 1);
	// None in a row: nothing.
	in.servers[0].data_failures.reset();
	TEST_EXPECT(of(hosted::fleet_health(in, {}), "data_unavailable").empty());
	return 0;
}

// The static addresses: a release whose last step is past kStuckReleaseS is
// stuck, one just at it not; addresses held while the add-on is off, none
// when it is on or none is held.
int test_addresses() {
	hosted::HealthInputs in = healthy();
	in.releasing_addresses.push_back({ 7, "slow", kNow - hosted::kStuckReleaseS - 1 });
	in.releasing_addresses.push_back({ 8, "draining", kNow - hosted::kStuckReleaseS });
	in.held_addresses = 3;
	in.addresses_on = true;
	auto all = hosted::fleet_health(in, {});
	const auto stuck = of(all, "stuck_address");
	TEST_EXPECT(stuck.size() == 1 && stuck[0].subject == "Address slow" && stuck[0].level == HealthLevel::Warn &&
			stuck[0].link == "/admin/addresses" && stuck[0].since == kNow - hosted::kStuckReleaseS - 1);
	TEST_EXPECT(of(all, "addresses_off").empty());
	// The add-on off with addresses held.
	in.addresses_on = false;
	all = hosted::fleet_health(in, {});
	auto off = of(all, "addresses_off");
	TEST_EXPECT(off.size() == 1 && off[0].subject == "Static addresses" && off[0].level == HealthLevel::Warn &&
			off[0].link == "/admin/addresses" && off[0].text.rfind("3 are held while the add-on is off", 0) == 0);
	in.held_addresses = 1;
	off = of(hosted::fleet_health(in, {}), "addresses_off");
	TEST_EXPECT(off.size() == 1 && off[0].text.rfind("1 is held while the add-on is off", 0) == 0);
	in.held_addresses = 0;
	TEST_EXPECT(of(hosted::fleet_health(in, {}), "addresses_off").empty());
	return 0;
}

int test_order_and_gauges() {
	hosted::HealthInputs in = healthy();
	in.lobbies_live = 0; // danger, since kNow - 50
	in.lobby_last_seen = kNow - 50;
	in.machines.push_back(machine(1, FleetMachineState::Lost, 3600));
	in.machines[0].row.lost_held_since_ms = (kNow - 500) * 1000; // danger, older
	in.machines.push_back(machine(2, FleetMachineState::Booting, 3600)); // warn, since kNow - 3600
	in.machines.push_back(machine(3, FleetMachineState::Ready, 60));
	in.machines[2].moves_last_hour = 9; // warn, no time
	in.failed.push_back({ 41, "Broken", "", kNow - 7200 }); // warn, the oldest
	hosted::ReleaseImages release;
	release.image_id = "ami-0new";
	in.machines[2].row.image_id = "ami-0old"; // info
	const auto all = hosted::fleet_health(in, release);
	std::vector<std::string> order;
	for (const Finding &f : all)
		order.push_back(f.source);
	TEST_EXPECT(order ==
			std::vector<std::string>({ "lost_held", "no_lobby", "failed_server", "stuck_machine",
					"flapping_machine", "outdated_image" }));
	TEST_EXPECT(std::string(hosted::health_level_text(all.front().level)) == "danger" &&
			std::string(hosted::health_level_text(all.back().level)) == "info");
	const hosted::HealthGauges g = hosted::health_gauges(in, all);
	TEST_EXPECT(g.danger == 2 && g.warn == 3 && g.machines_stuck == 2);
	// Machine 3 is the one ready machine reporting its use.
	TEST_EXPECT(g.disk_free_min_percent == 50);
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "healthy", test_healthy },
		{ "stalled_start", test_stalled_start },
		{ "stuck_machines", test_stuck_machines },
		{ "protocol_and_images", test_protocol_and_images },
		{ "pressure_and_ticks", test_pressure_and_ticks },
		{ "stale_reports", test_stale_reports },
		{ "launches_lobby_failures", test_launches_lobby_failures },
		{ "data_unavailable", test_data_unavailable },
		{ "addresses", test_addresses },
		{ "order_and_gauges", test_order_and_gauges },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the fleet's health\n");
	return 0;
}
