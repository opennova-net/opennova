#include "job_admin.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <utility>
#include <variant>

namespace opennova::novaworld_server::jobs {

namespace {

using db::BindValue;

// A job's columns as JobView reads them (row_to_view), its payload's text or
// none.
std::string view_columns(bool payload) {
	return std::string("id, kind, state, ") + (payload ? "payload::text" : "''::text") +
			", CASE WHEN secret IS NULL THEN 0 WHEN secret = '' THEN 2 ELSE 1 END, attempts, "
			"max_attempts, last_error, run_at, created_at, started_at, finished_at, lease_until, "
			"COALESCE(worker_id, ''), COALESCE(dedupe_key, ''), COALESCE(schedule_key, ''), "
			"CASE WHEN state = 'running' THEN CAST(floor(extract(epoch FROM lease_until - now())) "
			"AS bigint) END, "
			"CASE WHEN state = 'running' THEN CAST(floor(extract(epoch FROM now() - started_at)) "
			"AS bigint) END";
}

// The database's "YYYY-MM-DD HH:MM:SS" (UTC) as ISO 8601; "" stays "".
std::string iso(const std::optional<std::string> &text) {
	if (!text || text->size() != 19 || (*text)[10] != ' ')
		return text.value_or("");
	return text->substr(0, 10) + "T" + text->substr(11) + "Z";
}

std::string iso_from_ms(int64_t unix_ms) {
	const std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char out[32];
	std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return out;
}

std::optional<double> real_at(const db::Row &row, size_t i) {
	if (i >= row.values.size())
		return std::nullopt;
	if (const auto *d = std::get_if<double>(&row.values[i]))
		return *d;
	if (const auto *n = std::get_if<int64_t>(&row.values[i]))
		return static_cast<double>(*n);
	return std::nullopt;
}

JobView row_to_view(const db::Row &r) {
	JobView j;
	j.id = r.as_int(0).value_or(0);
	j.kind = r.as_text(1).value_or("");
	j.state = r.as_text(2).value_or("");
	j.payload = r.as_text(3).value_or("");
	const int64_t secret = r.as_int(4).value_or(0);
	j.secret = secret == 1 ? JobView::Secret::Held
			: secret == 2  ? JobView::Secret::Dropped
						   : JobView::Secret::None;
	j.attempts = static_cast<int>(r.as_int(5).value_or(0));
	j.max_attempts = static_cast<int>(r.as_int(6).value_or(0));
	j.last_error = r.as_text(7).value_or("");
	j.run_at = iso(r.as_text(8));
	j.created_at = iso(r.as_text(9));
	j.started_at = iso(r.as_text(10));
	j.finished_at = iso(r.as_text(11));
	j.lease_until = iso(r.as_text(12));
	j.worker_id = r.as_text(13).value_or("");
	j.dedupe_key = r.as_text(14).value_or("");
	j.schedule_key = r.as_text(15).value_or("");
	j.lease_left_s = r.as_int(16);
	j.running_s = r.as_int(17);
	return j;
}

std::vector<JobView> views(
		db::Database &db, const std::string &sql, const std::vector<BindValue> &binds = {}) {
	std::vector<JobView> out;
	for (const db::Row &r : db.query(sql, binds))
		out.push_back(row_to_view(r));
	return out;
}

// `text` as a LIKE pattern matching it anywhere, its wildcards and the escape
// taken literally.
std::string contains_pattern(const std::string &text) {
	std::string out = "%";
	for (const char c : text) {
		if (c == '%' || c == '_' || c == '\\')
			out += '\\';
		out += c;
	}
	return out + "%";
}

// The worker's pool: its id less the thread's index ("worker-1a2b3c4d-0" is
// "worker-1a2b3c4d"'s).
std::string process_of(const std::string &worker_id) {
	const size_t dash = worker_id.rfind('-');
	if (dash == std::string::npos || dash + 1 >= worker_id.size())
		return worker_id;
	for (size_t i = dash + 1; i < worker_id.size(); ++i) {
		if (!std::isdigit(static_cast<unsigned char>(worker_id[i])))
			return worker_id;
	}
	return worker_id.substr(0, dash);
}

constexpr const char *kManualKey = "@manual-";

} // namespace

const char *secret_name(JobView::Secret secret) {
	switch (secret) {
		case JobView::Secret::None:
			return "none";
		case JobView::Secret::Held:
			return "held";
		case JobView::Secret::Dropped:
			return "dropped";
	}
	return "none";
}

bool retryable(const JobView &job) {
	return job.state == "failed" && job.secret != JobView::Secret::Dropped;
}

bool cancellable(const JobView &job) {
	return job.state == "queued";
}

bool manual_run(const JobView &job) {
	return job.schedule_key.find(kManualKey) != std::string::npos;
}

JobsOverview overview(db::Database &db) {
	JobsOverview out;
	out.at = iso(db.query("SELECT now();").front().as_text(0));

	out.totals.kind = "";
	double run_s_sum = 0;
	for (const db::Row &r : db.query(
				 "SELECT kind, "
				 "COUNT(*) FILTER (WHERE state = 'queued'), "
				 "COUNT(*) FILTER (WHERE state = 'queued' AND run_at <= now()), "
				 "COUNT(*) FILTER (WHERE state = 'running'), "
				 "COUNT(*) FILTER (WHERE state = 'done' AND finished_at > now() - interval '1 "
				 "hour'), "
				 "COUNT(*) FILTER (WHERE state = 'done' AND finished_at > now() - interval '1 "
				 "day'), "
				 "COUNT(*) FILTER (WHERE state = 'failed' AND finished_at > now() - interval '1 "
				 "hour'), "
				 "COUNT(*) FILTER (WHERE state = 'failed' AND finished_at > now() - interval '1 "
				 "day'), "
				 "COUNT(*) FILTER (WHERE state = 'failed'), "
				 "CAST(floor(extract(epoch FROM now() - MIN(run_at) FILTER (WHERE state = "
				 "'queued' AND run_at <= now()))) AS bigint), "
				 "CAST(AVG(extract(epoch FROM finished_at - started_at)) FILTER (WHERE state = "
				 "'done' AND finished_at > now() - interval '1 hour') AS double precision) "
				 "FROM jobs GROUP BY kind ORDER BY kind;")) {
		KindSummary k;
		k.kind = r.as_text(0).value_or("");
		k.queued = r.as_int(1).value_or(0);
		k.due = r.as_int(2).value_or(0);
		k.running = r.as_int(3).value_or(0);
		k.done_hour = r.as_int(4).value_or(0);
		k.done_day = r.as_int(5).value_or(0);
		k.failed_hour = r.as_int(6).value_or(0);
		k.failed_day = r.as_int(7).value_or(0);
		k.failed = r.as_int(8).value_or(0);
		k.oldest_wait_s = r.as_int(9);
		k.mean_run_s = real_at(r, 10);
		KindSummary &t = out.totals;
		t.queued += k.queued;
		t.due += k.due;
		t.running += k.running;
		t.done_hour += k.done_hour;
		t.done_day += k.done_day;
		t.failed_hour += k.failed_hour;
		t.failed_day += k.failed_day;
		t.failed += k.failed;
		if (k.oldest_wait_s && (!t.oldest_wait_s || *k.oldest_wait_s > *t.oldest_wait_s))
			t.oldest_wait_s = k.oldest_wait_s;
		if (k.mean_run_s)
			run_s_sum += *k.mean_run_s * static_cast<double>(k.done_hour);
		out.kinds.push_back(std::move(k));
	}
	if (out.totals.done_hour > 0)
		out.totals.mean_run_s = run_s_sum / static_cast<double>(out.totals.done_hour);

	for (const db::Row &r : db.query(
				 "SELECT g.m, COUNT(j.id) FILTER (WHERE j.state = 'done'), "
				 "COUNT(j.id) FILTER (WHERE j.state = 'failed') "
				 "FROM generate_series(date_trunc('minute', now()) - make_interval(mins => "
				 "$1::int), "
				 "date_trunc('minute', now()), interval '1 minute') AS g(m) "
				 "LEFT JOIN jobs j ON j.state IN ('done', 'failed') AND j.finished_at >= g.m AND "
				 "j.finished_at < g.m + interval '1 minute' "
				 "GROUP BY g.m ORDER BY g.m;",
				 { BindValue(static_cast<int64_t>(kPerMinuteBuckets - 1)) })) {
		MinuteCount m;
		m.at = iso(r.as_text(0));
		m.done = r.as_int(1).value_or(0);
		m.failed = r.as_int(2).value_or(0);
		out.per_minute.push_back(std::move(m));
	}

	out.attention = views(db,
			"SELECT " + view_columns(false) +
					" FROM jobs WHERE state = 'failed' AND finished_at > now() - interval '1 day' "
					"AND NOT starts_with(last_error, $1) ORDER BY finished_at DESC, id DESC "
					"LIMIT $2;",
			{ BindValue(std::string(kCancelledByAdmin)),
					BindValue(static_cast<int64_t>(kAttentionShown)) });
	out.running = views(db,
			"SELECT " + view_columns(false) +
					" FROM jobs WHERE state = 'running' ORDER BY started_at, id;");

	const BindValue window(static_cast<double>(kWorkerWindowS));
	for (const db::Row &r : db.query(
				 "SELECT worker_id, MAX(id) FILTER (WHERE state = 'running'), "
				 "MAX(kind) FILTER (WHERE state = 'running'), "
				 "COUNT(*) FILTER (WHERE state = 'done' AND finished_at > now() - "
				 "make_interval(secs => $1)), "
				 "COUNT(*) FILTER (WHERE state = 'failed' AND finished_at > now() - "
				 "make_interval(secs => $1)), "
				 "GREATEST(MAX(started_at), MAX(finished_at)) "
				 "FROM jobs WHERE worker_id IS NOT NULL AND (state = 'running' OR finished_at > "
				 "now() - make_interval(secs => $1)) GROUP BY worker_id ORDER BY worker_id;",
				 { window })) {
		WorkerView w;
		w.worker_id = r.as_text(0).value_or("");
		w.process = process_of(w.worker_id);
		w.job_id = r.as_int(1);
		w.job_kind = r.as_text(2).value_or("");
		w.done = r.as_int(3).value_or(0);
		w.failed = r.as_int(4).value_or(0);
		w.last_active_at = iso(r.as_text(5));
		out.workers.push_back(std::move(w));
	}

	const auto leader = db.query("SELECT holder, elected_at FROM controller_leader WHERE id = 1;");
	if (!leader.empty()) {
		out.leader = leader.front().as_text(0).value_or("");
		out.elected_at = iso(leader.front().as_text(1));
	}
	out.last_scheduled_at =
			iso(db.query("SELECT MAX(created_at) FROM jobs WHERE schedule_key IS "
						 "NOT NULL AND strpos(schedule_key, $1) = 0;",
						  { BindValue(std::string(kManualKey)) })
							.front()
							.as_text(0));
	return out;
}

std::vector<ScheduleView> schedules(db::Database &db, const std::vector<PeriodicJob> &periodic) {
	std::vector<ScheduleView> out;
	const int64_t now_ms =
			db.query("SELECT CAST(floor(extract(epoch FROM now()) * 1000) AS bigint);")
					.front()
					.as_int(0)
					.value_or(0);
	for (const PeriodicJob &p : periodic) {
		ScheduleView s;
		s.kind = p.kind;
		const int64_t period = std::max<int64_t>(p.period_ms, 1);
		s.interval_s = period / 1000;
		s.at_start = p.at_start;
		s.conditional = static_cast<bool>(p.wanted);
		const auto last = views(db,
				"SELECT " + view_columns(false) +
						" FROM jobs WHERE kind = $1 ORDER BY id DESC LIMIT 1;",
				{ BindValue(p.kind) });
		if (!last.empty())
			s.last = last.front();
		const auto finished = views(db,
				"SELECT " + view_columns(false) +
						" FROM jobs WHERE kind = $1 AND state IN ('done', 'failed') ORDER BY "
						"finished_at DESC, id DESC LIMIT 1;",
				{ BindValue(p.kind) });
		if (!finished.empty())
			s.last_finished = finished.front();
		// This bucket has its job once its key does, or (a kind the scheduler's
		// start enqueues) once a job of the kind with no key came in it.
		const int64_t bucket = now_ms / period;
		const bool enqueued =
				!db.query("SELECT 1 FROM jobs WHERE schedule_key = $1 OR ($2 AND kind = $3 AND "
						  "schedule_key IS NULL AND created_at >= to_timestamp($4::double "
						  "precision)) LIMIT 1;",
						   { BindValue(Scheduler::schedule_key(p.kind, now_ms, period)),
								   BindValue(db::Bool{ p.at_start }), BindValue(p.kind),
								   BindValue(static_cast<double>(bucket * period) / 1000.0) })
						 .empty();
		s.next_at = iso_from_ms((enqueued ? bucket + 1 : bucket) * period);
		s.pending = job_pending(db, p.kind);
		out.push_back(std::move(s));
	}
	return out;
}

JobPage list_jobs(db::Database &db, const JobFilter &filter) {
	std::string where = "TRUE";
	std::vector<BindValue> binds;
	const auto bind = [&binds](BindValue value) {
		binds.push_back(std::move(value));
		return "$" + std::to_string(binds.size());
	};
	if (!filter.state.empty())
		where += " AND state = " + bind(filter.state);
	if (!filter.kind.empty())
		where += " AND kind = " + bind(filter.kind);
	if (filter.from_s)
		where += " AND created_at >= to_timestamp(" + bind(static_cast<double>(*filter.from_s)) +
				"::double precision)";
	if (filter.to_s)
		where += " AND created_at < to_timestamp(" + bind(static_cast<double>(*filter.to_s)) +
				"::double precision)";
	if (!filter.search.empty()) {
		const std::string pattern = bind(contains_pattern(filter.search));
		where += " AND (dedupe_key ILIKE " + pattern + " OR schedule_key ILIKE " + pattern + ")";
	}
	JobPage page;
	page.total = db.query("SELECT COUNT(*) FROM jobs WHERE " + where + ";", binds)
						 .front()
						 .as_int(0)
						 .value_or(0);
	if (filter.before_id)
		where += " AND id < " + bind(*filter.before_id);
	const size_t limit = std::clamp<size_t>(filter.limit, 1, kListMax);
	page.jobs = views(db,
			"SELECT " + view_columns(false) + " FROM jobs WHERE " + where +
					" ORDER BY id DESC LIMIT " + bind(static_cast<int64_t>(limit + 1)) + ";",
			binds);
	if (page.jobs.size() > limit) {
		page.jobs.resize(limit);
		page.next_before = page.jobs.back().id;
	}
	return page;
}

std::optional<JobView> find_job(db::Database &db, int64_t id) {
	const auto found = views(
			db, "SELECT " + view_columns(true) + " FROM jobs WHERE id = $1;", { BindValue(id) });
	if (found.empty())
		return std::nullopt;
	return found.front();
}

const char *action_result_name(ActionResult result) {
	switch (result) {
		case ActionResult::Done:
			return "done";
		case ActionResult::NotFound:
			return "not_found";
		case ActionResult::WrongState:
			return "wrong_state";
		case ActionResult::SecretDropped:
			return "secret_dropped";
		case ActionResult::AlreadyPending:
			return "already_pending";
	}
	return "not_found";
}

ActionResult retry_now(db::Database &db, int64_t id) {
	try {
		db.exec("UPDATE jobs SET state = 'queued', run_at = now(), max_attempts = attempts + 1, "
				"finished_at = NULL, lease_until = NULL, worker_id = NULL "
				"WHERE id = $1 AND state = 'failed' AND secret IS NULL;",
				{ BindValue(id) });
	} catch (const db::Error &e) {
		// Its dedupe key: another job holding it waits or runs.
		if (e.unique_violation())
			return ActionResult::AlreadyPending;
		throw;
	}
	if (db.changes() > 0)
		return ActionResult::Done;
	const auto job = find_job(db, id);
	if (!job)
		return ActionResult::NotFound;
	if (job->state == "failed" && job->secret == JobView::Secret::Dropped)
		return ActionResult::SecretDropped;
	return ActionResult::WrongState;
}

ActionResult cancel(db::Database &db, int64_t id, const std::string &actor) {
	db.exec("UPDATE jobs SET state = 'failed', finished_at = now(), last_error = $2, "
			"lease_until = NULL, worker_id = NULL, "
			"secret = CASE WHEN secret IS NULL THEN NULL ELSE '' END "
			"WHERE id = $1 AND state = 'queued';",
			{ BindValue(id), BindValue(std::string(kCancelledByAdmin) + " (" + actor + ")") });
	if (db.changes() > 0)
		return ActionResult::Done;
	return find_job(db, id) ? ActionResult::WrongState : ActionResult::NotFound;
}

int64_t run_now(db::Database &db, const PeriodicJob &periodic,
		const std::optional<io::JsonValue> &payload) {
	const int64_t now_ms = Scheduler::wall_now_ms();
	NewJob job;
	job.kind = periodic.kind;
	job.payload = payload ? *payload : periodic.payload;
	job.dedupe_key = periodic.kind;
	job.schedule_key = periodic.kind + kManualKey + std::to_string(now_ms);
	job.max_attempts = periodic.max_attempts;
	return enqueue(db, job);
}

} // namespace opennova::novaworld_server::jobs
