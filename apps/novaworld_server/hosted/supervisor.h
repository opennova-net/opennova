#pragma once

// The hosted servers' supervisor (ADR 0053, proposed): a thread of the
// NovaWorld service that walks every live hosted server through its lifecycle
// on a slot of one of the fleet's machines, logging every transition in
// hosted_server_events, and keeps the machines (fleet_machines) in step: it
// places servers (placement.h), launches and ends machines through the
// MachineDriver, and answers each machine agent's sync with the containers its
// machine should run (FleetDesiredState, agent_route.h). A machine is never a
// host (CONTEXT.md "Machine").
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
//                 scratch for the slot's ports (hosted_settings.h). A server
//                 past its owner's entitlement, counted in id order among the
//                 owner's live servers not stopping, is stopped instead, and one
//                 whose data set this service no longer offers fails, which no
//                 restart mends
//   booting       its container (srv-<id>-g<generation>) is wanted on its slot;
//                 the machine's agent starts it once the slot is free (its
//                 boot_timeout_ms counts from the machine's ready)
//   listing       its agent reports it running; until the server's listing
//                 appears (a listed RID with one of the service account's
//                 HostKeys claimed)
//   running       every health interval the listing is checked and (with admin
//                 links) the server's admin link answering, every pass its
//                 agent's report; health_misses failed checks in a row fail it
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
// The owners' limits: every pass opens by stopping what an owner's hosting
// entitlement no longer covers (enforce_limits): its newest live servers past
// the entitlement's servers, then its newest on own machines past the add-on's,
// a stopping one counting toward neither, each with an event its owner reads
// ("its owner's support tier allows 1 hosted server"). An entitlement is read
// from the billing cache whether or not billing is configured, and a past_due
// tier keeps its servers. A pass that would stop more owners' servers than
// max(2, half the owners with live servers) stops none: it logs an ERROR and
// holds (limits_held, limits_hold.h), so a cache gone wrong never mass-stops
// payers. While held no limit stops. The hold releases once a full reconcile
// with Stripe has completed clean after it and the same owners (or some of
// them) are still past their entitlements, whose stops then apply; an admin's
// override (POST /api/admin/billing/reconcile {"force_limits": true}) applies
// them at the next pass.
//
// Restarts. A stop is the supervisor's own act, never read from an exit code.
// What the server itself causes is a crash and counts: while the server should
// run, its container exiting with ANY code (0 for its rotation's end or the
// admin's GOTO MENUSTATE, 3 when the service ends its hosting, 1 for a SIGTERM
// while it waits to be listed), the kernel ending it out of memory, a start
// that fails (docker's 125), its container or its listing not coming in time,
// a machine launched for it that the cloud refused, and a machine it was placed
// on that never booted. A crash lets go of the slot and goes back to
// provisioning after restart_backoff_ms while the server has restarts left:
// restart_limit of them (3), and an hour (restart_reset_ms) of continuous
// running resets the count. What the fleet causes is no crash and costs none:
// its machine lost, ended by an admin, gone, or drained (an admin's drain, a
// fleet roll, an agent on another protocol). The server lets go of its slot and
// is placed again at once. A stopped or failed server's service account is
// deleted with the transition that ends it. Always on: nothing reaps an idle
// server, and nothing ends one for its age.
//
// The machines (fleet_repository.h, each move a compare-and-set):
//
//   launching -> booting   the driver's launch answered (mark_launched); a
//                          launch that fails leaves the row failed, and no
//                          launch of that kind for launch_backoff_ms
//   booting -> ready       its agent's first sync (the agent routes); one that
//                          never reports in the driver's boot_timeout_ms is
//                          terminated and failed
//   ready -> lost          its agent silent for machine_silence_s (60): the
//                          cloud is asked; gone, it ends now, running, it is
//                          terminated lost_end_ms (3 min) on, and an agent that
//                          reports again first makes it ready again
//   ready -> draining      an admin's drain or a fleet roll: its servers move
//                          off one at a time (the next once the last moved one
//                          runs again elsewhere), and once it holds none it ends
//   ready -> ended         a shared machine idle (no server on it) for idle_ms
//                          (ONNET_FLEET_IDLE_MINUTES), an own machine whose
//                          server ended; an admin's end (ending) the next pass
//
// At start() the supervisor reconciles: a machine row still launching (the
// service went mid-launch) fails, a cloud machine no live row names (tagged by
// the driver) is terminated, a live row whose cloud machine is gone ends, a
// server whose machine is gone (or whose files went with the service: booting
// and never reported) is placed again, and the rest resume where they were (a
// running one gets listing_timeout_ms for its lister to re-host before the
// health checks count). The sweep half runs again every
// orphan_sweep_interval_ms. A driver that cannot list its machines (its
// list_tagged throws) has nothing terminated or ended on that.
//
// The desired state (sync): the containers of the live servers placed on the
// machine that are booting, listing, running or stopping (a stopping one only
// once its agent has reported it), each with the files rendered at its start
// until the agent has reported it once; the service keeps those files in
// memory alone, so a server whose start the service's restart cut short is
// rendered again (a new generation, new credentials). sync runs on the
// internal listener's handler threads; it locks what it shares with the pass.
//
// The admin link (admin_link.h): with set_admin_links, every pass ends by
// syncing the links with the servers in running, at their machine's private
// address and their slot's admin port.
//
// The stats (stats_sampler.h): the first pass of each minute of the wall clock
// (wall_clock_s) samples every live server, its link's scene among the rest,
// and rolls its hour up.
//
// The routes only write requests, stops and machine moves (hosted_repository.h,
// fleet_repository.h); every transition past them is the supervisor's, a
// compare-and-set on the row, so a stop that lands while a pass places a server
// wins. Each pass leases its own pooled connection.

#include "admin_link.h"
#include "billing/entitlement.h"
#include "data_sets.h"
#include "fleet_desired_state.h"
#include "fleet_repository.h"
#include "hosted_repository.h"
#include "limits_hold.h"
#include "machine_driver.h"
#include "placement.h"
#include "stats_sampler.h"

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
	// (loopback for the local driver, ONNET_PUBLIC_HOST otherwise).
	std::string master_host = "127.0.0.1";
	uint16_t master_gate_port = GATE_DEFAULT_PORT;
	// The data sets the driver offers (offered_data_sets): a server's game
	// picks its title (/game) and the data its slot mounts.
	std::vector<DataSet> data_sets;
	// The fleet (fleet_config.h): placement's knobs, how long a shared machine
	// with no server lives on, each container's limits, and the serve image a
	// start pins (ONNET_HOSTED_SERVE_IMAGE; the local driver runs its own
	// binary and only names one).
	PlacementPolicy placement;
	uint64_t idle_ms = 30 * 60 * 1000;
	double slot_cpus = 1.0;
	int slot_memory_mb = 512;
	int slot_pids = 128; // fleet_config.h kSlotPids
	std::string serve_image = "opennova-serve:local";
	// The pass cadence. The rest is service policy.
	uint64_t tick_interval_ms = 1000;
	// A container's start, counted once its machine is ready (an image pull
	// included).
	uint64_t boot_timeout_ms = 5 * 60 * 1000;
	uint64_t listing_timeout_ms = 3 * 60 * 1000;
	uint64_t health_interval_ms = 20 * 1000;
	int health_misses = 3;
	// At most restart_limit crash restarts within the restart window: a run of
	// restart_reset_ms in `running` sets the count back to 0.
	int restart_limit = 3;
	uint64_t restart_reset_ms = 60 * 60 * 1000;
	uint64_t restart_backoff_ms = 15 * 1000;
	uint64_t stop_timeout_ms = 60 * 1000;
	uint64_t orphan_sweep_interval_ms = 5 * 60 * 1000;
	// A machine whose agent last reported longer ago than this is lost; a lost
	// one still running is terminated lost_end_ms later.
	int64_t machine_silence_s = 60;
	uint64_t lost_end_ms = 3 * 60 * 1000;
	// After a launch the cloud refused, none of that kind for this long.
	uint64_t launch_backoff_ms = 60 * 1000;
	// The wall clock, Unix seconds, the owners' hosting entitlements are read
	// at (a subscription's period end is a date); the tests step it by hand.
	std::function<int64_t()> wall_clock_s = billing::unix_now;
};

class Supervisor : public FleetDesiredState {
public:
	// The ServerStopHosting push to listed RID `rid`, scoped to the account that
	// holds it (NwUdpListener::push_stop_hosting with its owner); true once queued.
	using StopListing = std::function<bool(uint32_t rid, int64_t account_user_id)>;

	// `pool` and `driver` must outlive the supervisor.
	Supervisor(db::ConnectionPool &pool, MachineDriver &driver, SupervisorConfig config);
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

	// The boot's reconciliation and one pass, at `now_ms` on a steady clock in
	// milliseconds: what the thread runs, and what the tests drive by hand.
	void reconcile(uint64_t now_ms);
	void tick(uint64_t now_ms);

	// FleetDesiredState: machine `machine`'s containers (the class comment).
	FleetDecision sync(db::Database &db, const FleetMachineRow &machine,
			const std::vector<ContainerReport> &reported) override;

	const SupervisorConfig &config() const { return config_; }

	// The passes' progress (one per server stepped and one per pass): the stall
	// watchdog's counter (health.h). Thread-safe.
	uint64_t pass_turns() const { return pass_turns_.load(std::memory_order_relaxed); }
	// The machine launches that failed so far (the heartbeat's launch_failures
	// counts those since its last line). Thread-safe.
	uint64_t launch_failures() const { return launch_failures_.load(std::memory_order_relaxed); }

	// The owners' limits' hold (the class comment, limits_hold.h): set before
	// start(); it must outlive the supervisor. Null: the supervisor's own. The
	// heartbeat's limits_held reads it.
	void set_limits_hold(LimitsHold *hold) { hold_ = hold != nullptr ? hold : &own_hold_; }
	bool limits_held() const { return hold_->held(); }
	std::string limits_hold_reason() const { return hold_->reason(); }
	void clear_limits_hold();

private:
	// What a pass remembers of a live server between passes (the database holds
	// the rest): since when it has been in its state, the health misses, and the
	// stop's push.
	struct Runtime {
		State state = State::Requested;
		uint64_t since_ms = 0;
		uint64_t last_check_ms = 0;
		uint64_t grace_until_ms = 0;  // a resumed server's re-host window
		uint64_t not_before_ms = 0;   // a restart's backoff
		int misses = 0;
		bool stop_pushed = false;
		uint64_t stop_pushed_ms = 0;
		bool waiting_logged = false;  // "waits for a slot", once
	};
	// What a pass remembers of a live machine: since when it has been in its
	// state, since when it has held no server, the server its drain moved last,
	// and whether it drained before it was lost.
	struct MachineRuntime {
		FleetMachineState state = FleetMachineState::Launching;
		uint64_t since_ms = 0;
		uint64_t idle_since_ms = 0;
		int64_t moving = 0;
		bool was_draining = false;
	};
	// A start the supervisor rendered: the container it wants on the server's
	// slot, its files kept until the machine's agent has reported it.
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
	// false when none was (the launch backoff, a refused launch).
	bool launch_machine(db::Database &db, const HostedServer &server, MachineKind kind, Runtime &rt,
			uint64_t now_ms);
	// The start on the slot `server` (read after assign_slot) holds on
	// `machine`: the account, the logins, the files, the container wanted.
	void start_on_slot(db::Database &db, const HostedServer &server, const FleetMachineRow &machine,
			const SlotPlan &plan, Runtime &rt, uint64_t now_ms);
	// Its container exited while it should run: a crash.
	void on_exit(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const SlotReport &report);
	// A crash (the server's cause): its slot let go, then a restart while the
	// server has some left (after the backoff), else failed.
	void attempt_failed(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const std::string &reason);
	// The fleet's cause: its slot let go and placed again at once, no restart
	// counted (a stopping server's stop ends instead).
	void replace(db::Database &db, const HostedServer &server, uint64_t now_ms, const std::string &reason);
	// The end: the row stopped or failed with its service account deleted, in
	// one transaction; its container no longer wanted.
	void finish(db::Database &db, const HostedServer &server, State to, const std::string &detail);
	// A transition the pass made, logged and mirrored in `rt`.
	bool move(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms, State to,
			const std::string &detail, TransitionOptions options = {});
	// Whether the owner may keep the server: its account exists and is active
	// (the entitlement is enforce_limits').
	bool owner_active(db::Database &db, const HostedServer &server, std::string &why);
	// Stops the servers past their owners' entitlements (the class comment);
	// the ids it stopped.
	std::set<int64_t> enforce_limits(db::Database &db, const std::vector<HostedServer> &live);
	// The admin links brought in step with the servers in running (their rows
	// read again, after this pass's moves).
	void sync_admin_links(db::Database &db);
	void forget_start(int64_t server_id);

	// Each live machine's step (the machines' lifecycle in the class comment).
	void step_machines(db::Database &db, uint64_t now_ms);
	void drain_step(db::Database &db, const FleetMachineRow &machine,
			const std::vector<HostedServer> &holders, MachineRuntime &mrt, uint64_t now_ms);
	// The machine terminated and its row moved to `to` (ended or failed) with
	// `reason`; each server on it crashed (`counted`, the server's cause) or
	// placed again.
	void end_machine(db::Database &db, const FleetMachineRow &machine, FleetMachineState to,
			const std::string &reason, bool counted, uint64_t now_ms);
	// The driver's machines against the rows (`when` labels the log lines):
	// terminates each cloud machine no live row names, and ends each live row
	// whose cloud machine is gone. Throws what list_tagged throws.
	void sweep(db::Database &db, uint64_t now_ms, const char *when);

	db::ConnectionPool &pool_;
	MachineDriver &driver_;
	const SupervisorConfig config_;
	StatsSampler sampler_;
	StopListing stop_listing_;
	AdminLinks *admin_links_ = nullptr;
	std::map<int64_t, Runtime> runtimes_;
	std::map<int64_t, MachineRuntime> machine_runtimes_;
	uint64_t last_orphan_sweep_ms_ = 0;
	std::map<MachineKind, uint64_t> launch_not_before_ms_;

	// The starts sync() answers from, by server id (the pass writes, the
	// internal listener's threads read).
	std::mutex starts_mu_;
	std::map<int64_t, Start> starts_;

	std::thread thread_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool stop_requested_ = false;
	std::atomic<bool> running_{ false };
	std::atomic<uint64_t> pass_turns_{ 0 };
	std::atomic<uint64_t> launch_failures_{ 0 };

	LimitsHold own_hold_;
	LimitsHold *hold_ = &own_hold_;
};

} // namespace opennova::novaworld_server::hosted
