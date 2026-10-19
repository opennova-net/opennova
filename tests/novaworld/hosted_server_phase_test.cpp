// Where a hosted server's start stands in its owner's words, and why its last run ended
// (apps/novaworld_common/hosted/model/server_phase.h), over hand-made rows: each state's
// phase; a provisioning server's wait by why its last run ended (a crash or a timeout:
// restarting n of the limit; its machine's cause, the owner's update or a move off an own
// machine: moving; its data
// not fetched: data_unavailable, the agent's reason an admin's alone), else by
// the fleet (launches held, a machine launching for it, a slot to wait for); a booting
// server's by its machine (still booting, or not showing its static address yet:
// fleet_repository.h awaits_address) and its container's report (its trees with the bytes
// so far of their total, its image, its start); and the last run's end for a stopped or
// failed server alone (the agent's reason a start never ran an admin's line alone). Pure
// functions: no database.

#include "hosted/model/server_phase.h"

#include "common/test_expect.h"

#include <cstdio>
#include <string>

namespace hosted = opennova::novaworld_server::hosted;
using hosted::Phase;
using hosted::State;

namespace {

hosted::ServerStart ended_by(hosted::StartCause cause) {
	hosted::ServerStart s;
	s.generation = 2;
	s.ended_at = "2026-10-18 14:05:03";
	s.cause = cause;
	return s;
}

int test_states() {
	hosted::PhaseInputs in;
	const auto phase_of = [&in](State state) {
		in.state = state;
		return hosted::server_phase(in).phase;
	};
	TEST_EXPECT(phase_of(State::Requested) == Phase::Queued);
	TEST_EXPECT(phase_of(State::Listing) == Phase::SigningIn);
	TEST_EXPECT(phase_of(State::Running) == Phase::Running);
	TEST_EXPECT(phase_of(State::Stopping) == Phase::Stopping);
	TEST_EXPECT(phase_of(State::Stopped) == Phase::Stopped);
	TEST_EXPECT(phase_of(State::Failed) == Phase::Failed);
	for (const Phase p : { Phase::Queued, Phase::Restarting, Phase::Moving,
				 Phase::DataUnavailable, Phase::LaunchesHeld,
				 Phase::LaunchingMachine, Phase::WaitingForSlot, Phase::WaitingForAddress,
				 Phase::DownloadingData, Phase::DownloadingServer, Phase::StartingServer,
				 Phase::SigningIn, Phase::Running, Phase::Stopping, Phase::Stopped, Phase::Failed })
		TEST_EXPECT(hosted::phase_text(p)[0] != '\0');
	TEST_EXPECT(std::string(hosted::phase_text(Phase::DownloadingData)) == "downloading_data" &&
			std::string(hosted::phase_text(Phase::LaunchesHeld)) == "launches_held" &&
			std::string(hosted::phase_text(Phase::WaitingForAddress)) == "waiting_for_address" &&
			std::string(hosted::phase_text(Phase::DataUnavailable)) == "data_unavailable");
	return 0;
}

int test_provisioning() {
	hosted::PhaseInputs in;
	in.state = State::Provisioning;
	// No run yet: where the fleet stands for it.
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::WaitingForSlot);
	in.machine_launching = true;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::LaunchingMachine);
	in.launches_held = true;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::LaunchesHeld);
	// After a crash, or a start that timed out: restarting, n of the limit, after
	// which (never "a crash" for a timeout).
	in.last_start = ended_by(hosted::StartCause::Crash);
	in.restarts = 2;
	auto p = hosted::server_phase(in);
	TEST_EXPECT(p.phase == Phase::Restarting && p.restart == 2 &&
			p.restart_limit == hosted::kRestartLimit &&
			p.restart_cause == hosted::StartCause::Crash);
	in.last_start = ended_by(hosted::StartCause::Timeout);
	p = hosted::server_phase(in);
	TEST_EXPECT(p.phase == Phase::Restarting && p.restart_cause == hosted::StartCause::Timeout);
	// Its restarts forgiven: no restart to name, the fleet's wait again.
	in.restarts = 0;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::LaunchesHeld);
	// Its machine's cause, the owner's update or a move off an own machine its
	// owner no longer has (its owner's limits): moving, no restart.
	for (const hosted::StartCause cause :
			{ hosted::StartCause::Fleet, hosted::StartCause::Update, hosted::StartCause::Limits }) {
		in.last_start = ended_by(cause);
		TEST_EXPECT(hosted::server_phase(in).phase == Phase::Moving);
	}
	// Its data not fetched: no restart, whatever its count; the agent's reason
	// (the fetch's error) an admin's alone.
	in.last_start = ended_by(hosted::StartCause::DataUnavailable);
	in.last_start->reason = "its data could not be fetched: aws s3 sync exited 1: AccessDenied";
	in.restarts = 1;
	p = hosted::server_phase(in);
	TEST_EXPECT(p.phase == Phase::DataUnavailable && p.restart == 0 && p.line.empty());
	in.admin = true;
	TEST_EXPECT(hosted::server_phase(in).line == in.last_start->reason);
	in.admin = false;
	in.restarts = 0;
	// A run still open (placed, not yet rendered) says nothing of the wait.
	in.last_start->ended_at.clear();
	in.launches_held = false;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::LaunchingMachine);
	return 0;
}

int test_booting() {
	hosted::PhaseInputs in;
	in.state = State::Booting;
	// Placed on a machine that still boots, or none known: the machine's wait.
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::LaunchingMachine);
	in.machine_state = hosted::FleetMachineState::Booting;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::LaunchingMachine);
	in.machine_state = hosted::FleetMachineState::Ready;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::StartingServer);
	// Its machine does not show its static address yet: the supervisor holds its
	// container back, so it waits for the address; still booting, the machine's wait.
	in.address_wait = true;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::WaitingForAddress);
	in.machine_state = hosted::FleetMachineState::Booting;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::LaunchingMachine);
	in.machine_state = hosted::FleetMachineState::Ready;
	// A container already reported is no longer held: its report says where it stands.
	hosted::SlotReport started;
	started.state = hosted::ContainerState::Starting;
	in.report = started;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::StartingServer);
	in.address_wait = false;
	// Its container waits for its trees: the bytes so far of their manifests' total.
	hosted::SlotReport report;
	report.state = hosted::ContainerState::Starting;
	report.waiting = hosted::ContainerWait::Trees;
	in.report = report;
	hosted::MachineTree base;
	base.tree_id = 7;
	base.state = hosted::TreeReportState::Ready;
	base.bytes = 1000;
	hosted::MachineTree expansion;
	expansion.tree_id = 8;
	expansion.bytes = 250;
	in.trees = { base, expansion };
	in.data_bytes_total = 2300;
	auto p = hosted::server_phase(in);
	TEST_EXPECT(p.phase == Phase::DownloadingData && p.bytes_done == 1250 && p.bytes_total == 2300);
	// A total the manifests understate never reads below what is there.
	in.data_bytes_total = 0;
	p = hosted::server_phase(in);
	TEST_EXPECT(p.bytes_done == 1250 && p.bytes_total == 1250);
	report.waiting = hosted::ContainerWait::Image;
	in.report = report;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::DownloadingServer);
	report.waiting = hosted::ContainerWait::None;
	in.report = report;
	TEST_EXPECT(hosted::server_phase(in).phase == Phase::StartingServer);
	return 0;
}

int test_awaits_address() {
	// The supervisor's hold, fleet_health's wait and the phase read one rule: a machine
	// that carries an address waits until its public address is the address's.
	hosted::FleetMachineRow m;
	m.public_address = "198.51.100.7";
	TEST_EXPECT(!hosted::awaits_address(m, ""));
	TEST_EXPECT(!hosted::awaits_address(m, "203.0.113.20"));
	m.address_id = 4;
	TEST_EXPECT(hosted::awaits_address(m, "")); // not allocated yet
	TEST_EXPECT(hosted::awaits_address(m, "203.0.113.20"));
	m.public_address.clear();
	TEST_EXPECT(hosted::awaits_address(m, "203.0.113.20"));
	m.public_address = "203.0.113.20";
	TEST_EXPECT(!hosted::awaits_address(m, "203.0.113.20"));
	return 0;
}

int test_stopped_because() {
	hosted::ServerStart run = ended_by(hosted::StartCause::Crash);
	run.exit_code = 3;
	run.reason = "the session ended: the NovaWorld service stopped the hosting";
	auto why = hosted::stopped_because(State::Failed, run, false);
	TEST_EXPECT(why && why->cause == hosted::StartCause::Crash && why->exit_code == 3 &&
			!why->oom && why->line == run.reason);
	// A start whose data could not be fetched: the agent's reason is no line of
	// the server's, so its owner reads none; an admin reads it.
	run.cause = hosted::StartCause::DataUnavailable;
	run.exit_code = 125;
	run.reason = "its data could not be fetched: aws s3 sync exited 1: AccessDenied";
	why = hosted::stopped_because(State::Stopped, run, false);
	TEST_EXPECT(why && why->cause == hosted::StartCause::DataUnavailable &&
			why->exit_code == 125 && why->line.empty());
	why = hosted::stopped_because(State::Stopped, run, true);
	TEST_EXPECT(why && why->line == run.reason);
	run.cause = hosted::StartCause::LinkLost;
	run.exit_code.reset();
	run.reason.clear();
	why = hosted::stopped_because(State::Stopped, run, false);
	TEST_EXPECT(why && why->cause == hosted::StartCause::LinkLost && !why->exit_code &&
			why->line.empty());
	// A live server, a server with no run, a run not ended: nothing to say.
	TEST_EXPECT(!hosted::stopped_because(State::Running, run, false));
	TEST_EXPECT(!hosted::stopped_because(State::Stopped, std::nullopt, false));
	run.ended_at.clear();
	TEST_EXPECT(!hosted::stopped_because(State::Stopped, run, false));
	return 0;
}

} // namespace

int main() {
	if (test_states() != 0 || test_provisioning() != 0 || test_booting() != 0 ||
			test_awaits_address() != 0 || test_stopped_because() != 0)
		return 1;
	std::printf("OK: a hosted server's phase\n");
	return 0;
}
