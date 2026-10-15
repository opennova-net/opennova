#pragma once

// The service's health, as its operators and its container runtime read it
// (service policy, not a port):
//
// - The stall watchdog. The loops that must keep turning (the gate's receive
//   loop and the NovaWorld UDP listener's, which wake at least every 250 ms
//   with nothing to read, and the hosted servers' supervisor pass, which runs
//   every second) each count their turns; main()'s loop samples the counters
//   every tick (check()). A loop whose count has not moved for kStallMs is
//   stalled, and so is main()'s own loop when its last check is that old: the
//   process logs which and exits 70, and the container runtime starts it
//   again. GET /healthz (outside /api, http_listener.cpp) answers "ok", or
//   503 "stalled" while healthy() is false, with no detail.
// - The heartbeat: one JSON line on stdout every kHeartbeatMs,
//   {"heartbeat":{...}}, which the log pipeline's metric filters read
//   (infra/aws observability): the NovaWorld sessions, the listed servers, the
//   hosted servers live and failed since the last line, the machines lost and
//   launch failures since the last line, the age of the oldest webhook event
//   the billing worker has not synced yet (-1 with billing off) and the
//   webhook deliveries refused since the last line.
//
// Crow-free and database-free; thread-safe.

#include <cstdint>
#include <functional>
#include <mutex>
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
	// bumps), from main()'s thread and a request's. Before the first check().
	void watch(std::string name, std::function<uint64_t()> progress);

	// One sample, from main()'s loop: each loop's counter is read, and one that
	// moved restarts its clock. The name of the first loop stalled for the
	// stall time ("" when none); a stalled loop stays stalled until it moves.
	std::string check();

	// Whether the process is healthy now: every watched loop moved within the
	// stall time, and main()'s last check() is no older (before the first
	// check, the clock starts at construction). For GET /healthz.
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
// deltas.
struct Heartbeat {
	int64_t nw_sessions = 0;
	int64_t listed_hosts = 0;
	int64_t hosted_live = 0;
	int64_t hosted_failed = 0;
	int64_t machines_lost = 0;
	int64_t launch_failures = 0;
	int64_t billing_sync_age_s = -1;
	int64_t webhook_rejects = 0;
};

// How often main() writes the heartbeat line.
inline constexpr uint64_t kHeartbeatMs = 60 * 1000;

// The heartbeat line, without its newline: {"heartbeat":{"nw_sessions":N,...}}.
std::string heartbeat_line(const Heartbeat &beat);

} // namespace opennova::novaworld_server
