#pragma once

// A MachineProvider the hosted-server tests drive by hand: launch() records the
// LaunchSpec and hands out a machine that starts Pending (or Running, with
// `start_running`), and the test moves each machine through its states (run,
// exit with a code, vanish) and reads what the supervisor asked for (the specs,
// the terminations). A launch can be made to fail. Nothing runs.

#include "hosted/machine_provider.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hosted_test {

namespace hosted = opennova::novaworld_server::hosted;

class FakeProvider : public hosted::MachineProvider {
public:
	struct Machine {
		int64_t server_id = 0;
		hosted::MachineState state = hosted::MachineState::Pending;
		std::optional<int> exit_code;
		hosted::LaunchSpec spec;
		bool terminated = false;
		int describes = 0;
	};

	const char *name() const override { return "fake"; }

	hosted::MachinePlan plan(int64_t server_id) override {
		hosted::MachinePlan p;
		p.nw_port = static_cast<uint16_t>(40000 + server_id);
		p.admin_port = static_cast<uint16_t>(41000 + server_id);
		p.admin_allowed_ip = "10.0.0.1";
		return p;
	}

	std::optional<std::string> launch(const hosted::LaunchSpec &spec, std::string &error) override {
		++launches;
		if (fail_next_launch) {
			fail_next_launch = false;
			error = "the fake refused the launch";
			return std::nullopt;
		}
		const std::string id = "fake-" + std::to_string(next_++);
		Machine m;
		m.server_id = spec.server_id;
		m.state = start_running ? hosted::MachineState::Running : hosted::MachineState::Pending;
		m.spec = spec;
		machines[id] = m;
		last_machine = id;
		if (on_launch) on_launch();
		return id;
	}

	hosted::MachineStatus describe(const std::string &machine_id) override {
		hosted::MachineStatus s;
		const auto it = machines.find(machine_id);
		if (it == machines.end() || it->second.terminated) return s; // Gone
		s.state = it->second.state;
		s.exit_code = it->second.exit_code;
		// A cloud machine is given its address as it boots: the first
		// hide_address_describes describes of a machine name none.
		if (++it->second.describes > hide_address_describes) s.address = "10.0.0.2";
		return s;
	}

	void terminate(const std::string &machine_id) override {
		terminations.push_back(machine_id);
		const auto it = machines.find(machine_id);
		if (it != machines.end()) it->second.terminated = true;
	}

	std::vector<hosted::TaggedMachine> list_tagged() override {
		std::vector<hosted::TaggedMachine> out;
		for (const auto &entry : machines) {
			if (!entry.second.terminated) out.push_back({ entry.first, entry.second.server_id });
		}
		return out;
	}

	// The test's hands.
	void run(const std::string &id) { machines.at(id).state = hosted::MachineState::Running; }
	void exit_with(const std::string &id, int code) {
		machines.at(id).state = hosted::MachineState::Exited;
		machines.at(id).exit_code = code;
	}
	void vanish(const std::string &id) { machines.erase(id); }
	// A machine the provider runs that no row may name (reconciliation).
	std::string adopt(int64_t server_id) {
		const std::string id = "fake-" + std::to_string(next_++);
		Machine m;
		m.server_id = server_id;
		m.state = hosted::MachineState::Running;
		machines[id] = m;
		return id;
	}
	bool was_terminated(const std::string &id) const {
		for (const std::string &t : terminations) {
			if (t == id) return true;
		}
		return false;
	}
	const hosted::LaunchFile *file(const std::string &id, const std::string &name) const {
		const auto it = machines.find(id);
		if (it == machines.end()) return nullptr;
		for (const auto &f : it->second.spec.files) {
			if (f.name == name) return &f;
		}
		return nullptr;
	}

	std::map<std::string, Machine> machines;
	std::vector<std::string> terminations;
	std::string last_machine;
	int launches = 0;
	bool start_running = false;
	bool fail_next_launch = false;
	int hide_address_describes = 0;
	// Runs inside launch(), once the machine exists (a route acting mid-launch).
	std::function<void()> on_launch;

private:
	uint64_t next_ = 1;
};

} // namespace hosted_test
