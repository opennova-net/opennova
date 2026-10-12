#pragma once

// The hosted servers' supervisor (ADR 0053, proposed): a thread of the
// NovaWorld service that walks every live hosted server through its lifecycle
// on a MachineProvider, logging every transition in hosted_server_events.
//
//   requested     the owner asked (POST /api/me/server/start): the account's
//                 approval, its one live server and the cap were checked then
//   provisioning  the service account srv-<id> with a new random password
//                 (minted at the first start, its password set again at every
//                 one), a new admin password, the provider's plan, the files
//                 rendered from scratch (hosted_settings.h) and the machine
//                 launched
//   booting       until the machine runs opennova-serve
//   listing       until the server's listing appears (a listed RID with one of
//                 the service account's HostKeys claimed)
//   running       every health interval the listing is checked, every pass the
//                 process; health_misses missing listings in a row end the
//                 machine and fail the server
//   stopping      asked for by the owner, an admin or the supervisor itself (an
//                 owner's account deleted or no longer active): the
//                 ServerStopHosting push to the listing, then the machine ended
//                 by the supervisor once the listing has left the browser, or
//                 stop_timeout_ms after the push; opennova-serve exiting in this
//                 state (with any code) ends the stop too, never a restart
//   stopped       a stop's end
//   failed        restart_limit restarts used up, or a fault no restart mends
//
// A stop is the supervisor's own act, never read from an exit code: while the
// server should run, opennova-serve exiting with ANY code (0 for its
// rotation's end or the admin's GOTO MENUSTATE, 3 when the service ends its
// hosting, 1 for a SIGTERM while it waits to be listed) is a crash, as are its
// machine gone, the boot or the listing not coming in time and a launch that
// fails. A crash ends the machine and goes back to provisioning while the
// server has restarts left, after restart_backoff_ms; a stopped or failed
// server's service account is deleted. Always on: nothing reaps an idle
// server, and nothing ends one for its age.
//
// At start() the supervisor reconciles the provider's machines with the rows: a
// machine no live row names is ended, a live row's machine that still runs
// resumes where it was (a running one gets listing_timeout_ms for its lister to
// re-host before the health checks count), and a live row whose machine is gone
// is launched again (a service restart is no crash: it costs no restart).
//
// The routes only write requests and stops (hosted_repository.h); every
// transition past them is the supervisor's, a compare-and-set on the row, so a
// stop that lands while a pass is launching wins and the launched machine is
// ended on the next pass. Each pass leases its own pooled connection.

#include "hosted_repository.h"
#include "machine_provider.h"

#include <net/novaworld/db/sqlite.h>
#include <net/novaworld/gate_probe.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace opennova::novaworld_server::hosted {

struct SupervisorConfig {
	// ONNET_HOSTED_CAP: the global cap. The start route checks it (in the
	// request's transaction); the supervisor logs it.
	int cap = 5;
	// --master-host / --master-gate-port: where every server reaches NovaWorld
	// (loopback for the local provider, ONNET_PUBLIC_HOST otherwise).
	std::string master_host = "127.0.0.1";
	uint16_t master_gate_port = GATE_DEFAULT_PORT;
	// The pass cadence. The rest is service policy.
	uint64_t tick_interval_ms = 1000;
	uint64_t boot_timeout_ms = 5 * 60 * 1000;
	uint64_t listing_timeout_ms = 3 * 60 * 1000;
	uint64_t health_interval_ms = 20 * 1000;
	int health_misses = 3;
	int restart_limit = 3;
	uint64_t restart_backoff_ms = 15 * 1000;
	uint64_t stop_timeout_ms = 60 * 1000;
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

	// Reconciles, then runs a pass every tick_interval_ms on the supervisor's own
	// thread until stop().
	void start();
	// Joins the thread; the machines keep running (the provider's to end).
	void stop();

	// The boot's reconciliation and one pass, at `now_ms` on a steady clock in
	// milliseconds: what the thread runs, and what the tests drive by hand.
	void reconcile(uint64_t now_ms);
	void tick(uint64_t now_ms);

	const SupervisorConfig &config() const { return config_; }

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
	// The machine's process exited (or the machine went) outside a stop: a clean
	// exit stops the server, anything else is a crash.
	void on_machine_down(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const MachineStatus &status);
	// A failed start or a crash: the machine ended, then a restart while the
	// server has some left (after the backoff), else failed.
	void attempt_failed(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms,
			const std::string &reason);
	// The end: the machine ended, the row stopped or failed, the service account
	// deleted.
	void finish(db::Database &db, const HostedServer &server, State to, const std::string &detail);
	// A transition the pass made, logged and mirrored in `rt`.
	bool move(db::Database &db, const HostedServer &server, Runtime &rt, uint64_t now_ms, State to,
			const std::string &detail, TransitionOptions options = {});
	// Whether the owner may keep the server: its account exists and is active.
	bool owner_active(db::Database &db, const HostedServer &server, std::string &why);

	db::ConnectionPool &pool_;
	MachineProvider &provider_;
	const SupervisorConfig config_;
	StopListing stop_listing_;
	std::map<int64_t, Runtime> runtimes_;

	std::thread thread_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool stop_requested_ = false;
	std::atomic<bool> running_{ false };
};

} // namespace opennova::novaworld_server::hosted
