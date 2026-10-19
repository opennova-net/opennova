#include "worker_pool.h"

#include "core/loggable.h"

#include <base/os_random/os_random.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

namespace opennova::novaworld_server::jobs {

namespace {

std::string hex32(uint32_t value) {
	char buf[9];
	std::snprintf(buf, sizeof(buf), "%08x", value);
	return buf;
}

// The handler's answer, never an exception: one it throws is a retry.
JobResult run_handler(const JobKind &kind, db::ConnectionPool &pool, const ClaimedJob &job) {
	try {
		JobContext context{ pool, job };
		return kind.handler(context);
	} catch (const std::exception &e) {
		return JobResult::retry(e.what());
	}
}

// The job's end written; false when the claim was no longer its own (its lease
// ran out and it ran again elsewhere).
bool finish(db::ConnectionPool &pool, const JobKind &kind, const ClaimedJob &job,
		const JobResult &result) {
	auto lease = pool.acquire();
	db::Database &db = *lease;
	switch (result.kind) {
		case JobResult::Kind::Done:
			return complete(db, job);
		case JobResult::Kind::Fail:
			std::fprintf(stderr, "[jobs] WARN %s #%lld failed for good: %s\n", job.kind.c_str(),
					static_cast<long long>(job.id), loggable(result.error, 300).c_str());
			return fail(db, job, result.error);
		case JobResult::Kind::Retry: {
			const int64_t delay = result.delay_ms >= 0
					? result.delay_ms
					: retry_delay_ms(
							  job.attempts, kind.retry_base_ms, kind.retry_max_ms, os_random_u32());
			if (job.attempts >= job.max_attempts)
				std::fprintf(stderr,
						"[jobs] WARN %s #%lld failed at attempt %d of %d: %s; no attempt left\n",
						job.kind.c_str(), static_cast<long long>(job.id), job.attempts,
						job.max_attempts, loggable(result.error, 300).c_str());
			else
				std::fprintf(stderr,
						"[jobs] WARN %s #%lld failed at attempt %d of %d: %s; again in %lld s\n",
						job.kind.c_str(), static_cast<long long>(job.id), job.attempts,
						job.max_attempts, loggable(result.error, 300).c_str(),
						static_cast<long long>((delay + 999) / 1000));
			return retry(db, job, result.error, delay);
		}
	}
	return false;
}

} // namespace

void JobRegistry::add(JobKind kind) {
	const std::string name = kind.kind;
	kinds_[name] = std::move(kind);
}

const JobKind *JobRegistry::find(const std::string &kind) const {
	const auto it = kinds_.find(kind);
	return it == kinds_.end() ? nullptr : &it->second;
}

std::vector<std::string> JobRegistry::kinds() const {
	std::vector<std::string> out;
	for (const auto &entry : kinds_)
		out.push_back(entry.first);
	return out;
}

std::map<std::string, int> JobRegistry::caps() const {
	std::map<std::string, int> out;
	for (const auto &[name, kind] : kinds_) {
		if (kind.cap > 0)
			out[name] = kind.cap;
	}
	return out;
}

bool run_one(db::ConnectionPool &pool, const JobRegistry &registry, const std::string &worker_id,
		int64_t lease_ms, const std::function<void(const ClaimedJob &)> &on_claim,
		const std::function<void(const ClaimedJob &)> &on_end) {
	std::optional<ClaimedJob> job;
	{
		auto lease = pool.acquire();
		job = claim(*lease, worker_id, registry.kinds(), registry.caps(), lease_ms);
	}
	if (!job)
		return false;
	const JobKind *kind = registry.find(job->kind);
	if (on_claim)
		on_claim(*job);
	const JobResult result = kind != nullptr
			? run_handler(*kind, pool, *job)
			: JobResult::fail("no handler for the kind " + job->kind);
	try {
		static const JobKind kNone;
		if (!finish(pool, kind != nullptr ? *kind : kNone, *job, result))
			std::fprintf(stderr,
					"[jobs] WARN %s #%lld: its claim lapsed while it ran; it runs again\n",
					job->kind.c_str(), static_cast<long long>(job->id));
	} catch (const db::Error &e) {
		// Its lease runs out and it runs again: the handlers are idempotent.
		std::fprintf(stderr, "[jobs] WARN %s #%lld: its end could not be written: %s\n",
				job->kind.c_str(), static_cast<long long>(job->id), e.what());
	}
	if (on_end)
		on_end(*job);
	return true;
}

size_t run_due(db::ConnectionPool &pool, const JobRegistry &registry, const std::string &worker_id,
		size_t max) {
	size_t ran = 0;
	while (ran < max && run_one(pool, registry, worker_id))
		++ran;
	return ran;
}

WorkerPool::WorkerPool(
		db::ConnectionPool &pool, const JobRegistry &registry, WorkerPoolConfig config) :
		pool_(pool),
		registry_(registry),
		config_(std::move(config)),
		tag_(hex32(os_random_u32())) {}

WorkerPool::~WorkerPool() {
	stop();
}

void WorkerPool::start() {
	if (!threads_.empty())
		return;
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = false;
	}
	// The pool's room: each worker leases a connection around its job's database
	// work, the keeper one every turn, and the process holds one for its life (the
	// program's main loop). More workers than the rest would leave the keeper waiting
	// on the pool, the leases it extends lapsing under jobs that still run.
	const int room = std::max(static_cast<int>(pool_.max()) - 2, 1);
	const int threads = std::min(std::max(config_.threads, 1), room);
	if (threads < config_.threads)
		std::fprintf(stderr,
				"[jobs] WARN %d worker threads asked (ONNET_WORKER_THREADS), past the %zu "
				"connections of the database pool (DATABASE_POOL_MAX) less the keeper's and the "
				"process's own: %d\n",
				config_.threads, pool_.max(), threads);
	std::string kinds;
	for (const std::string &k : registry_.kinds())
		kinds += (kinds.empty() ? "" : ", ") + k;
	std::printf("[jobs] %d worker thread(s) %s-%s for %s\n", threads, config_.name.c_str(),
			tag_.c_str(), kinds.empty() ? "(no kind)" : kinds.c_str());
	for (int i = 0; i < threads; ++i)
		threads_.emplace_back([this, i] { worker_main(i); });
	keeper_ = std::thread([this] { keeper_main(); });
}

void WorkerPool::stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = true;
	}
	cv_.notify_all();
	for (std::thread &t : threads_) {
		if (t.joinable())
			t.join();
	}
	threads_.clear();
	if (keeper_.joinable())
		keeper_.join();
}

bool WorkerPool::wait_stop(int64_t ms) {
	std::unique_lock<std::mutex> lock(mu_);
	return cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping_; });
}

void WorkerPool::worker_main(int index) {
	const std::string id = config_.name + "-" + tag_ + "-" + std::to_string(index);
	int64_t wait = config_.poll_ms;
	std::string last_error;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(mu_);
			if (stopping_)
				return;
		}
		bool ran = false;
		try {
			ran = run_one(
					pool_, registry_, id, config_.lease_ms,
					[this, &id](const ClaimedJob &job) {
						std::lock_guard<std::mutex> lock(running_mu_);
						running_[job.id] = id;
					},
					[this](const ClaimedJob &job) {
						std::lock_guard<std::mutex> lock(running_mu_);
						running_.erase(job.id);
					});
			last_error.clear();
		} catch (const std::exception &e) {
			// The claim's database: logged once until it answers again.
			if (last_error != e.what())
				std::fprintf(stderr, "[jobs] WARN %s: %s\n", id.c_str(), e.what());
			last_error = e.what();
		}
		loop_turns_.fetch_add(1, std::memory_order_relaxed);
		if (ran) {
			jobs_run_.fetch_add(1, std::memory_order_relaxed);
			wait = config_.poll_ms;
			continue;
		}
		if (wait_stop(wait))
			return;
		wait = std::min(wait * 2, std::max(config_.idle_max_ms, config_.poll_ms));
	}
}

void WorkerPool::keeper_main() {
	while (!wait_stop(config_.keeper_ms)) {
		std::map<int64_t, std::string> running;
		{
			std::lock_guard<std::mutex> lock(running_mu_);
			running = running_;
		}
		try {
			auto lease = pool_.acquire();
			db::Database &db = *lease;
			for (const auto &[job, worker] : running)
				extend_leases(db, worker, { job }, config_.lease_ms);
			if (const size_t reaped = reap_expired(db); reaped > 0)
				std::fprintf(stderr, "[jobs] WARN %zu job(s) whose worker went away queued again\n",
						reaped);
		} catch (const std::exception &e) {
			std::fprintf(stderr, "[jobs] WARN the keeper: %s\n", e.what());
		}
		loop_turns_.fetch_add(1, std::memory_order_relaxed);
	}
}

} // namespace opennova::novaworld_server::jobs
