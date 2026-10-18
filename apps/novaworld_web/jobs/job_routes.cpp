#include "job_routes.h"

#include "http_json.h"
#include "loggable.h"
#include "jobs/maintenance_jobs.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace opennova::novaworld_server::jobs {

namespace {

template <typename T>
void value_or_null(crow::json::wvalue &out, const char *key, const std::optional<T> &value) {
	if (value)
		out[key] = *value;
	else
		out[key] = nullptr;
}

// A query parameter of digits alone (at most 18); nullopt when absent, and
// `bad` set when present but no such number.
std::optional<int64_t> query_number(const crow::request &req, const char *name, bool &bad) {
	const char *value = req.url_params.get(name);
	if (value == nullptr)
		return std::nullopt;
	const std::string text(value);
	if (text.empty() || text.size() > 18 ||
			!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
		bad = true;
		return std::nullopt;
	}
	const auto number = strutil::parse_llong(text);
	if (!number) {
		bad = true;
		return std::nullopt;
	}
	return static_cast<int64_t>(*number);
}

// The longest kind or key text a filter takes.
constexpr size_t kFilterTextMax = 128;

// The actor a state change is logged with (AccessMiddleware's own words).
std::string actor_of(const RouteAccess &caller) {
	return caller.user ? "session '" + loggable(caller.user->username) + "'" : std::string("token");
}

// A job as JSON (never its secret: only its state). `detail` adds its payload.
crow::json::wvalue job_json(const JobView &j, bool detail) {
	crow::json::wvalue out;
	out["id"] = j.id;
	out["kind"] = j.kind;
	out["state"] = j.state;
	out["attempts"] = j.attempts;
	out["max_attempts"] = j.max_attempts;
	out["last_error"] = j.last_error;
	text_or_null(out, "run_at", j.run_at);
	text_or_null(out, "created_at", j.created_at);
	text_or_null(out, "started_at", j.started_at);
	text_or_null(out, "finished_at", j.finished_at);
	text_or_null(out, "lease_until", j.lease_until);
	value_or_null(out, "lease_left_seconds", j.lease_left_s);
	value_or_null(out, "running_seconds", j.running_s);
	text_or_null(out, "worker_id", j.worker_id);
	text_or_null(out, "dedupe_key", j.dedupe_key);
	text_or_null(out, "schedule_key", j.schedule_key);
	out["manual"] = manual_run(j);
	out["secret_state"] = secret_name(j.secret);
	out["retryable"] = retryable(j);
	out["cancellable"] = cancellable(j);
	if (detail) {
		const crow::json::rvalue payload = crow::json::load(j.payload);
		if (payload)
			out["payload"] = crow::json::wvalue(payload);
		else
			out["payload"] = j.payload;
	}
	return out;
}

crow::json::wvalue kind_json(const KindSummary &k) {
	crow::json::wvalue out;
	if (!k.kind.empty())
		out["kind"] = k.kind;
	out["queued"] = k.queued;
	out["due"] = k.due;
	out["running"] = k.running;
	out["done_hour"] = k.done_hour;
	out["done_day"] = k.done_day;
	out["failed_hour"] = k.failed_hour;
	out["failed_day"] = k.failed_day;
	out["failed"] = k.failed;
	value_or_null(out, "oldest_wait_seconds", k.oldest_wait_s);
	value_or_null(out, "mean_run_seconds", k.mean_run_s);
	return out;
}

std::vector<crow::json::wvalue> jobs_json(const std::vector<JobView> &jobs) {
	std::vector<crow::json::wvalue> out;
	for (const JobView &j : jobs)
		out.push_back(job_json(j, false));
	return out;
}

// A job's reply after an action: it as it is now.
crow::response job_reply(int code, db::Database &db, int64_t id) {
	crow::json::wvalue out;
	if (const auto job = find_job(db, id))
		out["job"] = job_json(*job, true);
	else
		out["job"] = nullptr;
	return json_reply(code, out);
}

} // namespace

void JobRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/jobs/overview")
			.methods("GET"_method)(
					[this](const crow::request &req) { return overview_route(req); });
	CROW_ROUTE(app, "/api/admin/jobs").methods("GET"_method)([this](const crow::request &req) {
		return list_route(req);
	});
	CROW_ROUTE(app, "/api/admin/jobs/run").methods("POST"_method)([this](const crow::request &req) {
		return run_route(req);
	});
	CROW_ROUTE(app, "/api/admin/jobs/retry-group")
			.methods("POST"_method)(
					[this](const crow::request &req) { return retry_group_route(req); });
	CROW_ROUTE(app, "/api/admin/jobs/dismiss-group")
			.methods("POST"_method)(
					[this](const crow::request &req) { return dismiss_group_route(req); });
	CROW_ROUTE(app, "/api/admin/jobs/prune")
			.methods("POST"_method)([this](const crow::request &req) { return prune_route(req); });
	CROW_ROUTE(app, "/api/admin/jobs/<uint>")
			.methods("GET"_method)(
					[this](const crow::request &req, uint64_t id) { return job_route(req, id); });
	CROW_ROUTE(app, "/api/admin/jobs/<uint>/retry")
			.methods("POST"_method)(
					[this](const crow::request &req, uint64_t id) { return retry_route(req, id); });
	CROW_ROUTE(app, "/api/admin/jobs/<uint>/cancel")
			.methods("POST"_method)([this](const crow::request &req, uint64_t id) {
				return cancel_route(req, id);
			});
}

const PeriodicJob *JobRoutes::schedule_of(const std::string &kind) const {
	for (const PeriodicJob &p : schedules_) {
		if (p.kind == kind)
			return &p;
	}
	return nullptr;
}

// GET /api/admin/jobs/overview — {"at" (the database's now), "totals" and
// "kinds" (each kind's `queued` (due or not), `due`, `running`, `done_hour`,
// `done_day`, `failed_hour`, `failed_day`, `failed` (kept until the prune),
// `oldest_wait_seconds` (the oldest due job's wait: the queue's latency) and
// `mean_run_seconds` (the last hour's done jobs); the totals with no kind),
// "attention" (the newest 20 failure groups: the jobs of one kind failed in
// the last day with one failure key, an admin's cancels left out: `kind`,
// `key`, `count`, `retryable` (how many a retry takes), `last_job_id`,
// `last_error` (the newest one's text) and `last_at`), "attention_total"
// (every job the groups count, shown or not), "workers" (each worker process
// with a thread holding a job or done with one in the last hour: `process`,
// `threads`, `busy`, `lapsed` (its running jobs whose lease ran out),
// `lease_left_seconds` (the least left of its others, null with none),
// `done`, `failed`, `last_active_at`), "schedules" (each periodic kind:
// `kind`, `interval_seconds`, `at_start`, `conditional`, `last` and
// `last_finished` (jobs or null), `next_at`, `pending`), "last_scheduled_at"
// (the newest job a schedule enqueued, null for none), "fleet" (`leader`,
// `elected_at`, each null before an election)}. One snapshot; no-store.
crow::response JobRoutes::overview_route(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const JobsOverview o = overview(*db_conn);
		out["at"] = o.at;
		out["totals"] = kind_json(o.totals);
		std::vector<crow::json::wvalue> kinds;
		for (const KindSummary &k : o.kinds)
			kinds.push_back(kind_json(k));
		out["kinds"] = std::move(kinds);
		std::vector<crow::json::wvalue> attention;
		for (const FailureGroup &g : o.attention) {
			crow::json::wvalue e;
			e["kind"] = g.kind;
			e["key"] = g.key;
			e["count"] = g.count;
			e["retryable"] = g.retryable;
			e["last_job_id"] = g.last_job_id;
			e["last_error"] = g.last_error;
			text_or_null(e, "last_at", g.last_at);
			attention.push_back(std::move(e));
		}
		out["attention"] = std::move(attention);
		out["attention_total"] = o.attention_total;
		std::vector<crow::json::wvalue> workers;
		for (const WorkerView &w : o.workers) {
			crow::json::wvalue e;
			e["process"] = w.process;
			e["threads"] = w.threads;
			e["busy"] = w.busy;
			e["lapsed"] = w.lapsed;
			value_or_null(e, "lease_left_seconds", w.lease_left_s);
			e["done"] = w.done;
			e["failed"] = w.failed;
			text_or_null(e, "last_active_at", w.last_active_at);
			workers.push_back(std::move(e));
		}
		out["workers"] = std::move(workers);
		std::vector<crow::json::wvalue> scheduled;
		for (const ScheduleView &s : schedules(*db_conn, schedules_)) {
			crow::json::wvalue e;
			e["kind"] = s.kind;
			e["interval_seconds"] = s.interval_s;
			e["at_start"] = s.at_start;
			e["conditional"] = s.conditional;
			if (s.last)
				e["last"] = job_json(*s.last, false);
			else
				e["last"] = nullptr;
			if (s.last_finished)
				e["last_finished"] = job_json(*s.last_finished, false);
			else
				e["last_finished"] = nullptr;
			e["next_at"] = s.next_at;
			e["pending"] = s.pending;
			scheduled.push_back(std::move(e));
		}
		out["schedules"] = std::move(scheduled);
		text_or_null(out, "last_scheduled_at", o.last_scheduled_at);
		crow::json::wvalue fleet;
		text_or_null(fleet, "leader", o.leader);
		text_or_null(fleet, "elected_at", o.elected_at);
		out["fleet"] = std::move(fleet);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/jobs/overview failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// GET /api/admin/jobs[?state=][&kind=][&from=][&to=][&q=][&failure=][&before=][&limit=]
// — {"jobs" (newest first, as the overview's), "total" (every job the filter
// takes), "next_before" (the next page's `before`, null on the last page)}.
// `state` queued, running, done or failed; `kind` exact; `from` and `to`
// (Unix seconds) bound when a job was created, `to` excluded; `q` a text
// within the dedupe or schedule key, any case; `failure` a failure key (an
// attention group's `key`), exact; `before` a job id (the page after it);
// `limit` 1..200 (50). 400 invalid_field for another state, a number that is
// none, a kind or q past 128 bytes or a failure past 300; no-store.
crow::response JobRoutes::list_route(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	JobFilter filter;
	if (const char *state = req.url_params.get("state")) {
		filter.state = state;
		if (filter.state != "queued" && filter.state != "running" && filter.state != "done" &&
				filter.state != "failed")
			return json_error(400, "invalid_field", "state is queued, running, done or failed");
	}
	if (const char *kind = req.url_params.get("kind"))
		filter.kind = kind;
	if (const char *search = req.url_params.get("q"))
		filter.search = search;
	if (filter.kind.size() > kFilterTextMax || filter.search.size() > kFilterTextMax)
		return json_error(400, "invalid_field", "kind and q are at most 128 bytes");
	if (const char *failure = req.url_params.get("failure"))
		filter.failure = failure;
	if (filter.failure.size() > kFailureKeyMax)
		return json_error(400, "invalid_field", "failure is at most 300 bytes");
	bool bad = false;
	filter.from_s = query_number(req, "from", bad);
	filter.to_s = query_number(req, "to", bad);
	filter.before_id = query_number(req, "before", bad);
	const auto limit = query_number(req, "limit", bad);
	if (bad || (limit && (*limit < 1 || *limit > static_cast<int64_t>(kListMax))))
		return json_error(
				400, "invalid_field", "from, to and before are whole numbers, limit 1 to 200");
	if (limit)
		filter.limit = static_cast<size_t>(*limit);
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::ReadSnapshot snapshot(*db_conn);
		const JobPage page = list_jobs(*db_conn, filter);
		out["jobs"] = jobs_json(page.jobs);
		out["total"] = page.total;
		value_or_null(out, "next_before", page.next_before);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/jobs failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// GET /api/admin/jobs/<id> — {"job"}: as the list's, plus its `payload` (the
// JSON its handler reads); never its secret, only `secret_state` (`none`,
// `held`, or `dropped` once it ended). 404 not_found; no-store.
crow::response JobRoutes::job_route(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	std::optional<JobView> job;
	try {
		job = find_job(*pool_.acquire(), path_id(id));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/jobs/%llu failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
	if (!job)
		return json_error(404, "not_found", "no job has this id");
	crow::json::wvalue out;
	out["job"] = job_json(*job, true);
	return no_store(json_reply(200, out));
}

// POST /api/admin/jobs/<id>/retry — a failed job queued again to run now, one
// attempt more allowed than it has used (its attempts kept, max_attempts one
// past them), its last error kept until it runs. 200 {"job"}; 404 not_found;
// 409 not_failed (with its "state") for a job that has not failed;
// secret_dropped for one whose secret went as it ended (a mail, a machine's
// launch: it cannot run again); already_pending while a job with its dedupe
// key is queued or running.
crow::response JobRoutes::retry_route(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const int64_t job_id = path_id(id);
	try {
		auto db_conn = pool_.acquire();
		const ActionResult result = retry_now(*db_conn, job_id);
		switch (result) {
			case ActionResult::Done: {
				const auto job = find_job(*db_conn, job_id);
				std::printf("[jobs] job %lld (%s) queued again by an admin (%s): one more attempt, "
							"%d of %d\n",
						static_cast<long long>(job_id), job ? job->kind.c_str() : "?",
						actor_of(caller).c_str(), job ? job->attempts + 1 : 0,
						job ? job->max_attempts : 0);
				return job_reply(200, *db_conn, job_id);
			}
			case ActionResult::NotFound:
				return json_error(404, "not_found", "no job has this id");
			case ActionResult::SecretDropped:
				return json_error(409, "secret_dropped",
						"its secret went when it ended, so it cannot run again");
			case ActionResult::AlreadyPending:
				return json_error(
						409, "already_pending", "a job with its dedupe key is queued or running");
			case ActionResult::WrongState: {
				crow::json::wvalue out;
				out["error"] = "not_failed";
				out["message"] = "only a failed job is queued again";
				const auto job = find_job(*db_conn, job_id);
				out["state"] = job ? job->state : std::string();
				return json_reply(409, out);
			}
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/jobs/%llu/retry failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
	}
	return json_error(500, "db_error");
}

// POST /api/admin/jobs/retry-group — {"kind", "key"}: an attention group's
// jobs (of `kind`, failed in the last day with failure key `key`, an admin's
// cancels left out) each queued again as POST .../<id>/retry would, each its
// own compare-and-set. 200 {"retried", "secret_dropped" (their secret went as
// they ended: they cannot run again), "already_pending" (a job with their
// dedupe key waits or runs), "changed" (no longer failed)}; 400 invalid_json,
// missing_field, invalid_field (a kind past 128 bytes, a key past 300).
crow::response JobRoutes::retry_group_route(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto kind = body->text("kind");
	const auto key = body->text("key");
	if (body->wrong_type())
		return json_error(400, "invalid_field", "kind and key are texts");
	if (!kind || kind->empty() || !key)
		return json_error(400, "missing_field", "kind and key are required");
	if (kind->size() > kFilterTextMax || key->size() > kFailureKeyMax)
		return json_error(400, "invalid_field", "kind is at most 128 bytes, key 300");
	try {
		auto db_conn = pool_.acquire();
		const GroupRetry result = retry_group(*db_conn, *kind, *key);
		std::printf("[jobs] %lld failed %s job(s) queued again by an admin (%s); left: %lld "
					"whose secret went, %lld pending, %lld changed\n",
				static_cast<long long>(result.retried), loggable(*kind).c_str(),
				actor_of(caller).c_str(), static_cast<long long>(result.secret_dropped),
				static_cast<long long>(result.already_pending),
				static_cast<long long>(result.changed));
		crow::json::wvalue out;
		out["retried"] = result.retried;
		out["secret_dropped"] = result.secret_dropped;
		out["already_pending"] = result.already_pending;
		out["changed"] = result.changed;
		return json_reply(200, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/jobs/retry-group failed: %s\n", e.what());
	}
	return json_error(500, "db_error");
}

// POST /api/admin/jobs/dismiss-group — {"kind", "key"}: the failure group's
// jobs (as retry-group takes them) deleted. 200 {"dismissed"}; 400 as
// retry-group's.
crow::response JobRoutes::dismiss_group_route(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto kind = body->text("kind");
	const auto key = body->text("key");
	if (body->wrong_type())
		return json_error(400, "invalid_field", "kind and key are texts");
	if (!kind || kind->empty() || !key)
		return json_error(400, "missing_field", "kind and key are required");
	if (kind->size() > kFilterTextMax || key->size() > kFailureKeyMax)
		return json_error(400, "invalid_field", "kind is at most 128 bytes, key 300");
	try {
		auto db_conn = pool_.acquire();
		const int64_t dismissed = dismiss_group(*db_conn, *kind, *key);
		std::printf("[jobs] %lld failed %s job(s) dismissed by an admin (%s)\n",
				static_cast<long long>(dismissed), loggable(*kind).c_str(), actor_of(caller).c_str());
		crow::json::wvalue out;
		out["dismissed"] = dismissed;
		return json_reply(200, out);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/jobs/dismiss-group failed: %s\n", e.what());
	}
	return json_error(500, "db_error");
}

// POST /api/admin/jobs/<id>/cancel — a queued job failed for good, its error
// "cancelled by an admin (<actor>)", its secret dropped; a running one is its
// worker's and is never touched. 200 {"job"}; 404 not_found; 409 not_queued
// (with its "state").
crow::response JobRoutes::cancel_route(const crow::request &req, uint64_t id) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	const int64_t job_id = path_id(id);
	const std::string actor = actor_of(caller);
	try {
		auto db_conn = pool_.acquire();
		const ActionResult result = cancel(*db_conn, job_id, actor);
		if (result == ActionResult::NotFound)
			return json_error(404, "not_found", "no job has this id");
		const auto job = find_job(*db_conn, job_id);
		if (result != ActionResult::Done) {
			crow::json::wvalue out;
			out["error"] = "not_queued";
			out["message"] = "only a queued job is cancelled";
			out["state"] = job ? job->state : std::string();
			return json_reply(409, out);
		}
		std::printf("[jobs] job %lld (%s) cancelled by an admin (%s)\n",
				static_cast<long long>(job_id), job ? job->kind.c_str() : "?", actor.c_str());
		return job_reply(200, *db_conn, job_id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/jobs/%llu/cancel failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
	}
	return json_error(500, "db_error");
}

// POST /api/admin/jobs/run — {"kind"}: a periodic kind this deployment
// schedules, enqueued now as its schedule would (its payload, its attempts,
// its kind the dedupe key) with the key "<kind>@manual-<ms>". 202 {"job"};
// 400 invalid_json, missing_field, invalid_field; 404 not_found for a kind no
// schedule here names; 409 already_pending while one of the kind is queued or
// running.
crow::response JobRoutes::run_route(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const auto kind = body->text("kind");
	if (body->wrong_type())
		return json_error(400, "invalid_field", "kind is a text");
	if (!kind || kind->empty())
		return json_error(400, "missing_field", "kind is required");
	const PeriodicJob *periodic = schedule_of(*kind);
	if (periodic == nullptr)
		return json_error(404, "not_found", "no periodic job of this kind runs here");
	try {
		auto db_conn = pool_.acquire();
		const int64_t id = run_now(*db_conn, *periodic);
		if (id == 0)
			return json_error(409, "already_pending", "one of this kind is queued or running");
		std::printf("[jobs] %s run now by an admin (%s): job %lld\n", periodic->kind.c_str(),
				actor_of(caller).c_str(), static_cast<long long>(id));
		return job_reply(202, *db_conn, id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/jobs/run failed: %s\n", e.what());
	}
	return json_error(500, "db_error");
}

// POST /api/admin/jobs/prune — {"keep_days": 1..14} (14 when absent): the
// jobs finished more than that long ago deleted now, by a jobs.prune job (a
// worker runs it, as the hourly one, with that keep). 202 {"job"}; 400
// invalid_json, invalid_field; 409 already_pending while a jobs.prune is
// queued or running.
crow::response JobRoutes::prune_route(const crow::request &req) {
	auto caller = access_.require_admin(req);
	if (caller.refusal)
		return std::move(*caller.refusal);
	auto body = JsonBody::parse(req.body.empty() ? std::string("{}") : req.body);
	if (!body)
		return json_error(400, "invalid_json");
	const int64_t keep_days = body->integer("keep_days").value_or(kPruneKeepDaysMax);
	if (body->wrong_type() || keep_days < kPruneKeepDaysMin || keep_days > kPruneKeepDaysMax)
		return json_error(400, "invalid_field", "keep_days is a whole number of days, 1 to 14");
	PeriodicJob periodic;
	if (const PeriodicJob *scheduled = schedule_of(kJobJobsPrune))
		periodic = *scheduled;
	periodic.kind = kJobJobsPrune;
	io::JsonValue payload = io::JsonValue::make_object();
	payload.set(kPruneKeepKey, io::json_number(static_cast<double>(keep_days * 24 * 60 * 60)));
	try {
		auto db_conn = pool_.acquire();
		const int64_t id = run_now(*db_conn, periodic, payload);
		if (id == 0)
			return json_error(409, "already_pending", "a jobs.prune is queued or running");
		std::printf("[jobs] jobs.prune of the jobs finished %lld day(s) ago asked by an admin "
					"(%s): job %lld\n",
				static_cast<long long>(keep_days), actor_of(caller).c_str(),
				static_cast<long long>(id));
		return job_reply(202, *db_conn, id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] POST /api/admin/jobs/prune failed: %s\n", e.what());
	}
	return json_error(500, "db_error");
}

} // namespace opennova::novaworld_server::jobs
