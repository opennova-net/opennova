// The service's job queue (apps/novaworld_common/jobs/job_queue.h, worker_pool.h,
// scheduler.h) on a migrated database of the test's own (common/test_database.h):
// a job enqueued inside a transaction commits or rolls back with it; the dedupe key
// holds while a job is queued or running and the schedule key for good; several
// workers claiming at once run each job once; a kind's cap holds across workers; a
// lease that ran out puts the job back (or fails it past its attempts) and the lapsed
// claim's finish changes nothing; a retry waits its backoff, jittered; a job's secret
// goes when it ends; the periodic buckets enqueue once each (the `wanted` test, the
// at_start jobs); finished jobs are pruned; and the worker pool's threads and keeper
// run what is due, a worker gone included, its threads within the database pool's
// room; a claim never waits on a hosted start (the claim lock is a key of its own).
// Compiles the job sources directly.

#include "jobs/job_queue.h"
#include "jobs/maintenance_jobs.h"
#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include "common/test_database.h"
#include "common/test_expect.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace jobs = opennova::novaworld_server::jobs;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;
using test_db::TestDatabase;

namespace {

jobs::NewJob job_of(const std::string &kind, const std::string &dedupe = {}, int max_attempts = 3) {
	jobs::NewJob j;
	j.kind = kind;
	j.dedupe_key = dedupe;
	j.max_attempts = max_attempts;
	return j;
}

std::string state_of(Database &db, int64_t id) {
	const auto rows = db.query("SELECT state FROM jobs WHERE id = $1;", { BindValue(id) });
	return rows.empty() ? std::string("<gone>") : rows.front().as_text(0).value_or("");
}

int64_t count_state(Database &db, const std::string &state) {
	return db.query("SELECT COUNT(*) FROM jobs WHERE state = $1;", { BindValue(state) })
			.front()
			.as_int(0)
			.value_or(0);
}

bool wait_until(const std::function<bool()> &done, int ms) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		if (done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return done();
}

// Enqueued in a transaction, a job is the transaction's: rolled back, none; committed,
// one. The dedupe key holds while one is queued or running, and frees once it ended;
// the schedule key holds for good.
int test_enqueue_and_keys() {
	TestDatabase scratch;
	Database db = scratch.open();
	{
		opennova::db::Transaction tx(db);
		TEST_EXPECT(jobs::enqueue(db, job_of("test.a")) > 0);
	}
	TEST_EXPECT(count_state(db, "queued") == 0);
	int64_t id = 0;
	{
		opennova::db::Transaction tx(db);
		id = jobs::enqueue(db, job_of("test.a", "a:1"));
		tx.commit();
	}
	TEST_EXPECT(id > 0 && state_of(db, id) == "queued");
	TEST_EXPECT(jobs::enqueue(db, job_of("test.a", "a:1")) == 0);
	TEST_EXPECT(jobs::job_pending(db, "a:1"));
	auto claimed = jobs::claim(db, "w1", { "test.a" }, {}, 60000);
	TEST_EXPECT(claimed && claimed->id == id && claimed->attempts == 1);
	TEST_EXPECT(jobs::enqueue(db, job_of("test.a", "a:1")) == 0); // running still holds it
	TEST_EXPECT(jobs::complete(db, *claimed));
	TEST_EXPECT(!jobs::job_pending(db, "a:1"));
	TEST_EXPECT(jobs::enqueue(db, job_of("test.a", "a:1")) > 0);
	jobs::NewJob periodic = job_of("test.p");
	periodic.schedule_key = "test.p@7";
	TEST_EXPECT(jobs::enqueue(db, periodic) > 0);
	auto p = jobs::claim(db, "w1", { "test.p" }, {}, 60000);
	TEST_EXPECT(p && jobs::complete(db, *p));
	TEST_EXPECT(jobs::enqueue(db, periodic) == 0); // its bucket had one
	// A job not yet due is not claimed; one of another kind neither.
	jobs::NewJob later = job_of("test.later");
	later.delay_ms = 60000;
	TEST_EXPECT(jobs::enqueue(db, later) > 0);
	TEST_EXPECT(!jobs::claim(db, "w1", { "test.later", "test.none" }, {}, 60000));
	return 0;
}

// Several workers claim at once, each on its own connection: every job runs exactly once.
int test_concurrent_claims() {
	TestDatabase scratch;
	ConnectionPool pool(scratch.options(), 16);
	constexpr int kJobs = 200;
	{
		auto db = pool.acquire();
		for (int i = 0; i < kJobs; ++i)
			TEST_EXPECT(jobs::enqueue(*db, job_of("test.many")) > 0);
	}
	std::mutex mu;
	std::vector<int64_t> ran;
	std::atomic<int> errors{ 0 };
	std::vector<std::thread> workers;
	for (int w = 0; w < 8; ++w) {
		workers.emplace_back([&, w] {
			const std::string id = "worker-" + std::to_string(w);
			for (;;) {
				auto lease = pool.acquire();
				std::optional<jobs::ClaimedJob> job;
				try {
					job = jobs::claim(*lease, id, { "test.many" }, {}, 60000);
				} catch (const opennova::db::Error &) {
					++errors;
					continue;
				}
				if (!job)
					return;
				if (!jobs::complete(*lease, *job))
					++errors;
				std::lock_guard<std::mutex> lock(mu);
				ran.push_back(job->id);
			}
		});
	}
	for (std::thread &t : workers)
		t.join();
	TEST_EXPECT(errors.load() == 0);
	TEST_EXPECT(ran.size() == static_cast<size_t>(kJobs));
	std::sort(ran.begin(), ran.end());
	TEST_EXPECT(std::adjacent_find(ran.begin(), ran.end()) == ran.end());
	TEST_EXPECT(count_state(*pool.acquire(), "done") == kJobs);
	return 0;
}

// A kind's cap holds across every worker: at most 2 of "test.capped" run at once while
// 8 workers have jobs of it; an uncapped kind beside it runs wider.
int test_caps() {
	TestDatabase scratch;
	ConnectionPool pool(scratch.options(), 16);
	std::atomic<int> capped_now{ 0 }, capped_max{ 0 }, free_now{ 0 }, free_max{ 0 };
	const auto track = [](std::atomic<int> &now, std::atomic<int> &max) {
		const int n = ++now;
		int seen = max.load();
		while (n > seen && !max.compare_exchange_weak(seen, n)) {
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		--now;
		return jobs::JobResult::done();
	};
	jobs::JobRegistry registry;
	registry.add({ "test.capped", [&](jobs::JobContext &) { return track(capped_now, capped_max); },
			2 });
	registry.add({ "test.free", [&](jobs::JobContext &) { return track(free_now, free_max); }, 0 });
	{
		auto db = pool.acquire();
		for (int i = 0; i < 12; ++i) {
			TEST_EXPECT(jobs::enqueue(*db, job_of("test.capped")) > 0);
			TEST_EXPECT(jobs::enqueue(*db, job_of("test.free")) > 0);
		}
	}
	jobs::WorkerPoolConfig config;
	config.threads = 8;
	config.poll_ms = 10;
	config.idle_max_ms = 20;
	jobs::WorkerPool workers(pool, registry, config);
	workers.start();
	TEST_EXPECT(wait_until([&] { return workers.jobs_run() >= 24; }, 20000));
	workers.stop();
	TEST_EXPECT(capped_max.load() >= 1 && capped_max.load() <= 2);
	TEST_EXPECT(free_max.load() > 2);
	TEST_EXPECT(count_state(*pool.acquire(), "done") == 24);
	return 0;
}

// A worker that went away: its lease runs out and the job is queued again (its attempt
// counted, why on the row), the next claim is its second attempt, and the gone worker's
// late finish changes nothing; past its attempts it fails. A live worker's lease is
// extended.
int test_leases() {
	TestDatabase scratch;
	Database db = scratch.open();
	jobs::NewJob job = job_of("test.lease", "lease:1", 2);
	job.secret = "s3cret";
	const int64_t id = jobs::enqueue(db, job);
	auto gone = jobs::claim(db, "gone-worker", { "test.lease" }, {}, 60000);
	TEST_EXPECT(gone && gone->id == id && gone->secret == "s3cret");
	TEST_EXPECT(jobs::reap_expired(db) == 0); // its lease still runs
	db.exec("UPDATE jobs SET lease_until = now() - interval '1 second' WHERE id = $1;",
			{ BindValue(id) });
	TEST_EXPECT(jobs::reap_expired(db) == 1);
	auto rows = db.query("SELECT state, attempts, worker_id IS NULL, last_error, secret FROM jobs "
						 "WHERE id = $1;",
			{ BindValue(id) });
	TEST_EXPECT(rows.front().as_text(0) == "queued" && rows.front().as_int(1) == 1 &&
			rows.front().as_int(2) == 1);
	TEST_EXPECT(rows.front().as_text(3)->find("lease") != std::string::npos &&
			rows.front().as_text(4) == "s3cret");
	TEST_EXPECT(jobs::job_pending(db, "lease:1"));
	auto again = jobs::claim(db, "live-worker", { "test.lease" }, {}, 1000);
	TEST_EXPECT(again && again->attempts == 2);
	TEST_EXPECT(!jobs::complete(db, *gone)); // the lapsed claim
	TEST_EXPECT(!jobs::retry(db, *gone, "late", 0));
	TEST_EXPECT(state_of(db, id) == "running");
	TEST_EXPECT(jobs::extend_leases(db, "live-worker", { id }, 60000) == 1);
	TEST_EXPECT(jobs::extend_leases(db, "gone-worker", { id }, 60000) == 0);
	TEST_EXPECT(db.query("SELECT 1 FROM jobs WHERE id = $1 AND lease_until > now() + interval '50 "
						 "seconds';",
						  { BindValue(id) })
						.size() == 1);
	// Gone again at its last attempt: failed, its secret dropped (an empty text
	// where it was, so the row still says it carried one).
	db.exec("UPDATE jobs SET lease_until = now() - interval '1 second' WHERE id = $1;",
			{ BindValue(id) });
	TEST_EXPECT(jobs::reap_expired(db) == 1);
	rows = db.query("SELECT state, secret = '', finished_at IS NOT NULL FROM jobs WHERE id = $1;",
			{ BindValue(id) });
	TEST_EXPECT(rows.front().as_text(0) == "failed" && rows.front().as_int(1) == 1 &&
			rows.front().as_int(2) == 1);
	TEST_EXPECT(!jobs::job_pending(db, "lease:1"));
	return 0;
}

// The backoff: attempt n waits base * 2^(n-1), capped, jittered to between half and
// all of it. A handler's retry queues the job again that far on; its last attempt's
// failure fails it; a fail is for good; done and failed jobs drop their secret.
int test_retries() {
	TEST_EXPECT(jobs::retry_delay_ms(1, 1000, 60000, 0) == 500);
	TEST_EXPECT(jobs::retry_delay_ms(1, 1000, 60000, 0xffffffffu) <= 1000);
	for (uint32_t draw : { 0u, 7u, 12345u, 0xdeadbeefu }) {
		const int64_t d3 = jobs::retry_delay_ms(3, 1000, 60000, draw);
		TEST_EXPECT(d3 >= 2000 && d3 <= 4000);
		const int64_t d20 = jobs::retry_delay_ms(20, 1000, 60000, draw);
		TEST_EXPECT(d20 >= 30000 && d20 <= 60000);
	}
	TestDatabase scratch;
	ConnectionPool pool(scratch.options(), 4);
	int calls = 0;
	jobs::JobRegistry registry;
	jobs::JobKind flaky{ "test.flaky",
		[&calls](jobs::JobContext &ctx) {
			++calls;
			return ctx.job.attempts < 3 ? jobs::JobResult::retry("not yet", 0)
										: jobs::JobResult::done();
		},
		0 };
	registry.add(flaky);
	jobs::JobKind slow{ "test.backoff",
		[](jobs::JobContext &) { return jobs::JobResult::retry("try later"); }, 0 };
	slow.retry_base_ms = 20000;
	slow.retry_max_ms = 20000;
	registry.add(slow);
	registry.add(
			{ "test.fail", [](jobs::JobContext &) { return jobs::JobResult::fail("never"); }, 0 });
	registry.add({ "test.throws",
			[](jobs::JobContext &) -> jobs::JobResult { throw std::runtime_error("boom"); }, 0 });
	int64_t flaky_id = 0, backoff_id = 0, fail_id = 0, throws_id = 0;
	{
		auto db = pool.acquire();
		jobs::NewJob j = job_of("test.flaky", "", 5);
		j.secret = "kept until it ends";
		flaky_id = jobs::enqueue(*db, j);
		backoff_id = jobs::enqueue(*db, job_of("test.backoff", "", 3));
		fail_id = jobs::enqueue(*db, job_of("test.fail", "", 5));
		throws_id = jobs::enqueue(*db, job_of("test.throws", "", 1));
	}
	jobs::run_due(pool, registry);
	auto db = pool.acquire();
	TEST_EXPECT(calls == 3 && state_of(*db, flaky_id) == "done");
	TEST_EXPECT(db->query("SELECT 1 FROM jobs WHERE id = $1 AND secret = '' AND attempts = 3;",
						  { BindValue(flaky_id) })
						.size() == 1);
	// A job that never carried a secret keeps none.
	TEST_EXPECT(db->query("SELECT 1 FROM jobs WHERE id = $1 AND secret IS NULL;",
						  { BindValue(throws_id) })
						.size() == 1);
	// Its backoff: between 10 s and 20 s on.
	TEST_EXPECT(state_of(*db, backoff_id) == "queued");
	TEST_EXPECT(db->query("SELECT 1 FROM jobs WHERE id = $1 AND run_at > now() + interval '9 "
						  "seconds' AND "
						  "run_at < now() + interval '21 seconds' AND last_error = 'try later';",
						  { BindValue(backoff_id) })
						.size() == 1);
	TEST_EXPECT(state_of(*db, fail_id) == "failed");
	TEST_EXPECT(db->query("SELECT attempts FROM jobs WHERE id = $1;", { BindValue(fail_id) })
						.front()
						.as_int(0) == 1);
	// An exception is a retry: its one attempt used, failed.
	TEST_EXPECT(state_of(*db, throws_id) == "failed");
	TEST_EXPECT(db->query("SELECT last_error FROM jobs WHERE id = $1;", { BindValue(throws_id) })
						.front()
						.as_text(0) == "boom");
	return 0;
}

// The periodic buckets: a tick enqueues each kind's job once per bucket, none beside
// one still queued (its bucket's own follows once it ended), none for a bucket `wanted`
// declines; an at_start job stands for its bucket; two schedulers (two controllers in
// turn) never give a bucket two jobs.
int test_scheduler() {
	TestDatabase scratch;
	ConnectionPool pool(scratch.options(), 4);
	bool wanted = false;
	std::vector<jobs::PeriodicJob> periodic(3);
	periodic[0].kind = "test.every";
	periodic[0].period_ms = 1000;
	periodic[1].kind = "test.wanted";
	periodic[1].period_ms = 1000;
	periodic[1].wanted = [&wanted](Database &) { return wanted; };
	periodic[2].kind = "test.start";
	periodic[2].period_ms = 60000;
	periodic[2].at_start = true;
	jobs::Scheduler scheduler(pool, periodic);
	auto lease = pool.acquire();
	Database &db = *lease;
	scheduler.enqueue_at_start(db, 5000);
	TEST_EXPECT(
			scheduler.tick(db, 5000) == 1); // test.every (test.start's bucket has its start job)
	TEST_EXPECT(scheduler.tick(db, 5900) == 0);
	TEST_EXPECT(scheduler.tick(db, 6000) == 0); // test.every's still queued
	auto every = jobs::claim(db, "w", { "test.every" }, {}, 60000);
	TEST_EXPECT(every && jobs::complete(db, *every));
	TEST_EXPECT(scheduler.tick(db, 6100) == 1); // bucket 6 gets its own now
	wanted = true;
	TEST_EXPECT(scheduler.tick(db, 6200) == 1); // test.wanted, asked again each tick
	TEST_EXPECT(
			db.query("SELECT COUNT(*) FROM jobs WHERE kind = 'test.start';").front().as_int(0) ==
			1);
	TEST_EXPECT(jobs::Scheduler::schedule_key("k", 6200, 1000) == "k@6");
	// Another scheduler at the same time: nothing more.
	jobs::Scheduler second(pool, periodic);
	for (const char *kind : { "test.every", "test.wanted" }) {
		auto j = jobs::claim(db, "w", { kind }, {}, 60000);
		TEST_EXPECT(j && jobs::complete(db, *j));
	}
	TEST_EXPECT(second.tick(db, 6300) == 0);
	TEST_EXPECT(second.tick(db, 7000) == 2);
	return 0;
}

// The claim's lock is its own (database.h's keys): a hosted start holding the
// hosted-start lock for its transaction keeps no claim waiting (one that
// shared the key would wait out its lock timeout and fail), and no other
// one-key lock shares the claim's key.
int test_claim_lock_apart() {
	TestDatabase scratch;
	ConnectionPool pool(scratch.options(), 4);
	auto start = pool.acquire();
	auto worker = pool.acquire();
	const int64_t id = jobs::enqueue(*worker, job_of("test.apart"));
	start->begin();
	start->exec("SELECT pg_advisory_xact_lock($1);", { BindValue(opennova::db::kHostedStartLock) });
	std::optional<jobs::ClaimedJob> claimed;
	try {
		claimed = jobs::claim(*worker, "w", { "test.apart" }, {}, 60000);
	} catch (const opennova::db::Error &e) {
		std::fprintf(stderr, "  the claim waited on the hosted start: %s\n", e.what());
	}
	start->rollback();
	TEST_EXPECT(claimed && claimed->id == id);
	for (const int64_t other : { opennova::db::kMigrationLock, opennova::db::kHostedStartLock,
				 opennova::db::kControllerLock })
		TEST_EXPECT(other != opennova::db::kJobsClaimLock);
	return 0;
}

// Finished jobs past their keep are pruned (the jobs.prune job), the live ones never.
int test_prune() {
	TestDatabase scratch;
	ConnectionPool pool(scratch.options(), 4);
	jobs::JobRegistry registry;
	jobs::register_maintenance_jobs(registry, {});
	{
		auto db = pool.acquire();
		for (int i = 0; i < 3; ++i)
			jobs::enqueue(*db, job_of("test.old"));
		jobs::enqueue(*db, job_of("test.live"));
		for (int i = 0; i < 3; ++i) {
			auto j = jobs::claim(*db, "w", { "test.old" }, {}, 60000);
			TEST_EXPECT(j && jobs::complete(*db, *j));
		}
		db->exec("UPDATE jobs SET finished_at = now() - interval '15 days' WHERE kind = "
				 "'test.old';");
		TEST_EXPECT(jobs::enqueue(*db, job_of(jobs::kJobJobsPrune)) > 0);
		TEST_EXPECT(jobs::enqueue(*db, job_of(jobs::kJobHostsPrune)) > 0);
		TEST_EXPECT(jobs::enqueue(*db, job_of(jobs::kJobHostKeysPrune)) > 0);
	}
	TEST_EXPECT(jobs::run_due(pool, registry) == 3);
	auto db = pool.acquire();
	TEST_EXPECT(db->query("SELECT 1 FROM jobs WHERE kind = 'test.old';").empty());
	TEST_EXPECT(
			db->query("SELECT 1 FROM jobs WHERE kind = 'test.live' AND state = 'queued';").size() ==
			1);
	TEST_EXPECT(jobs::prune_finished(*db, 0) == 3); // the prunes' own, done just now

	// An admin's prune names its keep in its payload, a day at the least: one
	// finished two days ago goes, one finished two hours ago stays.
	for (int i = 0; i < 2; ++i) {
		jobs::enqueue(*db, job_of("test.kept"));
		auto j = jobs::claim(*db, "w", { "test.kept" }, {}, 60000);
		TEST_EXPECT(j && jobs::complete(*db, *j));
	}
	db->exec("UPDATE jobs SET finished_at = now() - CASE WHEN id = (SELECT MIN(id) FROM jobs "
			 "WHERE kind = 'test.kept') THEN interval '2 days' ELSE interval '2 hours' END "
			 "WHERE kind = 'test.kept';");
	jobs::NewJob keep = job_of(jobs::kJobJobsPrune);
	keep.payload.set(jobs::kPruneKeepKey, opennova::io::json_number(60)); // under a day: a day
	TEST_EXPECT(jobs::enqueue(*db, keep) > 0);
	TEST_EXPECT(jobs::run_due(pool, registry) == 1);
	TEST_EXPECT(db->query("SELECT 1 FROM jobs WHERE kind = 'test.kept';").size() == 1);
	return 0;
}

// The pool: its workers run what is due, its keeper puts back a job whose worker went
// away (claimed, its lease run out) and a worker runs it again; stop() joins them.
int test_pool_and_keeper() {
	TestDatabase scratch;
	ConnectionPool pool(scratch.options(), 8);
	std::atomic<int> ran{ 0 };
	jobs::JobRegistry registry;
	registry.add({ "test.work",
			[&ran](jobs::JobContext &ctx) {
				// A handler leases its own connection for its database work.
				auto lease = ctx.pool.acquire();
				lease->query("SELECT 1;");
				++ran;
				return jobs::JobResult::done();
			},
			0 });
	int64_t orphan = 0;
	{
		auto db = pool.acquire();
		orphan = jobs::enqueue(*db, job_of("test.work"));
		auto gone = jobs::claim(*db, "gone-worker", { "test.work" }, {}, 100); // its lease: 0.1 s
		TEST_EXPECT(gone && gone->id == orphan);
		for (int i = 0; i < 5; ++i)
			jobs::enqueue(*db, job_of("test.work"));
	}
	jobs::WorkerPoolConfig config;
	config.threads = 3;
	config.poll_ms = 20;
	config.idle_max_ms = 40;
	config.keeper_ms = 50;
	config.lease_ms = 5000;
	jobs::WorkerPool workers(pool, registry, config);
	workers.start();
	TEST_EXPECT(wait_until([&] { return ran.load() == 6; }, 10000));
	workers.stop();
	auto db = pool.acquire();
	TEST_EXPECT(state_of(*db, orphan) == "done");
	TEST_EXPECT(db->query("SELECT attempts FROM jobs WHERE id = $1;", { BindValue(orphan) })
						.front()
						.as_int(0) == 2);
	TEST_EXPECT(count_state(*db, "done") == 6);
	return 0;
}

// The workers fit the database pool: at most its max less two (the keeper's
// connection and the one the process holds for its life), at least one, so the
// keeper's acquire never waits behind every worker.
int test_threads_within_pool() {
	TestDatabase scratch;
	jobs::JobRegistry registry;
	registry.add({ "test.none", [](jobs::JobContext &) { return jobs::JobResult::done(); }, 0 });
	jobs::WorkerPoolConfig config;
	config.threads = 8;
	ConnectionPool four(scratch.options(), 4);
	jobs::WorkerPool capped(four, registry, config);
	TEST_EXPECT(capped.threads() == 0);
	capped.start();
	TEST_EXPECT(capped.threads() == 2);
	capped.stop();
	ConnectionPool two(scratch.options(), 2);
	jobs::WorkerPool one(two, registry, config);
	one.start();
	TEST_EXPECT(one.threads() == 1);
	one.stop();
	ConnectionPool sixteen(scratch.options(), 16);
	jobs::WorkerPool asked(sixteen, registry, config);
	asked.start();
	TEST_EXPECT(asked.threads() == 8);
	asked.stop();
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "enqueue_and_keys", test_enqueue_and_keys },
		{ "concurrent_claims", test_concurrent_claims },
		{ "caps", test_caps },
		{ "leases", test_leases },
		{ "retries", test_retries },
		{ "scheduler", test_scheduler },
		{ "claim_lock_apart", test_claim_lock_apart },
		{ "prune", test_prune },
		{ "pool_and_keeper", test_pool_and_keeper },
		{ "threads_within_pool", test_threads_within_pool },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the job queue\n");
	return 0;
}
