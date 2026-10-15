#pragma once

// The hosted servers' fleet in the database (backend/migrations/0003_fleet.sql):
// the machines (fleet_machines) and their lifecycle, every move a
// compare-and-set on the state so the supervisor and the agent routes never
// both win; each machine's one-time bootstrap token and its agent's long-lived
// secret, kept as their SHA-256 alone (agent_secret_digest) and compared in
// constant time; the slots the live servers hold on the machines
// (hosted_servers' machine_ref, slot, game_port and generation), which
// placement reads (placement.h); and what each machine's agent last reported
// of its slots (fleet_slot_reports), which the stats read. The supervisor and
// the agent routes (agent_route.h) share it. Crow-free. Each function throws
// db::Error on a database failure. Service policy, not a port.

#include "agent_protocol.h"
#include "hosted_repository.h"
#include "machine_driver.h"
#include "placement.h"

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// How long a machine's bootstrap token lives after its row is made: past the
// slowest boot (the cloud's pending, the docker install, the data volumes, the
// agent copied out of the serve image), so a machine that never bootstraps is
// one the supervisor's boot timeout ends, not one whose token lapsed first.
inline constexpr int64_t kMachineTokenLifetimeS = 30 * 60;

// Whether a machine in `state` still exists for the fleet: any state but
// ended and failed.
bool machine_is_live(FleetMachineState state);

// A fleet_machines row. Neither credential's digest is part of it, so nothing
// that serializes one can emit either.
struct FleetMachineRow {
	int64_t id = 0;
	std::string provider;
	MachineKind kind = MachineKind::Shared;
	FleetMachineState state = FleetMachineState::Launching;
	std::string cloud_id;
	std::string instance_type;
	int slots = 0;
	std::string private_address;
	std::string public_address;
	bool bootstrapped = false; // its agent redeemed its token and holds a secret
	std::string agent_version;
	std::string template_version;
	std::string serve_image;
	int64_t reserved_for = 0; // an own machine's server id (0: none)
	// UTC 'YYYY-MM-DD HH:MM:SS'; empty until set.
	std::string last_report_at;
	std::string launched_at;
	std::string ready_at;
	std::string ended_at;
	std::string end_reason;
};

// --- the machines ---

struct NewMachine {
	std::string provider; // the driver's name (MachineDriver::name)
	MachineKind kind = MachineKind::Shared;
	int slots = 1; // 1..kMaxSlots; an own machine's 1
	// The serve image its agent is copied out of; the bootstrap reply names it.
	std::string serve_image;
	int64_t reserved_for = 0; // own: the server it is launched for
};

struct CreatedMachine {
	int64_t id = 0;
	// kMachineTokenChars letters and digits from the OS CSPRNG, handed to the
	// launch (MachineLaunch) and kept nowhere: the row holds its SHA-256.
	std::string bootstrap_token;
};

// A machine about to launch: its row (launching) with a one-time bootstrap
// token that lapses `token_lifetime_s` from now.
CreatedMachine create_machine(db::Database &db, const NewMachine &machine,
		int64_t token_lifetime_s = kMachineTokenLifetimeS);
// The cloud's answer to the launch: launching -> booting, with the machine's
// cloud id, addresses, type and template. False (nothing written) when the
// row is no longer launching.
bool mark_launched(db::Database &db, int64_t id, const LaunchedMachine &launched);
// What a describe read of a live machine's addresses: the private one only
// fills an empty one (it is what the agent routes admit the machine from, so
// nothing moves it once set), the public one follows the cloud. False when
// nothing changed.
bool set_machine_addresses(db::Database &db, int64_t id, const std::string &private_address,
		const std::string &public_address);
// The machine's move from `from` (any of them) to `to` when the row is in one
// of them; false (nothing written) otherwise. Ready sets ready_at (once) and
// clears end_reason; draining, ending, ended, failed and lost take `reason`
// as end_reason (an empty one keeps the last). Ending, ended and failed spend
// the bootstrap token and the agent's secret: an ending machine's agent is
// refused from then on.
bool transition_machine(db::Database &db, int64_t id, const std::vector<FleetMachineState> &from,
		FleetMachineState to, const std::string &reason);
bool transition_machine(db::Database &db, int64_t id, FleetMachineState from, FleetMachineState to,
		const std::string &reason);

std::optional<FleetMachineRow> find_machine(db::Database &db, int64_t id);
std::optional<FleetMachineRow> find_machine_by_cloud_id(
		db::Database &db, const std::string &provider, const std::string &cloud_id);
// Every live machine (machine_is_live), oldest first.
std::vector<FleetMachineRow> list_live_machines(db::Database &db);
// The live machines, then the newest `ended_limit` ended and failed ones (the
// admin's fleet view).
std::vector<FleetMachineRow> list_machines(db::Database &db, size_t ended_limit);
// The ready and draining machines whose agent last reported more than
// `seconds` ago: the ones the supervisor marks lost.
std::vector<FleetMachineRow> list_silent_machines(db::Database &db, int64_t seconds);

// --- the agent's credentials ---

// The machine's bootstrap (POST /hosted/machines/<id>/bootstrap): when
// `token` is machine `id`'s live one (its digest compared in constant time)
// and the machine is booting, the token is spent and `secret_digest` (the new
// agent secret's agent_secret_digest) stored with `agent_version`, in one
// transaction, and the row returned. nullopt otherwise, with `why` for the log
// (never the token).
std::optional<FleetMachineRow> redeem_bootstrap_token(db::Database &db, int64_t id,
		std::string_view token, const std::string &secret_digest, const std::string &agent_version,
		std::string &why);
// A sync's bearer: machine `id`'s row when `secret` is its agent's (the digest
// compared in constant time) and the machine is booting, ready, draining or
// lost; nullopt otherwise, with `why` for the log (never the secret).
std::optional<FleetMachineRow> authenticate_agent(
		db::Database &db, int64_t id, std::string_view secret, std::string &why);

// --- the agent's reports ---

// What a machine's agent last reported of one slot.
struct SlotReport {
	int slot = 0;
	std::string container;
	ContainerState state = ContainerState::Starting;
	std::optional<int64_t> exit_code;
	bool oom = false;
	std::optional<int64_t> started_at; // Unix seconds
	double cpu_percent = 0.0;
	uint64_t memory_bytes = 0;
	uint64_t egress_bytes = 0; // the slot's, since the machine booted
	std::string reported_at; // UTC 'YYYY-MM-DD HH:MM:SS'
};

// One sync's report, in one transaction: last_report_at and agent_version, a
// booting machine ready (its agent reports), and the machine's slot rows
// replaced by `containers` (one per slot: the running one where a slot holds
// several, else the last; a slot past the machine's is dropped). The row
// after, or nullopt when the machine is no longer one that syncs (booting,
// ready, draining, lost).
std::optional<FleetMachineRow> record_sync(db::Database &db, int64_t id,
		const std::string &agent_version, const std::vector<ContainerReport> &containers);
// The machine's slot rows, by slot.
std::vector<SlotReport> slot_reports(db::Database &db, int64_t machine_id);
// The slot row of the slot server `server_id` holds, when its machine's agent
// reported one there (the stats' per-server read); nullopt otherwise.
std::optional<SlotReport> slot_report_of(db::Database &db, int64_t server_id);

// --- the slots ---

// A live server holding a slot of a machine.
struct SlotHolder {
	int64_t server_id = 0;
	int slot = 0;
	int game_port = 0;
	int64_t generation = 0;
	State state = State::Requested;
};
// The live servers on machine `machine_id`, by slot.
std::vector<SlotHolder> slot_holders(db::Database &db, int64_t machine_id);
// Every live machine as placement reads it (placement.h): its row and the
// slots its live servers hold, from one snapshot.
std::vector<FleetMachine> placement_machines(db::Database &db);
// Live server `server_id`, holding no slot, takes slot `slot` of machine
// `machine_id` (one taking servers, the slot one of its own) at `game_port`:
// its generation moves on (+1) and is returned. nullopt (nothing written) when
// the server or the machine does not qualify or another live server holds the
// slot.
std::optional<int64_t> assign_slot(
		db::Database &db, int64_t server_id, int64_t machine_id, int slot, int game_port);
// The server's slot let go (a restart elsewhere); its generation stays. False
// when it held none.
bool release_slot(db::Database &db, int64_t server_id);
// A restart on the slot it holds: live server `server_id`'s generation moves
// on (+1, a new container name) and is returned; nullopt when it is not live
// or holds no slot.
std::optional<int64_t> next_generation(db::Database &db, int64_t server_id);

} // namespace opennova::novaworld_server::hosted
