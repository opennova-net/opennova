#pragma once

// The hosted servers' supervisor (ADR 0053, proposed): a thread of the
// NovaWorld service that walks every live hosted server through its lifecycle
// on a MachineProvider, logging every transition in hosted_server_events.
//
//   requested     the owner asked (POST /api/me/server/start): the account's
//                 entitlement, its one live server and the cap were checked then
//   provisioning  the service account srv-<id> with a new random password
//                 (minted at the first start, its password set again at every
//                 one), a new admin password, the provider's plan, the files
//                 rendered from scratch (hosted_settings.h) and the machine
//                 launched on the server's data set (data_sets.h; one this
//                 service no longer offers fails the server, which no restart
//                 mends)
//   booting       until the machine runs opennova-serve
//   listing       until the server's listing appears (a listed RID with one of
//                 the service account's HostKeys claimed)
//   running       every health interval the listing is checked and (with admin
//                 links) the server's admin link answering, every pass the
//                 process; health_misses failed checks in a row end the
//                 machine and fail the server
//   stopping      asked for by the owner, an admin or the supervisor itself (an
//                 owner's account deleted or no longer active, or its hosting
//                 entitlement down to no server, billing/entitlement.h: a
//                 comp grant withdrawn, a support tier canceled; read every
//                 pass and again right before a start mints credentials): the
//                 ServerStopHosting push to the listing, then the machine ended
//                 by the supervisor once the listing has left the browser, or
//                 stop_timeout_ms after the push; opennova-serve exiting in this
//                 state (with any code) ends the stop too, never a restart
//   stopped       a stop's end
//   failed        the restarts used up, or a fault no restart mends
//
// A stop is the supervisor's own act, never read from an exit code: while the
// server should run, opennova-serve exiting with ANY code (0 for its
// rotation's end or the admin's GOTO MENUSTATE, 3 when the service ends its
// hosting, 1 for a SIGTERM while it waits to be listed) is a crash, as are its
// machine gone, the boot or the listing not coming in time and a launch that
// fails. A crash ends the machine and goes back to provisioning after
// restart_backoff_ms while the server has restarts left: restart_limit of them
// (3), and an hour (restart_reset_ms) of continuous running resets the count,
// so an always-on server that crashes now and then over weeks is never failed
// for it. A stopped or failed server's
// service account is deleted with the transition that ends it. Always on:
// nothing reaps an idle server, and nothing ends one for its age.
//
// At start() the supervisor reconciles the provider's machines with the rows: a
// machine no live row names is ended, a live row's machine that still runs
// resumes where it was (a running one gets listing_timeout_ms for its lister to
// re-host before the health checks count), and a live row whose machine is gone
// is launched again (a service restart is no crash: it costs no restart). The
// first half, ending the machines no live row names, runs again every
// orphan_sweep_interval_ms. A provider that cannot list its machines (its
// list_tagged throws) has nothing ended or launched again on that: at the boot
// every live row resumes as if its machine ran, until the passes' describes
// tell, and a sweep that fails leaves its pass to step the servers.
//
// The admin link (admin_link.h): with set_admin_links, every pass ends by
// syncing the links with the servers in running, so a server's link comes up
// once it runs, is replaced when a restart rendered a new login, and goes down
// as the server leaves running (at stopping, or a crash).
//
// The routes only write requests and stops (hosted_repository.h); every
// transition past them is the supervisor's, a compare-and-set on the row, so a
// stop that lands while a pass is launching wins and the launched machine is
// ended on the next pass. Each pass leases its own pooled connection.

#include "admin_link.h"
#include "billing/entitlement.h"
#include "data_sets.h"
#include "hosted_repository.h"
#include "machine_provider.h"

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
	// ONNET_HOSTED_CAP: the global cap. The start route checks it (in the
	// request's transaction); the supervisor logs it.
	int cap = 5;
	// --master-host / --master-gate-port: where every server reaches NovaWorld
	// (loopback for the local provider, ONNET_PUBLIC_HOST otherwise).
	std::string master_host = "127.0.0.1";
	uint16_t master_gate_port = GATE_DEFAULT_PORT;
	// The data sets the provider offers (offered_data_sets): a server's game
	// picks its title (/game) and the data its machine mounts.
	std::vector<DataSet> data_sets;
	// The pass cadence. The rest is service policy.
	uint64_t tick_interval_ms = 1000;
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
	// The wall clock, Unix seconds, the owners' hosting entitlements are read
	// at (a subscription's period end is a date); the tests step it by hand.
	std::function<int64_t()> wall_clock_s = billing::unix_now;
};

class Supervisor {
public:
	// The ServerStopHosting push to listed RID `rid`, scoped to the account that
	// holds it (NwUdpListener::push_stop_hosting with its owner); true once queued.
	using StopListing = std::function<bool(uint32_t rid, int64_t account_user_id)>;

	Supervisor(db::ConnectionPool &pool, MachineProvider &provider, SupervisorConfig config);
	~Supervisor();
	Supervisor(const Supervisor &) = delete;
	Supervisor &operator=(const Supervisor &) = delete;

	// Set before start(); unset, a stop ends the machine without the push.
	void set_stop_listing(StopListing push) { stop_listing_ = std::move(push); }
	// The admin links the passes keep in step with the running servers; set
	// before start(), and it must outlive the supervisor's thread. Unset, no
	// server gets one.
	void set_admin_links(AdminLinks *links) { admin_links_ = links; }

	// Reconciles, then runs a pass every tick_interval_ms on the supervisor's own
	// thread until stop().
	void start();
	// Joins the thread; the machines keep running (the provider's to end: the
	// local provider starts its children from a thread of its own, so joining
	// this one ends none of them).
	void stop();

	// The boot's reconciliation and one pass, at `now_ms` on a steady clock in
	// milliseconds: what the thread runs, and what the tests drive by hand.
	void reconcile(uint64_t now_ms);
	void tick(uint64_t now_ms);

	const SupervisorConfig &config() const { return config_; }

	// The passes' progress (one per server stepped and one per pass): the stall
	// watchdog's counter (health.h). Thread-safe.
	uint64_t pass_turns() const { return pass_turns_.load(std::memory_order_relaxed); }
	// The launches that failed so far (the heartbeat's launch_failures counts
	// those since its last line). Thread-safe.
	uint64_t launch_failures() const { return launch_failures_.load(std::memory_order_relaxed); }

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
	};

	void run();
	void step(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_requested(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_provisioning(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_booting(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_listing(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_running(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	void on_stopping(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms);
	// The machine's process exited (with any code) or the machine went while the
	// server should run: a crash (attempt_failed). Never a stop.
	void on_machine_down(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const MachineStatus &status);
	// A failed start or a crash: the machine ended, then a restart while the
	// server has some left (after the backoff), else failed.
	void attempt_failed(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const std::string &reason);
	// The end: the machine ended, then the row stopped or failed with its service
	// account deleted, in one transaction.
	void finish(db::Database &db, const HostedServer &server, State to, const std::string &detail);
	// A transition the pass made, logged and mirrored in `rt`.
	bool move(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms, State to,
			const std::string &detail, TransitionOptions options = {});
	// Whether the owner may keep the server: its account exists, is active and
	// has a hosting entitlement of at least one server.
	bool owner_active(db::Database &db, const HostedServer &server, std::string &why);
	// Ends every machine the provider runs that no live row in `live` names
	// (`when` labels the log line); the ids of the ones it keeps.
	std::set<std::string> end_untracked(const std::vector<HostedServer> &live, const char *when);
	// The provider's describe of the server's machine; an address the row lacks
	// (none was known at the launch) is written from it.
	MachineStatus describe(db::Database &db, const HostedServer &s);
	// The admin links brought in step with the servers in running (their rows
	// read again, after this pass's moves): each one's machine address and
	// admin login.
	void sync_admin_links(db::Database &db);

	db::ConnectionPool &pool_;
	MachineProvider &provider_;
	const SupervisorConfig config_;
	StopListing stop_listing_;
	AdminLinks *admin_links_ = nullptr;
	std::map<int64_t, Runtime> runtimes_;
	uint64_t last_orphan_sweep_ms_ = 0;

	std::thread thread_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool stop_requested_ = false;
	std::atomic<bool> running_{ false };
	std::atomic<uint64_t> pass_turns_{ 0 };
	std::atomic<uint64_t> launch_failures_{ 0 };
};

} // namespace opennova::novaworld_server::hosted
