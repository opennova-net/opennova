#pragma once

// The job queue's workers (job_queue.h): the handlers by kind, and a pool of
// threads that claim due jobs and run them. A worker holds no pooled
// connection while its handler runs: it leases one to claim, the handler
// leases its own around its database work (never across a slow call: the AWS
// CLI, Stripe, SES), and it leases one again to finish. So a slow job only
// occupies its worker, and no watched loop (core/health.h) waits on one. An idle
// worker polls every poll_ms, its wait doubling to idle_max_ms while nothing
// is due; no LISTEN/NOTIFY. A keeper thread extends the leases of the jobs the
// pool runs and puts back the jobs whose lease ran out (a worker that went
// away). The pool is the worker program's (apps/novaworld_worker), in as many
// processes as the deployment runs. Service policy, not a port.

#include "job_queue.h"

#include <net/novaworld/db/database.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opennova::novaworld_server::jobs {

// What a handler answers.
struct JobResult {
	enum class Kind : uint8_t {
		Done, // it ran
		Retry, // again later (the kind's backoff, or delay_ms), until its attempts are used up
		Fail, // failed for good
	};
	Kind kind = Kind::Done;
	std::string error;
	int64_t delay_ms = -1; // a retry's wait; -1: the kind's backoff

	static JobResult done() { return JobResult{}; }
	static JobResult retry(std::string error, int64_t delay_ms = -1) {
		return JobResult{ Kind::Retry, std::move(error), delay_ms };
	}
	static JobResult fail(std::string error) {
		return JobResult{ Kind::Fail, std::move(error), -1 };
	}
};

// What a handler runs with: its job, and the pool it leases its connections
// from (the calling thread holds none).
struct JobContext {
	db::ConnectionPool &pool;
	const ClaimedJob &job;
};

// A handler throws only what it cannot answer: an exception is a retry.
using JobHandler = std::function<JobResult(JobContext &)>;

struct JobKind {
	std::string kind;
	JobHandler handler;
	// The most jobs of the kind running at once across every worker of every
	// process (0: no cap), e.g. the EC2 calls under the API's rate limits.
	int cap = 0;
	// A retry's backoff: retry_base_ms doubling per attempt to retry_max_ms,
	// with a jitter (job_queue.h retry_delay_ms).
	int64_t retry_base_ms = 5 * 1000;
	int64_t retry_max_ms = 10 * 60 * 1000;
};

// The handlers a worker runs, by kind. Filled before the pool starts.
class JobRegistry {
public:
	void add(JobKind kind);
	const JobKind *find(const std::string &kind) const;
	std::vector<std::string> kinds() const;
	std::map<std::string, int> caps() const;
	bool empty() const { return kinds_.empty(); }

private:
	std::map<std::string, JobKind> kinds_;
};

struct WorkerPoolConfig {
	// ONNET_WORKER_THREADS; at most the database pool's max less two (the
	// keeper's connection and the one its process holds for its life), so the
	// keeper always gets one: start() runs that many, with a warning.
	int threads = 4;
	int64_t poll_ms = 500;
	int64_t idle_max_ms = 2000;
	int64_t lease_ms = 60 * 1000;
	int64_t keeper_ms = 10 * 1000;
	std::string name = "worker"; // the worker ids' prefix
};

// One due job of `registry`'s kinds claimed for `worker_id`, run on the
// calling thread (which holds no lease) and finished; false when none was due.
// `on_claim` and `on_end` (may be empty) see the claim and its end (the pool's
// keeper extends the lease between them).
bool run_one(db::ConnectionPool &pool, const JobRegistry &registry, const std::string &worker_id,
		int64_t lease_ms = 60 * 1000, const std::function<void(const ClaimedJob &)> &on_claim = {},
		const std::function<void(const ClaimedJob &)> &on_end = {});
// Due jobs run one after another until none is, `max` at most (the tests'
// worker); how many ran.
size_t run_due(db::ConnectionPool &pool, const JobRegistry &registry,
		const std::string &worker_id = "test", size_t max = 1000);

class WorkerPool {
public:
	// `pool` and `registry` must outlive the workers.
	WorkerPool(db::ConnectionPool &pool, const JobRegistry &registry, WorkerPoolConfig config);
	~WorkerPool();
	WorkerPool(const WorkerPool &) = delete;
	WorkerPool &operator=(const WorkerPool &) = delete;

	void start();
	// Each worker ends once its job has; then the keeper. Idempotent.
	void stop();

	// The jobs run so far (finished, whatever the outcome).
	uint64_t jobs_run() const { return jobs_run_.load(std::memory_order_relaxed); }
	// The claim loop's turns so far: each worker's claim or idle wait, and
	// each keeper pass (the stall watchdog's counter, core/health.h). A worker
	// running a slow job adds none while the others and the keeper do, so it
	// stands still only while every thread of the pool does. Thread-safe.
	uint64_t loop_turns() const { return loop_turns_.load(std::memory_order_relaxed); }
	const WorkerPoolConfig &config() const { return config_; }
	// The worker threads start() runs (WorkerPoolConfig::threads, within the
	// pool's room); 0 before it. The owner's thread's, as start() and stop().
	int threads() const { return static_cast<int>(threads_.size()); }

private:
	void worker_main(int index);
	void keeper_main();
	// Waits up to `ms`; true once the pool is stopping.
	bool wait_stop(int64_t ms);

	db::ConnectionPool &pool_;
	const JobRegistry &registry_;
	const WorkerPoolConfig config_;
	const std::string tag_; // this pool's: the worker ids' middle part

	std::mutex mu_;
	std::condition_variable cv_;
	bool stopping_ = false;
	std::vector<std::thread> threads_;
	std::thread keeper_;

	std::mutex running_mu_;
	std::map<int64_t, std::string> running_; // job id -> its worker's id
	std::atomic<uint64_t> jobs_run_{ 0 };
	std::atomic<uint64_t> loop_turns_{ 0 };
};

} // namespace opennova::novaworld_server::jobs
