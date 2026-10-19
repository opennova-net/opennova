#pragma once

// Where a hosted server's start stands, in its owner's words (the site's
// server cards and page, GET /api/me/servers; the admin's server view), and
// why its last run ended: pure functions over the rows (hosted_servers, its
// newest hosted_server_starts row, its machine and the launch backoff of its
// kind, its slot's report and its machine's trees), so every rule is a test.
// The owner sees the phase and the server's own last line, never a raw log, a
// machine's figures or the agent's reason a start never ran (its data could not
// be fetched: the fetch's error, an admin's alone). Service policy, not a port.
//
//   queued              requested: the supervisor has not picked it up
//   restarting          provisioning after a crash or a start that timed out
//                       (which, its cause), restart n of the limit
//   moving              provisioning after its machine's cause (a drain, a lost
//                       machine, a new lobby) or the owner's update: no restart
//   data_unavailable    provisioning after a start whose data its machine could
//                       not fetch: no restart, placed again shortly
//   launches_held       provisioning while its kind's launches are held (a
//                       refused launch's backoff)
//   launching_machine   a machine is launching or booting for it, or it waits
//                       for the one it was placed on to boot
//   waiting_for_slot    provisioning with no place in the fleet yet
//   waiting_for_address booting on a machine that does not show its static
//                       address yet: the supervisor holds its container
//                       until it does (fleet_repository.h awaits_address)
//   downloading_data    booting while its machine fetches its trees: the bytes
//                       so far of their manifests' total
//   downloading_server  booting while its machine pulls its image
//   starting_server     booting: its container starting
//   signing_in          listing: opennova-serve runs, its listing not yet up
//   running, stopping, stopped, failed   the states themselves

#include "fleet_repository.h"
#include "hosted_repository.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

enum class Phase : uint8_t {
	Queued,
	Restarting,
	Moving,
	DataUnavailable,
	LaunchesHeld,
	LaunchingMachine,
	WaitingForSlot,
	WaitingForAddress,
	DownloadingData,
	DownloadingServer,
	StartingServer,
	SigningIn,
	Running,
	Stopping,
	Stopped,
	Failed,
};
const char *phase_text(Phase phase); // "queued", ... "downloading_data", ... "failed"

// What the phase is read from.
struct PhaseInputs {
	State state = State::Requested;
	int restarts = 0;
	int restart_limit = kRestartLimit;
	std::optional<ServerStart> last_start; // its newest
	// The machine its slot is on (unset: it holds none), whether that machine
	// does not show its static address yet (awaits_address), whether a machine
	// is launching or booting for it, and whether its kind's launches are held.
	std::optional<FleetMachineState> machine_state;
	bool address_wait = false;
	bool machine_launching = false;
	bool launches_held = false;
	// Its own container's report on its slot, its trees as its machine's agent
	// holds them, and their manifests' bytes all told.
	std::optional<SlotReport> report;
	std::vector<MachineTree> trees;
	uint64_t data_bytes_total = 0;
	// An admin's view: the agent's reason a start never ran besides (line).
	bool admin = false;
};

struct ServerPhase {
	Phase phase = Phase::Queued;
	// downloading_data: the bytes fetched so far of its trees' total.
	uint64_t bytes_done = 0;
	uint64_t bytes_total = 0;
	// restarting: restart `restart` of `restart_limit`, after what (Crash or
	// Timeout).
	int restart = 0;
	int restart_limit = 0;
	StartCause restart_cause = StartCause::Crash;
	// data_unavailable, for an admin: the agent's reason (the fetch's error).
	std::string line;
};

ServerPhase server_phase(const PhaseInputs &in);

// Why a stopped or failed server's last run ended: its cause, how its container
// ended (the code, the OOM flag) and the line the server wrote then ("" for
// none), or, for an admin (`admin`), the agent's reason it never started (its
// data could not be fetched), which its owner never reads. Unset for a live
// server and one with no run.
struct StoppedBecause {
	StartCause cause = StartCause::Stop;
	std::optional<int64_t> exit_code;
	bool oom = false;
	std::string line;
};
std::optional<StoppedBecause> stopped_because(
		State state, const std::optional<ServerStart> &last_start, bool admin);

} // namespace opennova::novaworld_server::hosted
