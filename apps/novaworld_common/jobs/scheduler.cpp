#include "scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

namespace opennova::novaworld_server::jobs {

int64_t Scheduler::wall_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

Scheduler::Scheduler(
		db::ConnectionPool &pool, std::vector<PeriodicJob> jobs, Clock now_ms, int64_t tick_ms) :
		pool_(pool),
		jobs_(std::move(jobs)),
		now_(std::move(now_ms)),
		tick_ms_(std::max<int64_t>(tick_ms, 1)) {}

Scheduler::~Scheduler() {
	stop();
}

std::string Scheduler::schedule_key(const std::string &kind, int64_t now_ms, int64_t period_ms) {
	return kind + "@" + std::to_string(now_ms / std::max<int64_t>(period_ms, 1));
}

void Scheduler::enqueue_at_start(db::Database &db, int64_t now_ms) {
	for (const PeriodicJob &p : jobs_) {
		if (!p.at_start)
			continue;
		NewJob job;
		job.kind = p.kind;
		job.payload = p.payload;
		job.dedupe_key = p.kind;
		job.max_attempts = p.max_attempts;
		// It stands for this bucket: this scheduler adds none beside it.
		if (enqueue(db, job) != 0)
			last_bucket_[p.kind] = now_ms / std::max<int64_t>(p.period_ms, 1);
	}
}

size_t Scheduler::tick(db::Database &db, int64_t now_ms) {
	if (guard_ && !guard_(db))
		return 0;
	size_t enqueued = 0;
	for (const PeriodicJob &p : jobs_) {
		const int64_t period = std::max<int64_t>(p.period_ms, 1);
		const int64_t bucket = now_ms / period;
		const auto last = last_bucket_.find(p.kind);
		if (last != last_bucket_.end() && last->second == bucket)
			continue;
		if (p.wanted && !p.wanted(db))
			continue; // asked again next tick
		NewJob job;
		job.kind = p.kind;
		job.payload = p.payload;
		job.dedupe_key = p.kind;
		job.schedule_key = schedule_key(p.kind, now_ms, period);
		job.max_attempts = p.max_attempts;
		if (enqueue(db, job) != 0) {
			++enqueued;
			last_bucket_[p.kind] = bucket;
		} else if (job_pending(db, p.kind)) {
			// One still queued or running: the bucket is asked again next tick, and
			// gets its own once that one ended.
		} else {
			last_bucket_[p.kind] = bucket; // the bucket has its job already
		}
	}
	return enqueued;
}

void Scheduler::start() {
	if (thread_.joinable())
		return;
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = false;
	}
	try {
		auto lease = pool_.acquire();
		if (!guard_ || guard_(*lease))
			enqueue_at_start(*lease, now_());
	} catch (const std::exception &e) {
		std::fprintf(stderr, "[jobs] WARN the scheduler's start: %s\n", e.what());
	}
	thread_ = std::thread([this] { run(); });
}

void Scheduler::stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		stopping_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void Scheduler::run() {
	std::string last_error;
	for (;;) {
		try {
			auto lease = pool_.acquire();
			tick(*lease, now_());
			last_error.clear();
		} catch (const std::exception &e) {
			if (last_error != e.what())
				std::fprintf(stderr, "[jobs] WARN the scheduler: %s\n", e.what());
			last_error = e.what();
		}
		turns_.fetch_add(1, std::memory_order_relaxed);
		std::unique_lock<std::mutex> lock(mu_);
		if (cv_.wait_for(lock, std::chrono::milliseconds(tick_ms_), [this] { return stopping_; }))
			return;
	}
}

} // namespace opennova::novaworld_server::jobs
