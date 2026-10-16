#pragma once

// A MachineDriver the fleet tests drive by hand, and the machines' agents they
// play. launch() records the launch and hands out a cloud machine that runs at
// once (a launch can be made to fail, a machine to vanish, the cloud to answer
// nothing, the listing to throw, each call to take call_ms); a launch of a
// machine id it launched already in the same subnet answers that machine, as
// EC2 answers a client token it has seen (one per machine id and subnet), and a
// run again (its attempt past the first) launches where its row recorded an
// earlier run's call (MachineLaunch::subnet), else where it lists that run's
// machine (not while `lagging`: EC2 lists a launch late), as the EC2 driver
// does; it records its subnet (MachineLaunch::record_subnet) before it
// launches, and one it cannot record launches nothing. The fleet jobs
// (fleet_jobs.h) call it from any worker thread: its state is under a lock. A
// FakeAgent per machine syncs the way the agent routes do
// (fleet_repository.h record_sync, then the supervisor's
// FleetDesiredState::sync), runs each desired container it got files for once
// its slot is free (running at once, or left starting), and drops the ones the
// answer leaves out; the test exits its containers. Nothing runs.

#include "hosted/agent_protocol.h"
#include "hosted/fleet_desired_state.h"
#include "hosted/fleet_repository.h"
#include "hosted/machine_driver.h"
#include "hosted/slots.h"

#include <net/novaworld/db/database.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hosted_test {

namespace hosted = opennova::novaworld_server::hosted;

class FakeDriver : public hosted::MachineDriver {
public:
	struct Machine {
		int64_t machine_id = 0;
		hosted::MachineKind kind = hosted::MachineKind::Shared;
		int slots = 0;
		std::string token;
		hosted::CloudState state = hosted::CloudState::Running;
		bool terminated = false;
		std::string subnet; // the zone it launched in
		std::string private_address;
		std::string public_address;
	};

	const char *name() const override { return "fake"; }

	// The supervisor's view of its machines (the rig's profile).
	hosted::MachineProfile profile() {
		hosted::MachineProfile p;
		p.provider = name();
		p.boot_timeout_ms = boot_timeout;
		p.slot_plan = [this](int slot) { return slot_plan(slot); };
		return p;
	}

	std::optional<hosted::LaunchedMachine> launch(const hosted::MachineLaunch &launch, std::string &error) override {
		pause();
		std::unique_lock<std::mutex> lock(mu);
		++launches;
		if (fail_next_launch) {
			fail_next_launch = false;
			error = "the fake refused the launch";
			return std::nullopt;
		}
		// Its subnet: a run again goes where its row recorded an earlier run's
		// call, else where it lists that run's machine (as the EC2 driver's
		// describe of the machine id finds it, unless EC2 lists it late), a first
		// run to `subnet`.
		std::string in = subnet;
		if (!launch.subnet.empty()) {
			in = launch.subnet;
		} else {
			for (const auto &entry : machines) {
				if (launch.attempt > 1 && !lagging &&
						entry.second.machine_id == launch.machine_id && !entry.second.terminated)
					in = entry.second.subnet;
			}
		}
		if (launch.record_subnet) {
			lock.unlock();
			const bool recorded = launch.record_subnet(in);
			lock.lock();
			if (!recorded) {
				error = "the fake's subnet could not be recorded";
				return std::nullopt;
			}
		}
		// The machine id's launch again in that subnet (EC2's client token, one
		// per subnet): the same machine. In another subnet, another machine.
		std::string id;
		for (const auto &entry : machines) {
			if (entry.second.machine_id == launch.machine_id && entry.second.subnet == in &&
					!entry.second.terminated)
				id = entry.first;
		}
		if (id.empty()) {
			const int n = next_++;
			id = "fake-" + std::to_string(n);
			Machine m;
			m.machine_id = launch.machine_id;
			m.kind = launch.kind;
			m.slots = launch.slots;
			m.token = launch.bootstrap_token;
			m.subnet = in;
			m.private_address = "10.0.1." + std::to_string(n);
			m.public_address = "198.51.100." + std::to_string(n);
			machines[id] = m;
			last = id;
		}
		const Machine &m = machines[id];
		hosted::LaunchedMachine out;
		out.cloud_id = id;
		out.private_address = m.private_address;
		out.public_address = m.public_address;
		out.instance_type = launch.kind == hosted::MachineKind::Own ? "c7a.medium" : "t3a.large";
		out.template_version = "lt-0fake:1";
		return out;
	}

	hosted::CloudMachine describe(const std::string &cloud_id, int64_t launched_ago_ms) override {
		pause();
		std::lock_guard<std::mutex> lock(mu);
		++describes;
		last_launched_ago_ms = launched_ago_ms;
		hosted::CloudMachine c;
		if (unknown) {
			c.state = hosted::CloudState::Unknown;
			return c;
		}
		const auto it = machines.find(cloud_id);
		if (it == machines.end() || it->second.terminated) return c; // Gone
		c.state = it->second.state;
		c.private_address = it->second.private_address;
		c.public_address = it->second.public_address;
		return c;
	}

	void terminate(const std::string &cloud_id) override {
		pause();
		std::lock_guard<std::mutex> lock(mu);
		terminations.push_back(cloud_id);
		const auto it = machines.find(cloud_id);
		if (it != machines.end()) it->second.terminated = true;
	}

	std::vector<hosted::TaggedCloudMachine> list_tagged() override {
		std::lock_guard<std::mutex> lock(mu);
		if (fail_list) throw std::runtime_error("the fake could not list its machines");
		std::vector<hosted::TaggedCloudMachine> out;
		for (const auto &entry : machines) {
			if (!entry.second.terminated) out.push_back({ entry.first, entry.second.machine_id });
		}
		return out;
	}

	hosted::SlotPlan slot_plan(int slot) {
		if (on_slot_plan) on_slot_plan();
		hosted::SlotPlan plan;
		plan.game_port = hosted::slot_game_port(slot);
		plan.admin_port = hosted::slot_admin_port(slot);
		plan.admin_allowed_ip = "10.0.0.1";
		return plan;
	}

	// The test's hands.
	void vanish(const std::string &id) {
		std::lock_guard<std::mutex> lock(mu);
		machines.erase(id);
	}
	// A cloud machine the driver runs that no live row may name (reconciliation).
	std::string adopt(int64_t machine_id) {
		std::lock_guard<std::mutex> lock(mu);
		const std::string id = "fake-" + std::to_string(next_++);
		Machine m;
		m.machine_id = machine_id;
		machines[id] = m;
		return id;
	}
	bool was_terminated(const std::string &id) const {
		std::lock_guard<std::mutex> lock(mu);
		for (const std::string &t : terminations) {
			if (t == id) return true;
		}
		return false;
	}
	// The cloud machines not terminated.
	size_t running() const {
		std::lock_guard<std::mutex> lock(mu);
		size_t n = 0;
		for (const auto &entry : machines)
			n += entry.second.terminated ? 0 : 1;
		return n;
	}

	mutable std::mutex mu;
	std::map<std::string, Machine> machines;
	std::vector<std::string> terminations;
	std::string last;
	std::atomic<int> launches{ 0 };
	std::atomic<int> describes{ 0 };
	// The launch's age the last describe was handed (its row's), under mu.
	int64_t last_launched_ago_ms = -2;
	bool fail_next_launch = false;
	// The subnet a first run launches in (a test moves it, as the zones' counts
	// move between a launch's runs), under mu.
	std::string subnet = "subnet-a";
	// While set, a run again whose row recorded no subnet finds no earlier run's
	// machine (EC2 lists a launch late), so it goes to `subnet`; under mu.
	bool lagging = false;
	bool fail_list = false; // list_tagged() throws while set
	bool unknown = false;   // describe() answers Unknown while set
	uint64_t boot_timeout = 60 * 1000;
	// Each launch, describe and terminate takes this long (a slow cloud).
	std::atomic<int> call_ms{ 0 };
	// Runs inside slot_plan(), while a pass places a server (a route acting then).
	std::function<void()> on_slot_plan;
	// The supervisor's thread, set before any worker starts: a launch, describe or
	// terminate that runs on it is counted (the pass must never call the cloud).
	std::thread::id supervisor_thread{};
	mutable std::atomic<int> calls_on_supervisor_thread{ 0 };

private:
	void pause() const {
		if (supervisor_thread != std::thread::id{} && std::this_thread::get_id() == supervisor_thread)
			calls_on_supervisor_thread.fetch_add(1);
		if (const int ms = call_ms.load(); ms > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	}

	int next_ = 1;
};

// One machine's agent, as the test plays it.
class FakeAgent {
public:
	struct Container {
		hosted::ContainerReport report;
		hosted::DesiredContainer desired; // as the answer that started it gave it, files and all
	};

	FakeAgent() = default;
	explicit FakeAgent(int64_t machine_id) : machine_id_(machine_id) {}

	// One sync: the report (record_sync: a booting machine becomes ready), the
	// answer, then the answer acted on. A container the answer leaves out goes;
	// one it wants that the machine lacks starts, given its files, once no other
	// container holds its slot. False when the machine no longer syncs.
	bool sync(opennova::db::Database &db, hosted::FleetDesiredState &desired) {
		std::vector<hosted::ContainerReport> reports;
		for (const auto &entry : containers) reports.push_back(entry.second.report);
		const auto row = hosted::record_sync(db, machine_id_, "fake-agent", reports);
		if (!row) return false;
		last = desired.sync(db, *row, reports);
		++syncs;
		std::set<std::string> wanted;
		for (const hosted::DesiredContainer &d : last.desired) wanted.insert(d.name);
		for (auto it = containers.begin(); it != containers.end();)
			it = wanted.count(it->first) != 0 ? std::next(it) : containers.erase(it);
		for (const hosted::DesiredContainer &d : last.desired) {
			if (containers.count(d.name) != 0) continue;
			if (!d.files) {
				refused.insert(d.name);
				continue;
			}
			bool held = false;
			for (const auto &entry : containers) held = held || entry.second.report.slot == d.slot;
			if (held) continue;
			Container c;
			c.report.name = d.name;
			c.report.slot = d.slot;
			c.report.state = start_running ? hosted::ContainerState::Running : hosted::ContainerState::Starting;
			if (start_running) c.report.started_at = 1760000000;
			c.desired = d;
			containers[d.name] = std::move(c);
		}
		return true;
	}

	// The test's hands on its containers.
	void run(const std::string &name) {
		auto &r = containers.at(name).report;
		r.state = hosted::ContainerState::Running;
		r.started_at = 1760000000;
	}
	void exit_with(const std::string &name, int64_t code, bool oom = false) {
		auto &r = containers.at(name).report;
		r.state = hosted::ContainerState::Exited;
		r.exit_code = code;
		r.oom = oom;
	}
	// docker's 125: the container never started.
	void fail_start(const std::string &name) {
		auto &r = containers.at(name).report;
		r.state = hosted::ContainerState::Exited;
		r.exit_code = 125;
		r.started_at.reset();
	}
	const Container *find(const std::string &name) const {
		const auto it = containers.find(name);
		return it == containers.end() ? nullptr : &it->second;
	}

	std::map<std::string, Container> containers;
	hosted::FleetDecision last;
	int syncs = 0;
	bool start_running = true;
	std::set<std::string> refused; // desired containers it could not start (no files)

private:
	int64_t machine_id_ = 0;
};

} // namespace hosted_test
