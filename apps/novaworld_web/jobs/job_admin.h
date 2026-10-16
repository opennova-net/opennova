#pragma once

// The admin's view of the background jobs and the admin's actions on them
// (README "Background jobs"; web's /api/admin/jobs routes,
// job_routes.h). It reads the jobs table and the controller's row alone, never
// a worker's or the controller's memory, so every web replica answers alike:
// what it shows of a worker is what the claims leave on the rows (worker_id,
// started_at, lease_until), so a worker thread shows once it has claimed a
// job. Each action is one compare-and-set on a job's state (a retry takes a
// failed job only, a cancel a queued one only), so none ever changes a job a
// worker holds. Times are the database's, ISO 8601 UTC text ("" for none), and
// durations whole seconds. Never a job's secret: only whether it holds one, or
// held one that went as it ended (job_queue.h). Crow-free; each function
// throws db::Error on a database failure. Service policy, not a port.

#include "jobs/job_queue.h"
#include "jobs/scheduler.h"

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::jobs {

// What the last_error of a job an admin cancelled starts with: the overview's
// attention list leaves such a job out.
inline constexpr const char *kCancelledByAdmin = "cancelled by an admin";
// The failed jobs the overview lists for attention (the newest, of the last
// day), and a list's page: its default and its most.
inline constexpr size_t kAttentionShown = 20;
inline constexpr size_t kListDefault = 50;
inline constexpr size_t kListMax = 200;
// The overview's finished jobs per minute: the last hour's minutes.
inline constexpr int kPerMinuteBuckets = 60;
// The window a worker shows in after its last finished job.
inline constexpr int64_t kWorkerWindowS = 60 * 60;

// A job as the admin sees it.
struct JobView {
	enum class Secret : uint8_t {
		None, // it never carried one
		Held, // it carries one now (it has not ended)
		Dropped, // it carried one, dropped when it ended: it cannot run again
	};
	int64_t id = 0;
	std::string kind;
	std::string state; // queued, running, done, failed
	std::string payload; // JSON text (find_job's alone; "" in a list)
	Secret secret = Secret::None;
	int attempts = 0;
	int max_attempts = 0;
	std::string last_error;
	std::string run_at;
	std::string created_at;
	std::string started_at; // its last claim's
	std::string finished_at;
	std::string lease_until; // a running job's
	std::string worker_id; // the last worker that claimed it ("" for none)
	std::string dedupe_key;
	std::string schedule_key;
	// A running job's: the seconds left on its lease (negative once it ran
	// out: its worker went away, and the keeper queues it again) and since its
	// claim.
	std::optional<int64_t> lease_left_s;
	std::optional<int64_t> running_s;
};
const char *secret_name(JobView::Secret secret);
// Whether an admin may queue it again (failed, its secret not dropped), or
// cancel it (queued).
bool retryable(const JobView &job);
bool cancellable(const JobView &job);
// Whether its schedule key is an admin's run (run_now's "<kind>@manual-<ms>").
bool manual_run(const JobView &job);

// One kind's jobs now ("" in `kind`: every kind's together).
struct KindSummary {
	std::string kind;
	int64_t queued = 0; // waiting, due or not (a retry's backoff)
	int64_t due = 0; // queued and due: waiting on a worker
	int64_t running = 0;
	int64_t done_hour = 0;
	int64_t done_day = 0;
	int64_t failed_hour = 0; // an admin's cancels among them
	int64_t failed_day = 0;
	int64_t failed = 0; // every failed one kept (until jobs.prune)
	// The oldest due job's wait since it came due: the queue's latency.
	std::optional<int64_t> oldest_wait_s;
	// The last hour's done jobs, from their last claim to their end.
	std::optional<double> mean_run_s;
};

// One minute's finished jobs.
struct MinuteCount {
	std::string at; // the minute's start
	int64_t done = 0;
	int64_t failed = 0;
};

// A worker thread as the rows show it: one that holds a job now, or finished
// one within kWorkerWindowS.
struct WorkerView {
	std::string worker_id;
	std::string process; // its pool's (the id less its thread's index)
	std::optional<int64_t> job_id; // the job it holds now
	std::string job_kind;
	int64_t done = 0; // finished within the window
	int64_t failed = 0;
	std::string last_active_at; // its newest claim or end
};

struct JobsOverview {
	std::string at; // the database's now
	KindSummary totals;
	std::vector<KindSummary> kinds; // by kind
	std::vector<MinuteCount> per_minute; // kPerMinuteBuckets, oldest first
	// The newest kAttentionShown jobs failed in the last day, an admin's cancels
	// left out.
	std::vector<JobView> attention;
	// Every running job, its claim oldest first.
	std::vector<JobView> running;
	std::vector<WorkerView> workers; // by id
	// The controller that leads (controller_leader: "" before an election) and
	// since when, and the newest job a schedule enqueued.
	std::string leader;
	std::string elected_at;
	std::string last_scheduled_at;
};
JobsOverview overview(db::Database &db);

// A periodic kind's schedule (scheduler.h) with what its rows show.
struct ScheduleView {
	std::string kind;
	int64_t interval_s = 0;
	bool at_start = false;
	bool conditional = false; // its bucket only while its `wanted` test holds
	std::optional<JobView> last; // the kind's newest job
	std::optional<JobView> last_finished; // its newest done or failed one
	// The bucket the scheduler enqueues next: this one's start while this
	// bucket has none yet, else the next one's.
	std::string next_at;
	// One queued or running holds its dedupe key: the next bucket waits for it.
	bool pending = false;
};
std::vector<ScheduleView> schedules(db::Database &db, const std::vector<PeriodicJob> &periodic);

// The list's filter: each set one narrows it.
struct JobFilter {
	std::string state; // queued, running, done, failed; "" any
	std::string kind; // exact; "" any
	std::optional<int64_t> from_s; // created at or after (Unix seconds)
	std::optional<int64_t> to_s; // created before
	std::string search; // within the dedupe or schedule key, any case
	std::optional<int64_t> before_id; // the page after the one that ended there
	size_t limit = kListDefault; // 1..kListMax
};
struct JobPage {
	std::vector<JobView> jobs; // newest first
	int64_t total = 0; // every job the filter takes
	std::optional<int64_t> next_before; // the next page's cursor, while one follows
};
JobPage list_jobs(db::Database &db, const JobFilter &filter);
// Job `id` with its payload; nullopt for none.
std::optional<JobView> find_job(db::Database &db, int64_t id);

// An action's outcome.
enum class ActionResult : uint8_t {
	Done,
	NotFound,
	WrongState, // not failed (a retry) or not queued (a cancel)
	SecretDropped, // a retry of a job whose secret went as it ended
	AlreadyPending, // a job with its dedupe key is queued or running
};
const char *action_result_name(ActionResult result);
// A failed job queued again to run now, one attempt more allowed than it has
// used (max_attempts = attempts + 1, its attempts kept); its last error stays
// until it runs again.
ActionResult retry_now(db::Database &db, int64_t id);
// A queued job failed for good, "<kCancelledByAdmin> (<actor>)" its error and
// its secret dropped.
ActionResult cancel(db::Database &db, int64_t id, const std::string &actor);
// `periodic`'s kind enqueued now, as its schedule would (its payload, or
// `payload` when set; its max attempts; its kind the dedupe key), with the
// schedule key "<kind>@manual-<now ms>". Its id; 0 while one is queued or
// running.
int64_t run_now(db::Database &db, const PeriodicJob &periodic,
		const std::optional<io::JsonValue> &payload = std::nullopt);

} // namespace opennova::novaworld_server::jobs
