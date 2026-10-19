#pragma once

// The hosted servers' supervisor (ADR 0053, proposed): a thread of the
// NovaWorld service that walks every live hosted server through its lifecycle
// on a slot of one of the fleet's machines, logging every transition in
// hosted_server_events, and keeps the machines (fleet_machines) in step: it
// places servers (placement.h), launches and ends machines through the fleet's
// jobs (fleet_jobs.h), and answers each machine agent's sync with the
// containers its machine should run (FleetDesiredState, agent_route.h). A
// machine is never a host (CONTEXT.md "Machine").
//
// The pass never calls the cloud: it reads and writes rows alone, so it is
// fast and bounded, and the stall watchdog (core/health.h) watches it. A launch is
// the machine's row and its machine.launch job, written together; a boot or
// lost check enqueues a deduped machine.describe (at most every
// describe_interval_ms) and reads its answer from the row (cloud_state); an end
// enqueues machine.terminate; the sweep of the driver's machines is the
// periodic fleet.sweep job. What it knows of the driver without asking (its
// name, its boot timeout, a slot's ports) is its MachineProfile. What a pass
// remembers of a machine between passes (since when it is in its state, idle,
// the server its drain moved, a lost one that was draining) and each kind's
// launch backoff are rows too (fleet_repository.h), on the supervisor's clock
// (Unix milliseconds, clock_ms), so a new supervisor resumes them.
//
//   requested     the owner asked (POST /api/me/servers): the account's hosting
//                 entitlement (its live servers under it), the cap and the
//                 fleet's room were checked then
//   provisioning  placed (placement.h): on the fullest shared machine with a
//                 free slot, or the own machine reserved for it; with none, a
//                 machine of that kind is launched and the server placed on it
//                 at once (a booting machine takes servers: it waits for the
//                 boot in booting, no restart counted), and at the fleet's
//                 maximum it waits here for a slot. Once its slot is
//                 assigned (fleet_repository.h assign_slot: a new generation),
//                 the service account srv-<id> gets a new random password, a new
//                 admin password is minted and the files are rendered from
//                 scratch for the slot's ports (hosted_settings.h), and the
//                 trees it runs on are pinned (data_set_repository.h, ADR 0064:
//                 its data sets' current trees at its first start, kept through
//                 a crash restart or a move). A server on a shared machine
//                 past its owner's shared servers, counted in id order among
//                 the owner's live servers on shared machines not stopping, is
//                 stopped instead; one on a dedicated machine past its owner's
//                 dedicated machines, counted so, goes to a shared machine when
//                 its owner has a shared server free and the fleet room for it
//                 (else it is stopped); and one whose data set row is gone or has no data
//                 (no ready current tree, and no pins to keep) fails, which no
//                 restart mends
//   booting       its container (srv-<id>-g<generation>) is wanted on its slot;
//                 the machine's agent starts it once the slot is free (its
//                 boot_timeout_ms counts from the machine's ready)
//   listing       its agent reports it running; until the server's listing
//                 appears (a listed RID with one of the service account's
//                 HostKeys claimed)
//   running       every health interval the listing is checked and (with admin
//                 links) the server's admin link answering, every pass its
//                 agent's report; health_misses checks in a row with no answer
//                 on its admin link fail it, and its listing gone for
//                 listing_lost_ms places it again (below).
//                 In booting, listing and running alike, the owner's update
//                 (update_requested: POST /api/me/servers/<id>/update, or the
//                 admin's update-all) lets go of the slot and places the server
//                 again at once, no restart counted, and the next start pins it
//                 to its data sets' current trees (a new generation)
//   stopping      asked for by the owner, an admin or the supervisor itself (an
//                 owner's account deleted or no longer active, read every pass
//                 and again right before a start mints credentials, or a server
//                 past its owner's entitlement, below): the ServerStopHosting
//                 push to the listing, then the container no longer wanted (its
//                 agent stops it: opennova-serve deregisters on SIGTERM) once
//                 the listing has left the browser, or stop_timeout_ms after the
//                 push; its container exiting in this state ends the stop too,
//                 never a restart
//   stopped       a stop's end
//   failed        the restarts used up, or a fault no restart mends
//
// The owners' limits: every pass opens with what an owner's hosting
// entitlement no longer covers (enforce_limits), each of its two pools apart:
// it stops the owner's newest live servers on shared machines past its shared
// servers, then moves its newest on dedicated machines past its dedicated
// machines (paid ones and an admin's grant alike) to shared machines, each
// judged as a shared server's start is (move_to_shared: its owner must have a
// shared server free, the stops just asked still counted, and the fleet room
// for it; its match ends, its dedicated machine ends after it, and it starts
// again on a shared machine, no restart counted), stopping one its owner has no
// shared server for or the fleet no room for. A shared server never moves onto
// a free dedicated machine. A stopping one counts toward neither, and each has
// an event its owner reads ("its owner's support tier allows 1 hosted server on
// shared machines", "moved to a shared machine: its owner has 1 dedicated
// machine now", "its owner has 1 dedicated machine now and no shared server
// free"). An entitlement is read from the
// billing cache whether or not billing is configured, and a past_due tier keeps
// its servers. A pass that would stop or move more owners' servers than max(2,
// half the owners with live servers) acts on none: it logs an ERROR and holds
// (limits_hold.h, a row), so a cache gone wrong never mass-stops payers. While
// held no limit stops or moves a server. The hold releases once a full
// reconcile with Stripe has completed clean after it and the same owners (or
// some of them) are still past their entitlements, whose stops and moves then
// apply; an admin's override (POST /api/admin/billing/reconcile
// {"force_limits": true}) applies them at the next pass.
//
// Restarts. A stop is the supervisor's own act, never read from an exit code.
// What the server itself causes is a crash and counts: while the server should
// run, its container exiting with ANY code (0 for its rotation's end or the
// admin's GOTO MENUSTATE, 3 when the service ends its hosting, 1 for a SIGTERM
// while it waits to be listed), the kernel ending it out of memory, a start
// that fails (docker's 125, an image that did not pull among them), its
// container or its listing not coming in time (the listing's clock waits while
// its machine is lost). A crash lets go of the slot and goes back to
// provisioning after restart_backoff_ms while the server has restarts left:
// restart_limit of them (3), and an hour (restart_reset_ms) of continuous
// running resets the count. What the machine causes is no crash and costs
// none: a machine launched for it that the cloud refused, one it was placed on
// that never booted (gone, or past the driver's boot timeout), its machine
// lost, gone, ended (by an admin, or its agent held on another protocol for
// 10 minutes), or drained (an admin's drain, a fleet roll). A running server's
// listing gone for listing_lost_ms costs none either: its lobby's replacement took it (a start
// that then never lists still counts, at listing_timeout_ms). The server lets go of
// its slot and is placed again at once, or, its machine's launch refused,
// waits for the next one;
// the brake on a fleet that cannot launch is the per-kind launch backoff and
// the heartbeat's launch_failures (refused launches and machines that never
// booted alike), not the servers' restarts. Nor is NovaWorld's own cause a
// crash: while no lobby instance is live (lobbydb::live_lobbies, the gates'
// read: a lobby outage, a deployment's first release before its lobby runs)
// the gate hands a starting opennova-serve no lobby, and it gives up past its
// 30 s probe and exits (retail's, SESSION_GATE_PROBE_TIMEOUT_MS). So then no
// server is placed (each waits in provisioning, "waits for a live lobby"), an
// exit places the server again with no restart counted, and the listing's
// clocks wait (a listing server's timeout, a running one's listing_lost_ms:
// its lister reconnects to the lobby address and lists again once a lobby is
// back there); once one is live, the waiting servers start. A stopped or
// failed server's service account is deleted with the transition that ends it.
// Always on: nothing reaps an idle server, and nothing ends one for its age.
//
// The machines (fleet_repository.h, each move a compare-and-set):
//
//   launching -> booting   its machine.launch job's launch answered
//                          (mark_launched); a launch the cloud refused leaves
//                          the row failed, which the next pass counts: no
//                          launch of that kind for launch_backoff_ms, doubled
//                          per failure in a row to launch_backoff_max_ms (a
//                          failure while that backoff holds, of a launch made
//                          before it, counts once). One whose job never
//                          finished in the boot timeout fails too
//   booting -> ready       its agent's first sync (the agent routes), which
//                          ends its kind's backoff; one that never reports in
//                          the driver's boot_timeout_ms is terminated and
//                          failed, a failed launch as above
//   ready -> lost          its agent silent for machine_silence_s (60): the
//                          cloud is asked; gone, it ends now, running, it is
//                          terminated lost_end_ms (3 min) on, and an agent that
//                          reports again first makes it ready again. The
//                          mass-lost brake: while more than max(2, half) of the
//                          live machines are lost at once (a bad hostd rolled
//                          to every machine, the fleet's network gone), a lost
//                          machine's lost_end_ms holds instead (its row's
//                          lost_held_since_ms, an ERROR, the heartbeat's
//                          machines_held), each until it reports again or an
//                          admin ends it, the brake let go of or not
//   ready -> draining      an admin's drain or a fleet roll: its servers move
//                          off one at a time (the next once the last moved one
//                          runs again elsewhere), and once it holds none it ends
//   ready -> ended         a shared machine idle (no server on it) for idle_ms
//                          (ONNET_FLEET_IDLE_MINUTES), an own machine whose
//                          server ended or moved to a shared machine; an
//                          admin's end (ending) the next pass
//
// At start() the supervisor reconciles: a machine row still launching whose
// launch job is gone fails, a fleet.sweep is enqueued (a cloud machine no live
// row names is terminated, a live row whose cloud machine is gone ends at the
// next pass), a server whose machine is gone (or whose files went with the
// supervisor: booting and reported neither running nor exited, so an agent
// that waits for its image or its trees may lack them too) is placed again (a
// new generation, its files rendered afresh, no restart counted), and the rest resume
// where they were (a running one gets relist_grace_ms for its lister to
// re-host before the health checks count). The sweep runs again every
// kFleetSweepIntervalMs (fleet_jobs.h fleet_schedule). A driver that cannot list its
// machines has nothing terminated or ended on that.
//
// The desired state (sync): the containers of the live servers placed on the
// machine that are booting, listing, running or stopping (a stopping one only
// once its agent has reported it running or exited), each with the files
// rendered at its start until the agent reports it running or exited (one it
// reports starting waits for its image or its trees, and an agent replaced
// meanwhile, a hostd rolled, needs them again); the service keeps those files in
// memory alone, so a server whose start the service's restart cut short is
// rendered again (a new generation, new credentials); the trees those
// containers are pinned to, each with its URL in the object store (tree_url)
// and its manifest (a ready tree is immutable: its spec is read once and
// kept); drain is set while the machine drains, or is lost after draining (its
// row's was_draining). sync runs on the internal listener's handler threads;
// it locks what it shares with the pass, and reads the database outside that
// lock.
//
// The static addresses (hosted/model/address_repository.h, ADR 0066): an own
// machine launched for a server that names an address carries it
// (fleet_machines.address_id). Once the machine has a cloud id the pass
// enqueues its address.associate (and asks a describe, at most every
// describe_interval_ms, until its row shows the address), and holds the
// server's container back (booting, its clock waiting; sync answers no
// unstarted container) until the machine's public address is the address, so
// the lobby lists it at <address>:32768 from its first packet: an own
// machine's public address never changes while it runs a server. An address
// whose grant is gone lapses: a subscription's whose subscription no longer
// grants, and a comp one nothing covers (address_repository.h
// uncovered_comp_addresses: its account gone or no longer an active
// player's, or its newest comp ones while the account's addresses pass its
// own machines; a paid one never lapses for that). One the billing cache
// judged (a subscription's, a comp one of an account with subscriptions)
// lapses only once a clean reconcile with Stripe has completed since the pass
// first saw it so (limits_hold.h reconciles), and never more at once than
// max(2, half the addresses), the owners' limits' brake (the pass then logs
// an ERROR and lapses none of them; an admin may release one); one its
// account and its admin's grant alone judged lapses at once. A lapsed address's server
// lets go of it: its machine is ended (the association goes with it) and the
// server placed again without it, no restart counted, its event saying why;
// once no live machine carries the address it goes releasing with its
// address.release (re-armed each pass while it releases). With the add-on on
// (placement.max_addresses above 0) an allocating address's address.allocate
// is re-armed the same way, and a failed one whose refusal passes goes
// allocating again once its retry_at is reached (its backoff: the worker's
// fail_address).
//
// The admin link (admin_link.h): with set_admin_links, every pass ends by
// syncing the links with the servers in running, at their machine's private
// address and their slot's admin port.
//
// The starts (hosted_repository.h hosted_server_starts): each generation's row
// is made with its slot, given its image, trees and log stream when it renders,
// marked running when its agent first reports it so, and ended with its cause
// wherever the pass lets go of the slot or ends the server (a crash or a start
// that timed out, the machine's or NovaWorld's cause, a stop, the owner's
// update, the owner's limits, no data, the admin link lost), an exited
// container's code, OOM flag and exit line kept.
//
// The stats (stats_sampler.h): the first pass of each minute of the wall clock
// (wall_clock_s) samples every live server and every live machine, its link's
// scene among the rest, and rolls the servers' hours up.
//
// The routes only write requests, stops and machine moves (hosted_repository.h,
// fleet_repository.h); every transition past them is the supervisor's, a
// compare-and-set on the row, so a stop that lands while a pass places a server
// wins. Each pass leases its own pooled connection.

#include "admin_link.h"
#include "fleet_desired_state.h"
#include "stats_sampler.h"

#include "billing/entitlement.h"
#include "data_sets/data_set_repository.h"
#include "hosted/model/address_repository.h"
#include "hosted/model/fleet_repository.h"
#include "hosted/model/hosted_repository.h"
#include "hosted/model/limits_hold.h"
#include "hosted/model/machine_driver.h"
#include "hosted/model/placement.h"

#include <net/novaworld/db/database.h>
#include <net/novaworld/gate_probe.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

struct SupervisorConfig {
	// ONNET_HOSTED_CAP: the global cap, a backstop beside the fleet's room. The
	// start route checks it (in the request's transaction); the supervisor logs it.
	int cap = 24;
	// --master-host / --master-gate-port: where every server reaches NovaWorld
	// (ONNET_PUBLIC_HOST; a test's own gate).
	std::string master_host = "127.0.0.1";
	uint16_t master_gate_port = GATE_DEFAULT_PORT;
	// Where a machine's agent fetches tree `tree_id` from: the object store's
	// URL of trees/<id>/ (core/object_store.h url_of; the fleet holds no store,
	// only this pure function of its config). Unset, the replies name no URL.
	std::function<std::string(int64_t tree_id)> tree_url;
	// The fleet (fleet_config.h): placement's knobs, how long a shared machine
	// with no server lives on, each container's limits, and the serve image a
	// start pins (ONNET_HOSTED_SERVE_IMAGE; a local stack's machine runs its
	// own image's binary, which this names).
	PlacementPolicy placement;
	uint64_t idle_ms = 30 * 60 * 1000;
	double slot_cpus = 1.0;
	int slot_memory_mb = 512;
	int slot_pids = 128; // fleet_config.h kSlotPids
	std::string serve_image = "opennova-serve:local";
	// The pass cadence. The rest is service policy.
	uint64_t tick_interval_ms = 1000;
	// A container's start, counted once its machine is ready: a fresh machine
	// fetches its trees first (1-2 GB), and an image pull besides.
	uint64_t boot_timeout_ms = 15 * 60 * 1000;
	uint64_t listing_timeout_ms = 3 * 60 * 1000;
	// A server running at the service's start: its health checks wait this
	// long for it to list again, on either driver. The restart left its
	// lister's NOVAWORLDUDP session dead, which opennova-serve drops only past
	// the service template's 240 s of receive silence, retail's
	// (novaworldudp_session_timeout_ms; [orig: CNapiGameSession_InitNPConnection
	// @0x4d3e1f], the reap [orig: CNapiNPConnection_PumpStateMachine @0x6292e0]),
	// before it reconnects and re-hosts: twice that 240 s.
	uint64_t relist_grace_ms = 2 * 240 * 1000;
	// A running server whose listing no health check has found for this long
	// is placed again, no restart counted: its lobby went away (a lobby's
	// replacement takes its listings, ADR 0063 d8). Its lister, opennova-serve's
	// NWU session (retail's), notices only past those 240 s of receive silence,
	// then reconnects to the lobby address the gate gave it, never through the
	// gate again: its 0x41 to the gate's UDPNOVAWORLD targets 1 s on, every 3 s
	// in 10 s windows, the re-join's 0x42 for up to 20 s, then the re-verify
	// re-hosts (net/napi/session.h SESSION_RECONNECT_*, SESSION_JOIN_TIMEOUT_MS;
	// [orig: CNapiNPConnection_PumpStateMachine @0x6292e0, the reconnect tail
	// @0x629487..0x6296cb; CNapiNPConnection_PumpEnumeratorAndSend @0x6290c0,
	// the target walk @0x629183..0x6291b4;
	// CNapiGameSession_HandleConnectVerifyResponse @0x4d5961..0x4d599e]). A
	// lobby back at that address lists it again within about 271 s (240 + 1 +
	// 10 + 20, one health interval later here); one at a new address, an ECS
	// task's own, never does, and only a new start, through the gate, lists it
	// again. Twice the 240 s, as above.
	uint64_t listing_lost_ms = 2 * 240 * 1000;
	uint64_t health_interval_ms = 20 * 1000;
	int health_misses = 3;
	// At most restart_limit crash restarts within the restart window: a run of
	// restart_reset_ms in `running` sets the count back to 0.
	int restart_limit = kRestartLimit;
	uint64_t restart_reset_ms = 60 * 60 * 1000;
	uint64_t restart_backoff_ms = 15 * 1000;
	uint64_t stop_timeout_ms = 60 * 1000;
	// A booting, lost or unaddressed machine's describe, asked at most this
	// often (the ec2 driver reads one describe of every machine at most every
	// 10 s anyway).
	uint64_t describe_interval_ms = 10 * 1000;
	// A machine whose agent last reported longer ago than this is lost; a lost
	// one still running is terminated lost_end_ms later, unless the mass-lost
	// brake holds it (the class comment).
	int64_t machine_silence_s = 60;
	uint64_t lost_end_ms = 3 * 60 * 1000;
	// After a failed launch of a kind (the cloud refused it, or its machine
	// never booted), none of that kind for this long, doubled per failure in a
	// row (1, 2, 4, 8 ... min) to launch_backoff_max_ms; a machine of the kind
	// that boots ends it (fleet_launch_backoff).
	uint64_t launch_backoff_ms = 60 * 1000;
	uint64_t launch_backoff_max_ms = 30 * 60 * 1000;
	// The wall clock, Unix seconds, the owners' hosting entitlements are read
	// at (a subscription's period end is a date); the tests step it by hand.
	std::function<int64_t()> wall_clock_s = billing::unix_now;
	// The passes' clock, Unix milliseconds: what the machine rows' times are
	// on, so another supervisor reads them (the tests step one by hand).
	std::function<uint64_t()> clock_ms = wall_clock_ms;

	static uint64_t wall_clock_ms();
};

class Supervisor : public FleetDesiredState {
public:
	// The ServerStopHosting push to listed RID `rid`, scoped to the account that
	// holds it (lobbydb::queue_stop_hosting with its owner, a command for the
	// lobby), on the pass's connection `db`; true once queued.
	using StopListing = std::function<bool(db::Database &db, uint32_t rid, int64_t account_user_id)>;

	// `pool` must outlive the supervisor. `profile` is its driver's
	// (MachineProfile); the driver itself is the fleet jobs' (fleet_jobs.h).
	Supervisor(db::ConnectionPool &pool, MachineProfile profile, SupervisorConfig config);
	~Supervisor() override;
	Supervisor(const Supervisor &) = delete;
	Supervisor &operator=(const Supervisor &) = delete;

	// Set before start(); unset, a stop ends the container without the push.
	void set_stop_listing(StopListing push) { stop_listing_ = std::move(push); }
	// The admin links the passes keep in step with the running servers; set
	// before start(), and it must outlive the supervisor's thread. Unset, no
	// server gets one.
	void set_admin_links(AdminLinks *links) { admin_links_ = links; }

	// Reconciles, then runs a pass every tick_interval_ms on the supervisor's own
	// thread until stop().
	void start();
	// Joins the thread; the machines and their containers keep running (the
	// driver's to end). sync() still answers after it.
	void stop();

	// The boot's reconciliation and one pass, at `now_ms` on the passes' clock
	// (clock_ms): what the thread runs, and what the tests drive by hand.
	void reconcile(uint64_t now_ms);
	void tick(uint64_t now_ms);

	// FleetDesiredState: machine `machine`'s containers (the class comment).
	FleetDecision sync(db::Database &db, const FleetMachineRow &machine,
			const std::vector<ContainerReport> &reported) override;

	const SupervisorConfig &config() const { return config_; }

	const MachineProfile &profile() const { return profile_; }

	// The passes' progress (one per server stepped and one per pass): the stall
	// watchdog's counter (core/health.h). Thread-safe.
	uint64_t pass_turns() const { return pass_turns_.load(std::memory_order_relaxed); }
	// The machine launches that failed so far, refused or never booted, as this
	// supervisor's passes counted them (the heartbeat's launch_failures counts
	// those since its last line). Thread-safe.
	uint64_t launch_failures() const { return launch_failures_.load(std::memory_order_relaxed); }
	// When the last pass that failed in nothing ended, on the passes' clock
	// (clock_ms): a step that hit a database error, or a pass cut short, is no
	// such pass. start() sets it, so a leader whose passes all fail ages from its
	// start (the fleet leader's heartbeat's fleet_pass_age_s). Thread-safe.
	uint64_t last_good_pass_ms() const {
		return last_good_pass_ms_.load(std::memory_order_relaxed);
	}

	// A guard each pass asks first, on the pass's connection (a fleet process
	// that still leads, election.h); false skips the pass. It is asked
	// again inside the transaction of each write that enqueues cloud work (a
	// machine's launch with its row, an end with its terminate), where false
	// writes nothing: a pass a new leader's election overtook launches and ends
	// no machine. Set before start(); unset, every pass runs.
	void set_pass_guard(std::function<bool(db::Database &)> guard) {
		pass_guard_ = std::move(guard);
	}

private:
	// What a pass remembers of a live server between passes (the database holds
	// the rest): since when it has been in its state, the health misses, and the
	// stop's push.
	struct Runtime {
		State state = State::Requested;
		uint64_t since_ms = 0;
		uint64_t last_check_ms = 0;
		// The last health check that found its listing, or the last pass its
		// machine was lost (the listing's clock waits then); since_ms before either.
		uint64_t listing_seen_ms = 0;
		uint64_t grace_until_ms = 0;  // a resumed server's re-host window
		uint64_t not_before_ms = 0;   // a restart's backoff
		int misses = 0;
		bool stop_pushed = false;
		uint64_t stop_pushed_ms = 0;
		bool waiting_logged = false;  // "waits for a slot", once
		bool lobby_wait_logged = false; // "waits for a live lobby", once
		bool launch_failure_noted = false; // its machine's failed launch, in its events once
	};
	// A start the supervisor rendered: the container it wants on the server's
	// slot, its files kept until the machine's agent reports it running or
	// exited.
	struct Start {
		int64_t machine_id = 0;
		DesiredContainer container;
		bool acknowledged = false;
	};
	// A pass's view of the fleet: the live machines and their agents' last
	// reports, by machine and slot.
	struct Fleet {
		std::map<int64_t, FleetMachineRow> machines;
		std::map<std::pair<int64_t, int>, SlotReport> reports;
	};

	void run();
	Fleet read_fleet(db::Database &db);
	const FleetMachineRow *machine_of(const Fleet &fleet, const HostedServer &s) const;
	// The report of the server's own container on its slot, or null.
	const SlotReport *report_of(const Fleet &fleet, const HostedServer &s) const;

	void step(db::Database &db, const Fleet &fleet, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_requested(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_provisioning(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_booting(db::Database &db, const Fleet &fleet, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_listing(db::Database &db, const Fleet &fleet, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_running(db::Database &db, const Fleet &fleet, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_stopping(db::Database &db, const Fleet &fleet, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	// The server's place: a slot it is started on at once (on a machine launched
	// for it, if need be), or a wait.
	void place(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	// A machine of `kind` launched for `server` (an own one reserved for it);
	// false when none was (the launch backoff, a refused launch: the server
	// waits, no restart counted).
	bool launch_machine(db::Database &db, const HostedServer &server, MachineKind kind, Runtime &rt,
			uint64_t now_ms);
	// The start on the slot `server` (read after assign_slot) holds on
	// `machine`: the trees pinned, the account, the logins, the files, the
	// container wanted.
	void start_on_slot(db::Database &db, const HostedServer &server, const FleetMachineRow &machine,
			const SlotPlan &plan, Runtime &rt, uint64_t now_ms);
	// The server's data set rows: its base's title (/game) and its expansion's
	// code (/exp, "" with none); nullopt with `why` when a row is gone.
	struct ServerData {
		std::string title;
		std::string expansion_code;
	};
	std::optional<ServerData> data_of(db::Database &db, const HostedServer &server, std::string &why);
	// Whether the server's pins name trees that are still there (not pruned):
	// what a crash restart or a move keeps.
	bool pins_hold(db::Database &db, const HostedServer &server);
	// Tree `tree_id` as the reply carries it (tree_url and its manifest), from
	// the cache or its row; nullopt for a tree with no manifest.
	std::optional<TreeSpec> tree_spec(db::Database &db, int64_t tree_id);
	// Its container exited while it should run: a crash (or, one that never
	// started because its data could not be fetched, placed again after the
	// backoff with no restart counted).
	void on_exit(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const SlotReport &report);
	// A crash (the server's cause: `cause` Crash or Timeout, its start ended so):
	// its slot let go, then a restart while the server has some left (after the
	// backoff), else failed.
	void attempt_failed(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const std::string &reason, StartCause cause);
	// The machine's cause (or the owner's update, `cause`): its slot let go and
	// placed again at once, no restart counted (a stopping server's stop ends
	// instead).
	void replace(db::Database &db, const HostedServer &server, uint64_t now_ms,
			const std::string &reason, StartCause cause = StartCause::Fleet);
	// The end: the row stopped or failed with its service account deleted, in
	// one transaction, its start ended with `cause`; its container no longer
	// wanted.
	void finish(db::Database &db, const HostedServer &server, State to, const std::string &detail,
			StartCause cause);
	// A transition the pass made, logged and mirrored in `rt`.
	bool move(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms, State to,
			const std::string &detail, TransitionOptions options = {});
	// Whether the owner may keep the server: its account exists and is active
	// (the entitlement is enforce_limits').
	bool owner_active(db::Database &db, const HostedServer &server, std::string &why);
	// Stops the servers on shared machines past their owners' shared servers,
	// and moves those past their dedicated machines to shared machines (the
	// class comment); the ids it stopped.
	std::set<int64_t> enforce_limits(db::Database &db, const std::vector<HostedServer> &live, uint64_t now_ms);
	// A server on a dedicated machine its owner no longer has (`own_machines` of
	// them now) moved to a shared machine, when its owner has a shared server
	// free and the fleet room for it: its dedicated-machine choice cleared and
	// its slot let go together, under the lock a start is judged under
	// (hosted_repository.h clear_own_machine), and placed again as replace
	// places a server, its start ended for its owner's limits and its event
	// saying why. Nothing is written but on Moved.
	MoveOutcome move_to_shared(db::Database &db, const HostedServer &server, uint64_t now_ms, int own_machines);
	// The admin links brought in step with the servers in running (their rows
	// read again, after this pass's moves).
	void sync_admin_links(db::Database &db);
	void forget_start(int64_t server_id);

	// Each live machine's step (the machines' lifecycle in the class comment),
	// what it remembers of each kept in its row.
	void step_machines(db::Database &db, uint64_t now_ms);
	// An addressed machine's association asked for (and a describe) while its
	// row does not show the address (the class comment).
	void carry_address(db::Database &db, const FleetMachineRow &machine, uint64_t now_ms);
	// The static addresses' lapses, releases and allocations again (the class
	// comment).
	void step_addresses(db::Database &db, uint64_t now_ms);
	void drain_step(db::Database &db, FleetMachineRow &machine,
			const std::vector<HostedServer> &holders, uint64_t now_ms);
	// The launches the cloud refused and not counted yet (their jobs' rows), at
	// a pass's start and before each launch: each counted (launch_failed) and
	// its server's event written, once per wait.
	void note_refused_launches(db::Database &db, uint64_t now_ms);
	// A launch of `kind` failed (the cloud refused it, or its machine never
	// booted): counted for the heartbeat's launch_failures, and, unless a
	// backoff of the kind already holds, one more failure in a row: no launch of
	// that kind for launch_backoff_ms doubled per failure before it, at most
	// launch_backoff_max_ms (the row fleet_launch_backoff).
	void launch_failed(db::Database &db, MachineKind kind, uint64_t now_ms);
	// A machine.describe of `machine`, at most every describe_interval_ms; its
	// answer lands on the row (cloud_state) for a later pass.
	void ask_describe(db::Database &db, const FleetMachineRow &machine, uint64_t now_ms);
	// The machine's row moved to `to` (ended or failed) with `reason` and its
	// cloud machine's machine.terminate, in one transaction; each server on it
	// placed again, no restart counted (the machine's cause, whatever it was).
	void end_machine(db::Database &db, const FleetMachineRow &machine, FleetMachineState to,
			const std::string &reason, uint64_t now_ms);

	db::ConnectionPool &pool_;
	const MachineProfile profile_;
	const SupervisorConfig config_;
	StatsSampler sampler_;
	StopListing stop_listing_;
	AdminLinks *admin_links_ = nullptr;
	std::map<int64_t, Runtime> runtimes_;
	// Whether a lobby instance took clients at this pass's start (the class
	// comment's "no lobby is live"); the pass thread's alone.
	bool lobby_live_ = true;
	// This pass's servers placed on a machine the cloud has not answered for yet
	// (no address: launching), by machine and slot: the slot each will take once
	// it has one, which the servers placed after it in the pass see as held (so
	// twenty servers needing twenty machines launch them all in one pass).
	std::vector<std::pair<int64_t, int>> pass_reserved_;
	// The pass's memory of the static addresses: when it last asked a machine's
	// association (by machine), the clean reconciles counted when it first saw
	// an address's grant gone, as the billing cache judged it (by address), and whether
	// the lapses' brake holds (logged once).
	std::map<int64_t, uint64_t> associate_asked_ms_;
	std::map<int64_t, uint64_t> lapse_seen_;
	bool address_brake_held_ = false;

	// The starts sync() answers from, by server id (the pass writes, the
	// internal listener's threads read). No database work is done under
	// starts_mu_.
	std::mutex starts_mu_;
	std::map<int64_t, Start> starts_;
	// The ready trees' specs, by tree id (immutable once ready).
	std::mutex trees_mu_;
	std::map<int64_t, TreeSpec> tree_specs_;

	std::thread thread_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool stop_requested_ = false;
	std::atomic<bool> running_{ false };
	std::atomic<uint64_t> pass_turns_{ 0 };
	std::atomic<uint64_t> launch_failures_{ 0 };
	std::atomic<uint64_t> last_good_pass_ms_{ 0 };

	std::function<bool(db::Database &)> pass_guard_;
};

} // namespace opennova::novaworld_server::hosted
