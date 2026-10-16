#pragma once

// The service's background jobs in the database (backend/migrations/0007_jobs.sql):
// a queue in the river/Oban/good_job pattern, so the work no request or pass
// waits on (the fleet's cloud calls, the billing sync, the outbound mail, the
// prunes) is written with the change that causes it and outlives a restart of
// whatever wrote it. Service policy, not a port.
//
//   enqueue    on the caller's connection, so inside its open db::Transaction
//              the job commits (or rolls back) with the state change beside
//              it. A dedupe key is unique among the queued and running jobs
//              (a second one is no job: enqueue answers 0); a schedule key is
//              unique for good (a periodic job's kind and time bucket).
//   claim      one due job of the kinds a worker handles, under the claim lock
//              (db::kJobsClaimLock, an advisory lock of its own in
//              database.h's list, shared with no other use): the kinds at
//              their concurrency cap are left out, then
//              UPDATE ... WHERE id = (SELECT ... FOR UPDATE SKIP LOCKED LIMIT 1).
//              The job runs, its attempt counted, under a lease the worker
//              extends while it runs.
//   finish     complete (done), retry (queued again at a backoff, or failed
//              once its attempts are used up) or fail (for good); each a
//              compare-and-set on the claim (its worker and attempt), so a
//              worker whose lease ran out and whose job ran again elsewhere
//              changes nothing. A job that ends drops its secret (an empty
//              text where it held one, so its row still says it carried one,
//              never what).
//   reap       a running job whose lease ran out (its worker went away) is
//              queued again, or failed once its attempts are used up.
//   prune      finished jobs past their keep.
//
// Each function throws db::Error on a database failure.

#include <base/io/json.h>
#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::jobs {

// How long finished jobs are kept.
inline constexpr int64_t kFinishedKeepS = 14 * 24 * 60 * 60;

enum class JobState : uint8_t { Queued, Running, Done, Failed };
const char *job_state_name(JobState state);

struct NewJob {
	std::string kind;
	io::JsonValue payload = io::JsonValue::make_object();
	// What the payload must not keep: cleared when the job ends ("" for none).
	std::string secret;
	std::string dedupe_key; // "" for none
	std::string schedule_key; // "" for none
	int64_t delay_ms = 0; // run_at = now + delay
	int max_attempts = 5;
};

// Enqueues `job` on `db` (in its open transaction, when one is). Its id; 0
// when a queued or running job holds its dedupe key, or a job its schedule key.
int64_t enqueue(db::Database &db, const NewJob &job);

// A job a worker claimed.
struct ClaimedJob {
	int64_t id = 0;
	std::string kind;
	io::JsonValue payload;
	std::string secret;
	int attempts = 0; // this run's, counted
	int max_attempts = 1;
	std::string worker_id;
};

// The oldest due job of `kinds` whose kind is under its cap in `caps` (a kind
// absent from it, or capped at 0, is unlimited), claimed for `worker_id`
// under a lease of `lease_ms`; nullopt when none is due.
std::optional<ClaimedJob> claim(db::Database &db, const std::string &worker_id,
		const std::vector<std::string> &kinds, const std::map<std::string, int> &caps,
		int64_t lease_ms);

// The claim's end. Each is false when the claim is no longer the job's (its
// lease ran out and it was queued again).
bool complete(db::Database &db, const ClaimedJob &job);
// Queued again `delay_ms` on with `error`, or failed when its attempts are
// used up.
bool retry(db::Database &db, const ClaimedJob &job, const std::string &error, int64_t delay_ms);
bool fail(db::Database &db, const ClaimedJob &job, const std::string &error);

// The leases of `ids` held by `worker_id` run `lease_ms` from now; how many.
size_t extend_leases(db::Database &db, const std::string &worker_id,
		const std::vector<int64_t> &ids, int64_t lease_ms);
// The running jobs whose lease ran out: queued again at once, or failed once
// their attempts are used up; how many.
size_t reap_expired(db::Database &db);
// The jobs that finished more than `keep_s` ago, deleted; how many.
size_t prune_finished(db::Database &db, int64_t keep_s = kFinishedKeepS);

// The backoff of a retry after attempt `attempt` (1-based): `base_ms`
// doubling per attempt up to `max_ms`, then a jitter from `draw` (any 32 bits:
// os_random_u32) taking it to between half and all of that.
int64_t retry_delay_ms(int attempt, int64_t base_ms, int64_t max_ms, uint32_t draw);

// Whether a job with `dedupe_key` is queued or running.
bool job_pending(db::Database &db, const std::string &dedupe_key);

} // namespace opennova::novaworld_server::jobs
