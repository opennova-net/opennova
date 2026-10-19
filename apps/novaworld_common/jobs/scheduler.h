#pragma once

// The periodic jobs (job_queue.h): every period_ms of the wall clock, one job
// of each kind, keyed by its kind and the period's time bucket
// (<kind>@<now / period_ms>, the schedule key, unique for good), so however
// many schedulers tick, and however often, a bucket gets one job; and keyed
// by its kind as its dedupe key, so a job still queued or running when the
// next bucket comes is not joined by another. A kind marked at_start also gets
// one when the scheduler starts (the billing reconcile at a release's start),
// keyed by its start key where the scheduler has one (<kind>@start:<key>, a
// release's: every task of one release enqueues it once between them). A
// `wanted` test leaves a bucket with nothing to do unqueued (the billing sync
// with no customer waiting). Each program that runs jobs runs a scheduler over
// its own kinds alone (the worker's worker_schedule.h, the data program's data
// set prune), in every
// one of its processes, with no election: the keys keep the buckets single. It
// enqueues and runs nothing itself, and its program's pool runs what it
// enqueued. Service policy, not a port.

#include "job_queue.h"

#include <net/novaworld/db/database.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server::jobs {

struct PeriodicJob {
	std::string kind;
	int64_t period_ms = 60 * 1000;
	io::JsonValue payload = io::JsonValue::make_object();
	int max_attempts = 1;
	bool at_start = false;
	// Unset: every bucket gets one. Set: only a bucket for which it answers
	// true (it reads the database on the scheduler's connection).
	std::function<bool(db::Database &)> wanted;
};

class Scheduler {
public:
	using Clock = std::function<int64_t()>; // Unix milliseconds

	static int64_t wall_now_ms();

	// `pool` must outlive the scheduler. `start_key` keys the at_start jobs
	// (start_schedule_key); "" leaves them keyed by their kind alone.
	Scheduler(db::ConnectionPool &pool, std::vector<PeriodicJob> jobs, Clock now_ms = wall_now_ms,
			int64_t tick_ms = 1000, std::string start_key = {});
	~Scheduler();
	Scheduler(const Scheduler &) = delete;
	Scheduler &operator=(const Scheduler &) = delete;

	// The at_start jobs enqueued, then a tick every tick_ms on the scheduler's
	// own thread until stop().
	void start();
	void stop();

	// The at_start jobs at `now_ms`, each standing for its bucket then (start()
	// runs it; the tests by hand).
	void enqueue_at_start(db::Database &db, int64_t now_ms);
	// One tick at `now_ms`: each periodic job whose bucket has none yet; how
	// many were enqueued.
	size_t tick(db::Database &db, int64_t now_ms);

	// The schedule key of `kind` at `now_ms` with `period_ms`.
	static std::string schedule_key(const std::string &kind, int64_t now_ms, int64_t period_ms);
	// The schedule key of `kind`'s at_start job under start key `start_key`.
	static std::string start_schedule_key(const std::string &kind, const std::string &start_key);

	// The scheduler thread's ticks so far, a database error's included (the
	// stall watchdog's counter, core/health.h): it stands still only while a tick
	// hangs. Thread-safe.
	uint64_t turns() const { return turns_.load(std::memory_order_relaxed); }

private:
	void run();

	db::ConnectionPool &pool_;
	const std::vector<PeriodicJob> jobs_;
	const Clock now_;
	const int64_t tick_ms_;
	const std::string start_key_;
	// The bucket each kind was last enqueued (or found) for: a cache, so a tick
	// writes only when a bucket turns; the database's keys decide.
	std::map<std::string, int64_t> last_bucket_;
	std::atomic<uint64_t> turns_{ 0 };

	std::mutex mu_;
	std::condition_variable cv_;
	bool stopping_ = false;
	std::thread thread_;
};

} // namespace opennova::novaworld_server::jobs
