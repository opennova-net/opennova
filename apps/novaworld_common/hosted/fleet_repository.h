#pragma once

// The hosted servers' fleet in the database (backend/migrations/0003_fleet.sql):
// the machines (fleet_machines) and their lifecycle, every move a
// compare-and-set on the state so the supervisor and the agent routes never
// both win, and each move's event (fleet_machine_events, 0016: written in the
// same statement as the move, so no move goes unrecorded); what each machine's
// agent says of the machine (its image, its log stream, its use, the trees it
// holds); each machine's one-time bootstrap token and its agent's long-lived
// secret, kept as their SHA-256 alone (agent_secret_digest) and compared in
// constant time; the slots the live servers hold on the machines
// (hosted_servers' machine_ref, slot, game_port and generation), which
// placement reads (placement.h); and what each machine's agent last reported
// of its slots (fleet_slot_reports), which the stats read. The supervisor and
// the agent routes (agent_route.h) share it, and the fleet's jobs
// (fleet_jobs.h) write what the cloud answered here (0007_jobs.sql: the
// describes' cloud state, a refused launch) beside what the supervisor
// remembers of each machine between its passes. Crow-free. Each function
// throws db::Error on a database failure. Service policy, not a port.

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
	// What the last describe (or the sweep's) said of it (0007_jobs.sql):
	// CloudState's word, "" before any (and again from when it went lost).
	std::string cloud_state;
	std::string described_at;
	int64_t launched_for = 0; // the server whose placement launched it (0: none)
	// The subnet its launch last called the cloud in, written before the call
	// (record_launch_subnet); "" until one did.
	std::string launch_subnet;
	// The static address it was launched to carry (address_repository.h; an
	// own machine's): 0 for none.
	int64_t address_id = 0;
	// What the supervisor remembers of it between passes (supervisor.h, on its
	// clock): the state its pass last saw and since when, since when it has
	// held no server (0: it holds one), the server its drain moved last, a lost
	// machine that was draining, and when the pass last asked for a describe.
	std::string seen_state;
	int64_t seen_since_ms = 0;
	int64_t idle_since_ms = 0;
	int64_t drain_moving = 0;
	bool was_draining = false;
	int64_t describe_asked_ms = 0;
	// Its agent's envelope at its last sync, matched or not (0015_agent_envelope.sql,
	// agent_protocol.h AgentEnvelope): the protocol it speaks (0 before any), the
	// image its ECS task runs ("" off ECS), and since when its syncs speak another
	// protocol than the service's (UTC 'YYYY-MM-DD HH:MM:SS'; "" while they match).
	int agent_protocol = 0;
	std::string agent_image;
	std::string protocol_mismatch_since;
	// Since when the supervisor's mass-lost brake holds this lost machine from its
	// end (supervisor.h, on its clock; 0: not held).
	int64_t lost_held_since_ms = 0;
	// What its agent says of the machine (0016_fleet_observability.sql): the
	// machine's image and the agent's own log stream ("" for none), and its use
	// as the last report read it (none before one; stats_at its time).
	std::string image_id;
	std::string agent_log_stream;
	std::optional<MachineStats> stats;
	std::string stats_at;
};

// The words CloudState is stored as (fleet_machines.cloud_state).
const char *cloud_state_text(CloudState state);
// The word's state; nullopt for "" (none known) or another word.
std::optional<CloudState> parse_cloud_state(std::string_view text);

// One of a machine's events (fleet_machine_events): a move from `from_state`
// ("" for the row's making) to `to_state`, or, with the two the same, a note
// ("log stream <name>").
struct FleetMachineEvent {
	int64_t id = 0;
	int64_t machine_id = 0;
	std::string from_state;
	std::string to_state;
	std::string detail;
	std::string at; // UTC 'YYYY-MM-DD HH:MM:SS'
};
// Machine `machine_id`'s newest `limit` events, oldest first.
std::vector<FleetMachineEvent> list_machine_events(
		db::Database &db, int64_t machine_id, size_t limit);

// --- the machines ---

struct NewMachine {
	std::string provider; // the driver's name (MachineDriver::name)
	MachineKind kind = MachineKind::Shared;
	int slots = 1; // 1..kMaxSlots; an own machine's 1
	// The serve image its agent is copied out of; the bootstrap reply names it.
	std::string serve_image;
	int64_t reserved_for = 0; // own: the server it is launched for
	int64_t launched_for = 0; // the server whose placement launched it
	int64_t address_id = 0; // own: the static address it carries (0: none)
};

struct CreatedMachine {
	int64_t id = 0;
	// kMachineTokenChars letters and digits from the OS CSPRNG, handed to the
	// launch (MachineLaunch) and kept nowhere: the row holds its SHA-256.
	std::string bootstrap_token;
};

// A machine about to launch: its row (launching) with a one-time bootstrap
// token that lapses `token_lifetime_s` from now, and its first event.
CreatedMachine create_machine(db::Database &db, const NewMachine &machine,
		int64_t token_lifetime_s = kMachineTokenLifetimeS);
// The cloud's answer to the launch: launching -> booting, with the machine's
// cloud id, addresses, type and template, and launched_at now (the launch's
// answer, by the database's clock). False (nothing written) when the row is no
// longer launching.
bool mark_launched(db::Database &db, int64_t id, const LaunchedMachine &launched);
// The public IPv4 of the static address machine `id` carries (its row's
// address_id), "" for none or one not allocated.
std::string carried_address_ip(db::Database &db, int64_t id);
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
// refused from then on. The move's event carries `reason`.
bool transition_machine(db::Database &db, int64_t id, const std::vector<FleetMachineState> &from,
		FleetMachineState to, const std::string &reason);
bool transition_machine(db::Database &db, int64_t id, FleetMachineState from, FleetMachineState to,
		const std::string &reason);

std::optional<FleetMachineRow> find_machine(db::Database &db, int64_t id);
std::optional<FleetMachineRow> find_machine_by_cloud_id(
		db::Database &db, const std::string &provider, const std::string &cloud_id);
// Every live machine (machine_is_live), oldest first.
std::vector<FleetMachineRow> list_live_machines(db::Database &db);
// The live machines launched to carry static address `address_id`, oldest
// first (one, but while the last one ends).
std::vector<FleetMachineRow> live_machines_carrying(db::Database &db, int64_t address_id);
// The live machines, then the newest `ended_limit` ended and failed ones (the
// admin's fleet view).
std::vector<FleetMachineRow> list_machines(db::Database &db, size_t ended_limit);
// The ready and draining machines whose agent last reported more than
// `seconds` ago: the ones the supervisor marks lost (one whose agent never
// reported, drained while it booted, is not among them).
std::vector<FleetMachineRow> list_silent_machines(db::Database &db, int64_t seconds);
// Whether machine `id`'s agent reported within the last `seconds` (a lost
// machine's agent reporting again).
bool machine_reported_within(db::Database &db, int64_t id, int64_t seconds);
// A fleet roll (fleet_roll.h): each of `ids` that takes servers (launching,
// booting, ready) moves to draining with `reason`, so the supervisor moves
// their servers to new machines one machine's server at a time; the ids it
// moved, in order.
std::vector<int64_t> drain_machines(
		db::Database &db, const std::vector<int64_t> &ids, const std::string &reason);
// How many machines are in `state` (the heartbeat's machines_lost).
int count_machines(db::Database &db, FleetMachineState state);
// How many lost machines the mass-lost brake holds (the heartbeat's machines_held).
int count_held_machines(db::Database &db);
// Whether a machine launched for server `server_id` is launching or booting.
bool machine_launching_for(db::Database &db, int64_t server_id);
// How long an ended or failed machine's row stays after its end (the admin's
// fleet view lists the newest of them), as long as a finished job's.
inline constexpr int64_t kEndedMachineKeepS = 14 * 24 * 60 * 60;
// The ended and failed machines that ended more than `keep_s` ago deleted,
// with their slot reports, trees and events (their egress months, samples and
// hours stay, pruned by their own ages: stats_repository.h); a stopped or
// failed server that last ran on one keeps no machine (its machine_ref nulled,
// as the schema says), and one a live server still names stays. How many went.
size_t prune_ended_machines(db::Database &db, int64_t keep_s = kEndedMachineKeepS);

// --- what the cloud answered (the fleet's jobs, fleet_jobs.h) ---

// The subnet launching machine `id`'s launch is about to call the cloud in
// (launch_subnet), written before the call (MachineLaunch::record_subnet), so
// a run again of the launch calls there first, under the same client token.
// False (nothing written) when the row is no longer launching: the launch
// then calls nothing.
bool record_launch_subnet(db::Database &db, int64_t id, const std::string &subnet);

// How long ago machine `id`'s launch answered (its launched_at), in
// milliseconds by the database's clock: a describe's launch grace
// (MachineDriver::describe), judged alike by every worker; nullopt for no such
// machine.
std::optional<int64_t> launched_ago_ms(db::Database &db, int64_t id);

// A describe of live machine `id` whose cloud machine is `cloud_id`: its cloud
// state and when; a pending or running one's addresses as
// set_machine_addresses takes them (the public one kept when the cloud names
// none). False when the row is no longer that live machine.
bool record_cloud_state(
		db::Database &db, int64_t id, const std::string &cloud_id, const CloudMachine &cloud);
// A launch the cloud refused: launching -> failed with `reason` (fleet_jobs.h
// launch_failure_reason: the cloud's error code where it named one), the
// refusal left for the supervisor to count (launch_refused). False when the
// row was no longer launching.
bool fail_refused_launch(db::Database &db, int64_t id, const std::string &reason);
struct RefusedLaunch {
	int64_t machine_id = 0;
	MachineKind kind = MachineKind::Shared;
	int64_t launched_for = 0;
	std::string reason; // the machine's end_reason
};
// The refused launches not counted yet, counted now (taken once).
std::vector<RefusedLaunch> take_refused_launches(db::Database &db);

// --- what the supervisor remembers of a machine (on its clock) ---

// The row's seen_state, seen_since_ms, idle_since_ms, drain_moving and
// was_draining written from `m`.
void save_machine_runtime(db::Database &db, const FleetMachineRow &m);
// A ready or draining machine whose agent fell silent: lost with `reason`,
// seen lost from `now_ms`, whether it was draining kept, its cloud state
// cleared (only a describe after it counts), held by no brake. False when it
// was neither.
bool mark_machine_lost(db::Database &db, int64_t id, const std::string &reason, int64_t now_ms);
// The mass-lost brake holds lost machine `id` from its end since `now_ms`
// (lost_held_since_ms; kept when it holds already). False when it is not lost.
// Any move off lost (transition_machine) lets go of it.
bool hold_lost_machine(db::Database &db, int64_t id, int64_t now_ms);
// The pass asked for a describe of machine `id` at `now_ms`.
void note_describe_asked(db::Database &db, int64_t id, int64_t now_ms);

// --- each kind's launch backoff (fleet_launch_backoff, 0010_launch_backoff_failures.sql) ---

// A kind's backoff: its failed launches in a row (counted once per backoff),
// and no launch of it before not_before_ms (0: none held).
struct LaunchBackoff {
	int64_t failures = 0;
	int64_t not_before_ms = 0;
};
// The wait after a kind's `failures`-th failed launch in a row (from 1):
// `base_ms` doubled per failure past the first, at most `cap_ms`.
uint64_t launch_backoff_wait_ms(int64_t failures, uint64_t base_ms, uint64_t cap_ms);
LaunchBackoff launch_backoff(db::Database &db, MachineKind kind);
int64_t launch_not_before_ms(db::Database &db, MachineKind kind);
// A launch of `kind` failed at `now_ms`: its failures in a row counted, and no
// launch of it for launch_backoff_wait_ms of them; the backoff after. nullopt
// (nothing changed) while a backoff holds: a failure landing then came from a
// launch made before it began.
std::optional<LaunchBackoff> note_launch_failure(
		db::Database &db, MachineKind kind, int64_t now_ms, uint64_t base_ms, uint64_t cap_ms);
// A machine of `kind` booted: its failures back to 0, no backoff held.
void clear_launch_backoff(db::Database &db, MachineKind kind);

// --- the agent's credentials ---

// The machine's bootstrap (POST /hosted/machines/<id>/bootstrap): when
// `token` is machine `id`'s live one (its digest compared in constant time)
// and the machine is booting, `secret_digest` (the new agent secret's
// agent_secret_digest) is stored with `agent_version`, replacing any earlier
// redeem's, in one transaction, and the row returned. The token is spent by
// the agent's first sync (record_sync, record_unlisted_sync), not here, so an
// agent whose reply was lost redeems it again for a fresh secret (the route
// answers the machine's own private address alone). nullopt otherwise, with
// `why` for the log (never the token).
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
	ContainerWait waiting = ContainerWait::None;
	std::optional<int64_t> exit_code;
	bool oom = false;
	std::optional<int64_t> started_at; // Unix seconds
	std::string reason; // exited: why, when the agent knew (agent_protocol.h), else ""
	double cpu_percent = 0.0;
	uint64_t memory_bytes = 0;
	uint64_t egress_bytes = 0; // the slot's, since the machine booted
	uint64_t net_in_bytes = 0;
	std::optional<TickHealth> tick;
	std::string reported_at; // UTC 'YYYY-MM-DD HH:MM:SS'
};

// One sync's report, in one transaction: last_report_at and its envelope (its
// agent's version and image, this protocol, no mismatch), the bootstrap token
// spent, a booting machine ready (its agent reports: its kind's launch backoff
// cleared), and the machine's slot rows replaced by `containers` (one per
// slot: the running one where a slot holds several, else the last; a slot past
// the machine's is dropped). The row after, or nullopt when the machine is no
// longer one that syncs (booting, ready, draining, lost).
std::optional<FleetMachineRow> record_sync(db::Database &db, int64_t id,
		const std::string &agent_version, const std::vector<ContainerReport> &containers,
		const std::string &agent_image = {});
// A sync whose agent could not list its containers (agent_protocol.h
// listing_failed): last_report_at and its envelope as record_sync's, the
// bootstrap token spent, so the machine is heard from while its slot rows
// stand and a booting one stays booting. The row after, or nullopt as
// record_sync's.
std::optional<FleetMachineRow> record_unlisted_sync(db::Database &db, int64_t id,
		const std::string &agent_version, const std::string &agent_image = {});
// A sync in another protocol than the service's (agent_protocol.h, the
// envelope): last_report_at and `envelope` recorded, the bootstrap token spent,
// protocol_mismatch_since kept from the first such sync, the slot rows and the
// state left alone, so the machine is heard from (never lost) and acted on in
// nothing. The row after with `mismatch_s` the seconds its syncs have spoken
// another protocol, or nullopt as record_sync's.
std::optional<FleetMachineRow> record_held_sync(
		db::Database &db, int64_t id, const AgentEnvelope &envelope, int64_t &mismatch_s);
// What a sync's report says of the machine itself, in one transaction: its
// image and the agent's log stream (each kept when the report names none; a
// new stream an event of its own, in the same statement), its use with
// stats_at now when the report carries one, and its tree rows replaced by the
// report's. False when the machine is no longer one that syncs.
bool record_machine_report(db::Database &db, int64_t id, const SyncRequest &report);
// What machine `machine_id`'s agent last said of a tree it holds.
struct MachineTree {
	int64_t tree_id = 0;
	TreeReportState state = TreeReportState::Fetching;
	uint64_t bytes = 0;
	int tries = 0;
	std::string error;
	std::string reported_at;
};
// The machine's tree rows, by tree id.
std::vector<MachineTree> machine_trees(db::Database &db, int64_t machine_id);
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
// The live servers that hold no slot (requested, or provisioning before their
// placement), by id: the ones placement serves first.
std::vector<PlacementRequest> waiting_servers(db::Database &db);
// Whether the fleet has a place for a new server (on an own machine with
// `own_machine`) under `policy`: place_after over placement_machines and
// waiting_servers is anything but Full. The start request's at_capacity and GET
// /api/me/servers' `full` read it; inside a Transaction it reads that
// transaction's view.
bool fleet_has_room(db::Database &db, const PlacementPolicy &policy, bool own_machine);
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

} // namespace opennova::novaworld_server::hosted
