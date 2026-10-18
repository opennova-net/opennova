#pragma once

// The service's health, as its operators and its container runtime read it
// (service policy, not a port):
//
// - The stall watchdog, one per process, over its program's loops alone
//   (program.h): the gate's receive loop and the lobby's NovaWorld UDP receive
//   loop (each wakes at least every 250 ms with nothing to read); the
//   fleet's supervisor pass (every second; it never calls the cloud, its
//   driver's calls being jobs) and job scheduler (every second), or its
//   election's turns while it waits as a standby; the worker's job pool (its
//   workers' claims and its keeper; a slow job occupies its worker alone, so
//   only a pool whose every thread stands still stalls); and in every program
//   its main loop (its tick, its heartbeat). Each loop counts its turns, and a
//   thread of the watchdog's own samples the counters every second (check(),
//   program.cpp). A loop whose count has not moved for kStallMs is stalled,
//   and so is the process when the last check is that old: it logs which and
//   exits 70, and the container runtime starts it again, the other programs'
//   processes untouched. GET /livez answers from it, with no other detail:
//   "ok", or 503 "stalled" while healthy() is false (the container's health
//   check; health_listener.h on ONNET_HEALTH_PORT for the gate, the lobby, the
//   fleet and the worker, http_listener.cpp on web's own port). Web's
//   GET /healthz, whether it serves a retail client (the load balancer's), is
//   the same, else 503 "redis unreachable" while Redis does not answer its PING.
// - The heartbeat: one JSON line on stdout every kHeartbeatMs,
//   {"heartbeat":{"program":"<program>",...}}, which the log pipeline's metric
//   filters read (infra/aws observability), each program logging the fields it
//   knows (Heartbeat): the NovaWorld sessions and the listed servers (lobby);
//   the webhook deliveries refused since the last line (web, billing on); the
//   hosted servers live and failed since the last line, the machines lost,
//   the launch failures since the last line and the limits hold (fleet,
//   hosting on); the age of the oldest webhook event the billing sync has not
//   synced yet, -1 with billing off (worker); the probes left unanswered for
//   want of a lobby since the last line and the lobby instances that take
//   clients (gate: a gate with no lobby to hand out answers no one while its
//   loops turn and its /livez says ok); and every program's database
//   statements that failed on a lock since the last line (a lock timeout or a
//   deadlock, db::lock_failures()).
//
// Crow-free and database-free; thread-safe.

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server {

class Watchdog {
public:
	// How long a loop may make no progress before the process ends.
	static constexpr uint64_t kStallMs = 30 * 1000;
	// The exit code of a stalled process.
	static constexpr int kStallExitCode = 70;

	using Clock = std::function<uint64_t()>;

	// `clock` is a steady clock in milliseconds (steady_now_ms by default; the
	// tests step one by hand).
	explicit Watchdog(uint64_t stall_ms = kStallMs, Clock clock = steady_now_ms);

	// A loop to watch: `progress` reads its turn counter (an atomic the loop
	// bumps), from the watchdog's thread and a request's. Before the first
	// check().
	void watch(std::string name, std::function<uint64_t()> progress);

	// One sample, from the watchdog's thread: each loop's counter is read, and
	// one that moved restarts its clock. The name of the first loop stalled for
	// the stall time ("" when none); a stalled loop stays stalled until it moves.
	std::string check();

	// Whether the process is healthy now: every watched loop moved within the
	// stall time, and the last check() is no older (before the first check,
	// the clock starts at construction). For GET /livez and /healthz.
	bool healthy() const;

	uint64_t stall_ms() const { return stall_ms_; }

	static uint64_t steady_now_ms();

private:
	struct Loop {
		std::string name;
		std::function<uint64_t()> progress;
		uint64_t last_count = 0;
		uint64_t moved_ms = 0;
	};

	const uint64_t stall_ms_;
	const Clock clock_;
	mutable std::mutex mu_;
	std::vector<Loop> loops_;
	uint64_t last_check_ms_ = 0;
};

// The heartbeat's fields; the "since the last line" counts are the caller's
// deltas. A field left unset is one the program does not know, and the line
// leaves it out (a metric aggregated over the programs counts each once).
struct Heartbeat {
	std::string program; // the process's program (program.h)
	std::optional<int64_t> nw_sessions;
	std::optional<int64_t> listed_hosts;
	std::optional<int64_t> hosted_live;
	std::optional<int64_t> hosted_failed;
	std::optional<int64_t> machines_lost;
	std::optional<int64_t> machines_held; // the lost machines the mass-lost brake holds (fleet)
	std::optional<int64_t> launch_failures;
	std::optional<int64_t> billing_sync_age_s; // -1 with billing off
	std::optional<int64_t> webhook_rejects;
	std::optional<int64_t>
			limits_held; // 1 while the hosted servers' limits hold is on (hosted/limits_hold.h)
	std::optional<int64_t> probes_unanswered; // the gate's, since the last line
	std::optional<int64_t> lobbies_live; // the lobby instances that take clients (gate)
	int64_t db_lock_timeouts = 0;
	int64_t db_deadlocks = 0;
};

// How often a program's main loop writes the heartbeat line.
inline constexpr uint64_t kHeartbeatMs = 60 * 1000;

// The heartbeat line, without its newline:
// {"heartbeat":{"program":"lobby","nw_sessions":N,...,"db_deadlocks":N}}, the
// fields set in Heartbeat's order.
std::string heartbeat_line(const Heartbeat &beat);

} // namespace opennova::novaworld_server
