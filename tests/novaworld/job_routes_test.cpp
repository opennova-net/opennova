// The admin's background jobs routes (apps/novaworld_web/jobs/job_routes.h over
// job_admin.h), driven in-process: a database of the run's own
// (common/test_database.h), a Redis namespace of its own (common/test_redis.h), the
// Crow HttpListener from opennova_novaworld_web_core on a port the OS picks, with
// a schedule of three periodic kinds (jobs.prune hourly, test.every a minute,
// test.cond a minute while its test holds), and requests through apps/common's
// http_exchange. Jobs are planted on the rows by hand (enqueued, claimed, finished,
// their times moved) as workers and schedulers would leave them. The cases share the
// listener and run in order, each starting from an empty jobs table: every route
// refused without an admin (401, 403 for a player, 403 for a state change with no CSRF
// header) and nothing touched; the overview's counts by kind and state, the queue's
// latency, the last hour's minutes, the failed jobs to look at (an admin's cancels left
// out), the running jobs with their leases (one run out) and the workers they name,
// the schedules (the last run, the next bucket, a pending one) and the controller's
// row; the list's filters (state, kind, a time range, a key search) and its pages;
// one job's detail with its payload; the actions, each a compare-and-set (a retry of a
// failed job only, with one more attempt; refused for a job not failed, one whose
// secret went, one whose dedupe key another holds; a cancel of a queued job only, a
// running one left to its worker; a periodic kind run now once, then 409; a prune with
// its keep that a worker then runs); and never a job's secret in any reply.

#include "auth.h"
#include "http_listener.h"
#include "jobs/job_admin.h"
#include "jobs/job_queue.h"
#include "jobs/maintenance_jobs.h"
#include "jobs/scheduler.h"
#include "jobs/worker_pool.h"
#include "net_http.h"
#include "net_sockets.h"
#include "server_config.h"
#include "web_session.h"

#include <net/novaworld/db/database.h>

#include <crow/json.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace jobs = opennova::novaworld_server::jobs;
using opennova::db::BindValue;
using opennova::db::ConnectionPool;
using opennova::db::Database;

namespace {

constexpr const char *kAdminToken = "job-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer job-routes-admin-token";
constexpr const char *kJson = "Content-Type: application/json";
constexpr const char *kCsrf = "X-OpenNova-Request: 1";
// The secret planted on a job, which no reply may carry.
constexpr const char *kSecret = "planted-job-secret-7f3a";

struct Harness {
	ConnectionPool &pool;
	uint16_t port = 0;
	std::string admin_cookie; // an admin's session
	std::string player_cookie; // a player's

	net::HttpReply send(const char *method, const std::string &path,
			const std::vector<std::string> &headers = {}, const std::string &body = {}) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
		return net::http_exchange(
				method, url, headers, body, 5000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
	}
	// An admin's request with the Bearer token.
	net::HttpReply admin(
			const char *method, const std::string &path, const std::string &body = {}) const {
		return send(method, path, { kJson, kBearer }, body);
	}
};

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

std::string describe(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "no reply (" + reply.error + ")";
	return "HTTP " + std::to_string(reply.code) + " " + body_text(reply);
}

std::string str(const crow::json::rvalue &v) {
	return std::string(v.s());
}

bool is_null(const crow::json::rvalue &v) {
	return v.t() == crow::json::type::Null;
}

// The reply's JSON when it came with `code`; an empty value (and the reply
// logged) otherwise.
crow::json::rvalue json_of(const net::HttpReply &reply, int code) {
	if (!reply.transport_ok || reply.code != code) {
		std::fprintf(stderr, "  %s, want %d\n", describe(reply).c_str(), code);
		return {};
	}
	return crow::json::load(body_text(reply));
}

std::string error_of(const net::HttpReply &reply) {
	const auto json = crow::json::load(body_text(reply));
	return json && json.has("error") ? str(json["error"]) : std::string();
}

int64_t make_account(Database &db, const std::string &name, const char *role) {
	static int next = 0;
	char pcid[16];
	std::snprintf(pcid, sizeof(pcid), "%08x", 0x70b50000u + static_cast<unsigned>(next++));
	nws::CreateUserParams p;
	p.username = name;
	p.password = "pw-" + name;
	p.pcid = pcid;
	p.nwh = "1";
	p.nwhandle = name;
	const auto made = nws::create_user(db, p);
	if (!made.ok)
		return 0;
	db.exec("UPDATE players SET role = $1 WHERE id = $2;",
			{ BindValue(std::string(role)), BindValue(made.id) });
	return made.id;
}

// "Cookie: <name>=<token>" for a session minted for `id`.
std::string session_cookie(Database &db, int64_t id) {
	const auto user = nws::get_user_by_id(db, id);
	if (!user)
		return {};
	const auto token = nws::create_web_session(db, id, user->credentials_gen, "10.9.0.1", "test");
	return token ? std::string("Cookie: ") + nws::kWebSessionCookie + "=" + *token : std::string();
}

jobs::NewJob job_of(const std::string &kind, const std::string &dedupe = {}, int max_attempts = 3) {
	jobs::NewJob j;
	j.kind = kind;
	j.dedupe_key = dedupe;
	j.max_attempts = max_attempts;
	return j;
}

// A job of `kind` claimed by `worker`, so running; its id.
std::optional<jobs::ClaimedJob> claimed(
		Database &db, const jobs::NewJob &job, const std::string &worker) {
	if (jobs::enqueue(db, job) <= 0)
		return std::nullopt;
	return jobs::claim(db, worker, { job.kind }, {}, 60000);
}

int64_t count_jobs(Database &db, const std::string &where = "TRUE") {
	return db.query("SELECT COUNT(*) FROM jobs WHERE " + where + ";")
			.front()
			.as_int(0)
			.value_or(-1);
}

std::string state_of(Database &db, int64_t id) {
	const auto rows = db.query("SELECT state FROM jobs WHERE id = $1;", { BindValue(id) });
	return rows.empty() ? std::string("<gone>") : rows.front().as_text(0).value_or("");
}

void clear_jobs(Harness &h) {
	h.pool.acquire()->exec("DELETE FROM jobs;");
}

// Every route refused without an admin: 401 with no credential, 403 for a
// player's session, 403 csrf_header_required for an admin's state change with
// no CSRF header (an admin's read answers); nothing refused changes a job.
int test_access(Harness &h) {
	clear_jobs(h);
	int64_t queued = 0, failed = 0;
	{
		auto db = h.pool.acquire();
		queued = jobs::enqueue(*db, job_of("test.access"));
		auto run = claimed(*db, job_of("test.failing", {}, 1), "worker-access-0");
		TEST_EXPECT(queued > 0 && run && jobs::fail(*db, *run, "no"));
		failed = run->id;
	}
	struct Route {
		const char *method;
		std::string path;
		const char *body;
		int admin_code; // an admin's session without the CSRF header
	};
	const Route routes[] = {
		{ "GET", "/api/admin/jobs/overview", "", 200 },
		{ "GET", "/api/admin/jobs", "", 200 },
		{ "GET", "/api/admin/jobs/" + std::to_string(queued), "", 200 },
		{ "POST", "/api/admin/jobs/" + std::to_string(failed) + "/retry", "", 403 },
		{ "POST", "/api/admin/jobs/" + std::to_string(queued) + "/cancel", "", 403 },
		{ "POST", "/api/admin/jobs/run", "{\"kind\":\"test.every\"}", 403 },
		{ "POST", "/api/admin/jobs/prune", "{\"keep_days\":1}", 403 },
	};
	for (const Route &r : routes) {
		const auto check = [&r](const net::HttpReply &reply, int code, const char *who) {
			if (reply.transport_ok && reply.code == code)
				return true;
			std::fprintf(stderr, "  %s %s (%s): %s, want %d\n", r.method, r.path.c_str(), who,
					describe(reply).c_str(), code);
			return false;
		};
		TEST_EXPECT(check(h.send(r.method, r.path, { kJson }, r.body), 401, "no credential"));
		TEST_EXPECT(check(h.send(r.method, r.path, { kJson, kCsrf, h.player_cookie }, r.body), 403,
				"a player"));
		const auto admin = h.send(r.method, r.path, { kJson, h.admin_cookie }, r.body);
		TEST_EXPECT(check(admin, r.admin_code, "an admin, no CSRF header"));
		if (r.admin_code == 403)
			TEST_EXPECT(error_of(admin) == "csrf_header_required");
	}
	auto db = h.pool.acquire();
	TEST_EXPECT(count_jobs(*db) == 2);
	TEST_EXPECT(state_of(*db, queued) == "queued" && state_of(*db, failed) == "failed");
	// With the CSRF header an admin's session acts.
	const auto cancelled = h.send("POST", "/api/admin/jobs/" + std::to_string(queued) + "/cancel",
			{ kJson, kCsrf, h.admin_cookie });
	TEST_EXPECT(json_of(cancelled, 200));
	TEST_EXPECT(state_of(*db, queued) == "failed");
	return 0;
}

// The overview: the counts by kind and state, the queue's latency, the last
// hour's minutes, the failed jobs to look at, the running jobs with their
// leases and the workers they name, the schedules and the controller's row;
// and never a secret.
int test_overview(Harness &h) {
	clear_jobs(h);
	int64_t expired = 0, held = 0, failed = 0, cancelled = 0;
	{
		auto db = h.pool.acquire();
		// Two running: one held, one whose lease ran out (its worker went away).
		jobs::NewJob secretive = job_of("test.beta");
		secretive.secret = kSecret;
		auto a = claimed(*db, secretive, "worker-0badcafe-0");
		auto b = claimed(*db, job_of("test.beta"), "worker-0badcafe-1");
		TEST_EXPECT(a && b);
		held = a->id;
		expired = b->id;
		db->exec("UPDATE jobs SET started_at = now() - interval '2 minutes', lease_until = now() - "
				 "interval '30 seconds' WHERE id = $1;",
				{ BindValue(expired) });
		// Four done by another process's worker, each 4 s long, one of them two
		// hours ago (out of the hour, in the day).
		for (int i = 0; i < 4; ++i) {
			auto d = claimed(*db, job_of("test.alpha"), "worker-feedf00d-0");
			TEST_EXPECT(d && jobs::complete(*db, *d));
			db->exec("UPDATE jobs SET started_at = finished_at - interval '4 seconds' WHERE id = "
					 "$1;",
					{ BindValue(d->id) });
			if (i == 3)
				db->exec("UPDATE jobs SET finished_at = now() - interval '2 hours', started_at = "
						 "now() - interval '2 hours 4 seconds' WHERE id = $1;",
						{ BindValue(d->id) });
		}
		// Two queued and due a while (the oldest 90 s), one queued for later
		// (after the claims above, which take the oldest due first).
		const int64_t due_old = jobs::enqueue(*db, job_of("test.alpha"));
		const int64_t due_new = jobs::enqueue(*db, job_of("test.alpha"));
		jobs::NewJob later = job_of("test.alpha");
		later.delay_ms = 10 * 60 * 1000;
		TEST_EXPECT(due_old > 0 && due_new > 0 && jobs::enqueue(*db, later) > 0);
		db->exec("UPDATE jobs SET run_at = now() - interval '90 seconds' WHERE id = $1;",
				{ BindValue(due_old) });
		db->exec("UPDATE jobs SET run_at = now() - interval '5 seconds' WHERE id = $1;",
				{ BindValue(due_new) });
		// One failed for good; one an admin cancelled.
		auto f = claimed(*db, job_of("test.beta", {}, 1), "worker-feedf00d-0");
		TEST_EXPECT(f && jobs::fail(*db, *f, "the cloud said no"));
		failed = f->id;
		cancelled = jobs::enqueue(*db, job_of("test.alpha"));
		TEST_EXPECT(jobs::cancel(*db, cancelled, "token") == jobs::ActionResult::Done);
		// A scheduled jobs.prune that ran, and the controller that leads.
		jobs::NewJob prune = job_of(jobs::kJobJobsPrune, jobs::kJobJobsPrune, 1);
		prune.schedule_key = jobs::Scheduler::schedule_key(
				jobs::kJobJobsPrune, jobs::Scheduler::wall_now_ms(), 60 * 60 * 1000);
		auto p = claimed(*db, prune, "worker-feedf00d-1");
		TEST_EXPECT(p && jobs::complete(*db, *p));
		db->exec("UPDATE controller_leader SET holder = 'controller-1234abcd', generation = "
				 "generation + 1, elected_at = now() - interval '1 hour' WHERE id = 1;");
	}

	const auto reply = h.admin("GET", "/api/admin/jobs/overview");
	const auto o = json_of(reply, 200);
	TEST_EXPECT(o);
	TEST_EXPECT(body_text(reply).find(kSecret) == std::string::npos);
	TEST_EXPECT(str(o["at"]).size() == 20 && str(o["at"]).back() == 'Z');
	const auto &t = o["totals"];
	TEST_EXPECT(t["queued"].i() == 3 && t["due"].i() == 2 && t["running"].i() == 2);
	TEST_EXPECT(t["done_hour"].i() == 4 && t["done_day"].i() == 5); // the prune's among them
	TEST_EXPECT(t["failed_hour"].i() == 2 && t["failed_day"].i() == 2 && t["failed"].i() == 2);
	TEST_EXPECT(t["oldest_wait_seconds"].i() >= 89 && t["oldest_wait_seconds"].i() <= 120);
	bool alpha_seen = false;
	for (const auto &k : o["kinds"]) {
		if (str(k["kind"]) != "test.alpha")
			continue;
		alpha_seen = true;
		TEST_EXPECT(k["queued"].i() == 3 && k["due"].i() == 2 && k["running"].i() == 0);
		TEST_EXPECT(k["done_hour"].i() == 3 && k["done_day"].i() == 4 && k["failed"].i() == 1);
		TEST_EXPECT(k["oldest_wait_seconds"].i() >= 89);
		TEST_EXPECT(k["mean_run_seconds"].d() > 3.9 && k["mean_run_seconds"].d() < 4.1);
	}
	TEST_EXPECT(alpha_seen);
	// The last hour, a minute a point: the finished ones in the newest minute.
	TEST_EXPECT(o["per_minute"].size() == 60);
	int64_t done_minutes = 0, failed_minutes = 0;
	for (const auto &m : o["per_minute"]) {
		done_minutes += m["done"].i();
		failed_minutes += m["failed"].i();
	}
	TEST_EXPECT(done_minutes == 4 && failed_minutes == 2);
	// All of them just now: the newest minute, or the one before it should one
	// have turned meanwhile.
	TEST_EXPECT(o["per_minute"][58]["done"].i() + o["per_minute"][59]["done"].i() == 4);
	// Attention: the failure, not the admin's cancel.
	TEST_EXPECT(o["attention"].size() == 1 && o["attention"][0]["id"].i() == failed);
	TEST_EXPECT(str(o["attention"][0]["last_error"]) == "the cloud said no");
	TEST_EXPECT(o["attention"][0]["retryable"].b());
	// Running: the oldest claim first, its lease run out (negative).
	TEST_EXPECT(o["running"].size() == 2 && o["running"][0]["id"].i() == expired);
	TEST_EXPECT(o["running"][0]["lease_left_seconds"].i() < 0);
	TEST_EXPECT(o["running"][0]["running_seconds"].i() >= 119);
	TEST_EXPECT(str(o["running"][0]["worker_id"]) == "worker-0badcafe-1");
	TEST_EXPECT(
			o["running"][1]["id"].i() == held && o["running"][1]["lease_left_seconds"].i() > 50);
	TEST_EXPECT(str(o["running"][1]["secret_state"]) == "held");
	// Workers: each thread a claim named, its process, what it holds.
	TEST_EXPECT(o["workers"].size() == 4);
	TEST_EXPECT(str(o["workers"][0]["worker_id"]) == "worker-0badcafe-0" &&
			str(o["workers"][0]["process"]) == "worker-0badcafe" &&
			o["workers"][0]["job_id"].i() == held &&
			str(o["workers"][0]["job_kind"]) == "test.beta");
	TEST_EXPECT(str(o["workers"][2]["worker_id"]) == "worker-feedf00d-0" &&
			is_null(o["workers"][2]["job_id"]) && o["workers"][2]["done"].i() == 3 &&
			o["workers"][2]["failed"].i() == 1);
	// Schedules: jobs.prune ran this hour's bucket, so its next is the next hour;
	// test.every never ran, so its bucket is due; test.cond says it is conditional.
	TEST_EXPECT(o["schedules"].size() == 3);
	for (const auto &s : o["schedules"]) {
		const std::string kind = str(s["kind"]);
		if (kind == jobs::kJobJobsPrune) {
			TEST_EXPECT(s["interval_seconds"].i() == 3600 && !s["pending"].b());
			TEST_EXPECT(str(s["last"]["state"]) == "done" &&
					str(s["last_finished"]["state"]) == "done");
			TEST_EXPECT(str(s["next_at"]) > str(o["at"]));
		} else if (kind == "test.every") {
			TEST_EXPECT(is_null(s["last"]) && !s["conditional"].b());
			TEST_EXPECT(str(s["next_at"]) <= str(o["at"]));
		} else {
			TEST_EXPECT(kind == "test.cond" && s["conditional"].b());
		}
	}
	TEST_EXPECT(str(o["controller"]["leader"]) == "controller-1234abcd");
	TEST_EXPECT(!is_null(o["controller"]["elected_at"]) &&
			!is_null(o["controller"]["last_scheduled_at"]));
	return 0;
}

// The list: newest first, each filter, the pages, the refusals.
int test_list(Harness &h) {
	clear_jobs(h);
	std::vector<int64_t> ids;
	{
		auto db = h.pool.acquire();
		for (int i = 0; i < 5; ++i)
			ids.push_back(jobs::enqueue(
					*db, job_of("test.list", "test.list:machine-" + std::to_string(i))));
		auto d = claimed(*db, job_of("test.other"), "worker-11111111-0");
		TEST_EXPECT(d && jobs::complete(*db, *d));
		ids.push_back(d->id);
		jobs::NewJob keyed = job_of("test.other");
		keyed.schedule_key = "test.other@424242";
		ids.push_back(jobs::enqueue(*db, keyed));
		// The first two made a day ago.
		db->exec("UPDATE jobs SET created_at = now() - interval '1 day' WHERE id IN ($1, $2);",
				{ BindValue(ids[0]), BindValue(ids[1]) });
	}
	auto page = json_of(h.admin("GET", "/api/admin/jobs"), 200);
	TEST_EXPECT(page && page["total"].i() == 7 && page["jobs"].size() == 7 &&
			is_null(page["next_before"]));
	TEST_EXPECT(page["jobs"][0]["id"].i() == ids[6] && page["jobs"][6]["id"].i() == ids[0]);
	TEST_EXPECT(!page["jobs"][0].has("payload")); // the detail's alone
	page = json_of(h.admin("GET", "/api/admin/jobs?state=done"), 200);
	TEST_EXPECT(page && page["total"].i() == 1 && page["jobs"][0]["id"].i() == ids[5]);
	TEST_EXPECT(str(page["jobs"][0]["worker_id"]) == "worker-11111111-0");
	page = json_of(h.admin("GET", "/api/admin/jobs?kind=test.other"), 200);
	TEST_EXPECT(page && page["total"].i() == 2);
	// The key search: any case, within the dedupe or the schedule key, a
	// wildcard taken literally.
	page = json_of(h.admin("GET", "/api/admin/jobs?q=MACHINE-3"), 200);
	TEST_EXPECT(page && page["total"].i() == 1 && page["jobs"][0]["id"].i() == ids[3]);
	page = json_of(h.admin("GET", "/api/admin/jobs?q=424242"), 200);
	TEST_EXPECT(page && page["total"].i() == 1 && page["jobs"][0]["id"].i() == ids[6]);
	page = json_of(h.admin("GET", "/api/admin/jobs?q=%25"), 200);
	TEST_EXPECT(page && page["total"].i() == 0);
	// The time range on when each was made.
	const int64_t now_s = jobs::Scheduler::wall_now_ms() / 1000;
	page = json_of(h.admin("GET", "/api/admin/jobs?from=" + std::to_string(now_s - 3600)), 200);
	TEST_EXPECT(page && page["total"].i() == 5);
	page = json_of(h.admin("GET", "/api/admin/jobs?to=" + std::to_string(now_s - 3600)), 200);
	TEST_EXPECT(page && page["total"].i() == 2);
	page = json_of(h.admin("GET",
						   "/api/admin/jobs?kind=test.list&from=" + std::to_string(now_s - 3600) +
								   "&to=" + std::to_string(now_s + 60)),
			200);
	TEST_EXPECT(page && page["total"].i() == 3);
	// Pages of three: the cursor, then the last page.
	page = json_of(h.admin("GET", "/api/admin/jobs?limit=3"), 200);
	TEST_EXPECT(page && page["jobs"].size() == 3 && page["total"].i() == 7);
	TEST_EXPECT(page["next_before"].i() == ids[4]);
	page = json_of(h.admin("GET", "/api/admin/jobs?limit=3&before=" + std::to_string(ids[4])), 200);
	TEST_EXPECT(page && page["jobs"].size() == 3 && page["jobs"][0]["id"].i() == ids[3]);
	page = json_of(h.admin("GET", "/api/admin/jobs?limit=3&before=" + std::to_string(ids[1])), 200);
	TEST_EXPECT(page && page["jobs"].size() == 1 && is_null(page["next_before"]));
	// Refusals.
	for (const char *bad : { "?state=lost", "?limit=0", "?limit=201", "?from=yesterday",
				 "?before=-1", "?to=1.5" }) {
		const auto reply = h.admin("GET", std::string("/api/admin/jobs") + bad);
		TEST_EXPECT(reply.code == 400 && error_of(reply) == "invalid_field");
	}
	const auto long_kind = h.admin("GET", "/api/admin/jobs?kind=" + std::string(129, 'k'));
	TEST_EXPECT(long_kind.code == 400 && error_of(long_kind) == "invalid_field");
	return 0;
}

// One job's detail: its payload and every time, never its secret.
int test_detail(Harness &h) {
	clear_jobs(h);
	int64_t id = 0;
	{
		auto db = h.pool.acquire();
		jobs::NewJob j = job_of("test.detail", "test.detail:7");
		j.payload.set("machine_id", opennova::io::json_number(7));
		j.secret = kSecret;
		id = jobs::enqueue(*db, j);
	}
	auto reply = h.admin("GET", "/api/admin/jobs/" + std::to_string(id));
	auto json = json_of(reply, 200);
	TEST_EXPECT(json && body_text(reply).find(kSecret) == std::string::npos);
	const auto &job = json["job"];
	TEST_EXPECT(str(job["kind"]) == "test.detail" && str(job["state"]) == "queued");
	TEST_EXPECT(job["payload"]["machine_id"].i() == 7);
	TEST_EXPECT(str(job["secret_state"]) == "held" && !job.has("secret"));
	TEST_EXPECT(str(job["dedupe_key"]) == "test.detail:7" && is_null(job["schedule_key"]));
	TEST_EXPECT(job["attempts"].i() == 0 && job["max_attempts"].i() == 3);
	TEST_EXPECT(job["cancellable"].b() && !job["retryable"].b() && !job["manual"].b());
	TEST_EXPECT(
			is_null(job["started_at"]) && is_null(job["lease_until"]) && is_null(job["worker_id"]));
	// Run and failed for good: its secret dropped, which the detail says.
	{
		auto db = h.pool.acquire();
		auto run = jobs::claim(*db, "worker-22222222-0", { "test.detail" }, {}, 60000);
		TEST_EXPECT(run && jobs::fail(*db, *run, "gave up"));
	}
	reply = h.admin("GET", "/api/admin/jobs/" + std::to_string(id));
	json = json_of(reply, 200);
	TEST_EXPECT(json && str(json["job"]["secret_state"]) == "dropped");
	TEST_EXPECT(!json["job"]["retryable"].b() && str(json["job"]["last_error"]) == "gave up");
	TEST_EXPECT(str(json["job"]["finished_at"]).back() == 'Z');
	reply = h.admin("GET", "/api/admin/jobs/999999999");
	TEST_EXPECT(reply.code == 404 && error_of(reply) == "not_found");
	return 0;
}

// The retry: a failed job queued again now with one more attempt; refused for
// a job not failed, one whose secret went, one whose dedupe key another holds.
int test_retry(Harness &h) {
	clear_jobs(h);
	int64_t failed = 0, dropped = 0, keyed = 0, queued = 0;
	{
		auto db = h.pool.acquire();
		auto f = claimed(*db, job_of("test.retry", {}, 1), "worker-33333333-0");
		TEST_EXPECT(f && jobs::fail(*db, *f, "first try failed"));
		failed = f->id;
		jobs::NewJob secretive = job_of("test.retry", {}, 1);
		secretive.secret = kSecret;
		auto s = claimed(*db, secretive, "worker-33333333-0");
		TEST_EXPECT(s && jobs::fail(*db, *s, "mail refused"));
		dropped = s->id;
		auto k = claimed(*db, job_of("test.retry", "test.retry:key", 1), "worker-33333333-0");
		TEST_EXPECT(k && jobs::fail(*db, *k, "keyed failed"));
		keyed = k->id;
		// Another kind holding the same dedupe key, queued.
		queued = jobs::enqueue(*db, job_of("test.keyholder", "test.retry:key"));
		TEST_EXPECT(queued > 0);
	}
	auto reply = h.admin("POST", "/api/admin/jobs/" + std::to_string(failed) + "/retry");
	auto json = json_of(reply, 200);
	TEST_EXPECT(json && str(json["job"]["state"]) == "queued");
	TEST_EXPECT(json["job"]["attempts"].i() == 1 && json["job"]["max_attempts"].i() == 2);
	TEST_EXPECT(str(json["job"]["last_error"]) == "first try failed" &&
			is_null(json["job"]["finished_at"]));
	{
		auto db = h.pool.acquire();
		TEST_EXPECT(
				count_jobs(*db, "id = " + std::to_string(failed) + " AND run_at <= now()") == 1);
		// A worker takes it as the next due job of its kind, its attempt counted.
		auto again = jobs::claim(*db, "worker-33333333-1", { "test.retry" }, {}, 60000);
		TEST_EXPECT(again && again->id == failed && again->attempts == 2);
		TEST_EXPECT(jobs::complete(*db, *again) && state_of(*db, failed) == "done");
	}
	// Done now: not failed.
	reply = h.admin("POST", "/api/admin/jobs/" + std::to_string(failed) + "/retry");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "not_failed");
	TEST_EXPECT(str(crow::json::load(body_text(reply))["state"]) == "done");
	// Queued: not failed, and untouched.
	reply = h.admin("POST", "/api/admin/jobs/" + std::to_string(queued) + "/retry");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "not_failed");
	// Its secret went as it ended: it cannot run again.
	reply = h.admin("POST", "/api/admin/jobs/" + std::to_string(dropped) + "/retry");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "secret_dropped");
	TEST_EXPECT(body_text(reply).find(kSecret) == std::string::npos);
	// Its dedupe key is the queued one's: refused while that one waits.
	reply = h.admin("POST", "/api/admin/jobs/" + std::to_string(keyed) + "/retry");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "already_pending");
	reply = h.admin("POST", "/api/admin/jobs/999999999/retry");
	TEST_EXPECT(reply.code == 404 && error_of(reply) == "not_found");
	auto db = h.pool.acquire();
	TEST_EXPECT(state_of(*db, dropped) == "failed" && state_of(*db, keyed) == "failed");
	TEST_EXPECT(state_of(*db, queued) == "queued");
	return 0;
}

// The cancel: a queued job failed with the admin's name; a running one is its
// worker's (refused, its claim still finishing), a done one refused.
int test_cancel(Harness &h) {
	clear_jobs(h);
	int64_t queued = 0;
	std::optional<jobs::ClaimedJob> running;
	{
		auto db = h.pool.acquire();
		jobs::NewJob secretive = job_of("test.cancel");
		secretive.secret = kSecret;
		queued = jobs::enqueue(*db, secretive);
		running = claimed(*db, job_of("test.running"), "worker-44444444-0");
		TEST_EXPECT(queued > 0 && running);
	}
	auto reply = h.send("POST", "/api/admin/jobs/" + std::to_string(queued) + "/cancel",
			{ kJson, kCsrf, h.admin_cookie });
	auto json = json_of(reply, 200);
	TEST_EXPECT(json && str(json["job"]["state"]) == "failed");
	TEST_EXPECT(str(json["job"]["last_error"]) == "cancelled by an admin (session 'jobs_admin')");
	TEST_EXPECT(str(json["job"]["secret_state"]) == "dropped");
	TEST_EXPECT(body_text(reply).find(kSecret) == std::string::npos);
	reply = h.admin("POST", "/api/admin/jobs/" + std::to_string(queued) + "/cancel");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "not_queued");
	reply = h.admin("POST", "/api/admin/jobs/" + std::to_string(running->id) + "/cancel");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "not_queued");
	TEST_EXPECT(str(crow::json::load(body_text(reply))["state"]) == "running");
	{
		auto db = h.pool.acquire();
		TEST_EXPECT(jobs::complete(*db, *running) && state_of(*db, running->id) == "done");
	}
	reply = h.admin("POST", "/api/admin/jobs/999999999/cancel");
	TEST_EXPECT(reply.code == 404 && error_of(reply) == "not_found");
	return 0;
}

// Run now: a periodic kind enqueued with its manual key once, then refused
// while it waits; a kind no schedule names refused. The prune: a jobs.prune
// with its keep, which a worker then runs.
int test_run_and_prune(Harness &h) {
	clear_jobs(h);
	auto reply = h.admin("POST", "/api/admin/jobs/run", "{\"kind\":\"test.every\"}");
	auto json = json_of(reply, 202);
	TEST_EXPECT(json && str(json["job"]["kind"]) == "test.every" && json["job"]["manual"].b());
	TEST_EXPECT(str(json["job"]["dedupe_key"]) == "test.every");
	TEST_EXPECT(str(json["job"]["schedule_key"]).rfind("test.every@manual-", 0) == 0);
	TEST_EXPECT(json["job"]["max_attempts"].i() == 2);
	reply = h.admin("POST", "/api/admin/jobs/run", "{\"kind\":\"test.every\"}");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "already_pending");
	reply = h.admin("POST", "/api/admin/jobs/run", "{\"kind\":\"machine.launch\"}");
	TEST_EXPECT(reply.code == 404 && error_of(reply) == "not_found");
	reply = h.admin("POST", "/api/admin/jobs/run", "{}");
	TEST_EXPECT(reply.code == 400 && error_of(reply) == "missing_field");
	reply = h.admin("POST", "/api/admin/jobs/run", "{\"kind\":7}");
	TEST_EXPECT(reply.code == 400 && error_of(reply) == "invalid_field");
	reply = h.admin("POST", "/api/admin/jobs/run", "[1]");
	TEST_EXPECT(reply.code == 400 && error_of(reply) == "invalid_json");
	// The overview's schedule says one is pending.
	json = json_of(h.admin("GET", "/api/admin/jobs/overview"), 200);
	for (const auto &s : json["schedules"]) {
		if (str(s["kind"]) == "test.every")
			TEST_EXPECT(s["pending"].b() && s["last"]["manual"].b());
	}

	// Two finished jobs, two days and two hours old; the prune keeps a day.
	{
		auto db = h.pool.acquire();
		for (int i = 0; i < 2; ++i) {
			auto d = claimed(*db, job_of("test.old"), "worker-55555555-0");
			TEST_EXPECT(d && jobs::complete(*db, *d));
		}
		db->exec("UPDATE jobs SET finished_at = now() - CASE WHEN id = (SELECT MIN(id) FROM jobs "
				 "WHERE kind = 'test.old') THEN interval '2 days' ELSE interval '2 hours' END "
				 "WHERE kind = 'test.old';");
	}
	for (const char *bad : { "{\"keep_days\":0}", "{\"keep_days\":15}", "{\"keep_days\":\"1\"}",
				 "{\"keep_days\":1.5}" }) {
		reply = h.admin("POST", "/api/admin/jobs/prune", bad);
		TEST_EXPECT(reply.code == 400 && error_of(reply) == "invalid_field");
	}
	reply = h.admin("POST", "/api/admin/jobs/prune", "{\"keep_days\":1}");
	json = json_of(reply, 202);
	TEST_EXPECT(json && str(json["job"]["kind"]) == jobs::kJobJobsPrune);
	TEST_EXPECT(json["job"]["payload"]["keep_s"].i() == 86400);
	reply = h.admin("POST", "/api/admin/jobs/prune");
	TEST_EXPECT(reply.code == 409 && error_of(reply) == "already_pending");
	jobs::JobRegistry registry;
	jobs::register_maintenance_jobs(registry, {});
	TEST_EXPECT(jobs::run_due(h.pool, registry) == 1);
	auto db = h.pool.acquire();
	TEST_EXPECT(count_jobs(*db, "kind = 'test.old'") == 1);
	// Done, the next prune (its default keep, 14 days) is taken.
	reply = h.admin("POST", "/api/admin/jobs/prune");
	json = json_of(reply, 202);
	TEST_EXPECT(json && json["job"]["payload"]["keep_s"].i() == 14 * 86400);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("job_routes");
		test_db::TestDatabase database({ /*migrate=*/true, /*seed_games=*/true });
		ConnectionPool pool(database.options());
		test_redis::TestRedis redis;

		nws::ServerConfig config;
		config.public_host = "127.0.0.1";
		config.web_host = "127.0.0.1";
		config.admin_api_token = kAdminToken;
		config.http_port = 0;
		config.web_dist_dir = temp.path / "web_dist";
		config.templates_dir = temp.path / "templates";
		config.static_dir = temp.path / "static";
		std::filesystem::create_directories(config.templates_dir);

		// The deployment's schedule, as the controller's would be: the hourly
		// jobs.prune, a kind every minute, and one every minute while its test
		// holds.
		std::vector<jobs::PeriodicJob> schedule;
		jobs::PeriodicJob prune;
		prune.kind = jobs::kJobJobsPrune;
		prune.period_ms = 60 * 60 * 1000;
		schedule.push_back(prune);
		jobs::PeriodicJob every;
		every.kind = "test.every";
		every.period_ms = 60 * 1000;
		every.max_attempts = 2;
		schedule.push_back(every);
		jobs::PeriodicJob cond;
		cond.kind = "test.cond";
		cond.period_ms = 60 * 1000;
		cond.wanted = [](Database &) { return false; };
		schedule.push_back(cond);

		nws::HttpListener http(pool, redis.pool());
		http.set_job_schedules(schedule);
		TEST_EXPECT(http.start(config));
		TEST_EXPECT(http.bound_port() != 0);

		Harness harness{ pool, http.bound_port(), {}, {} };
		{
			auto db = pool.acquire();
			const int64_t admin = make_account(*db, "jobs_admin", "admin");
			const int64_t player = make_account(*db, "jobs_player", "player");
			TEST_EXPECT(admin > 0 && player > 0);
			harness.admin_cookie = session_cookie(*db, admin);
			harness.player_cookie = session_cookie(*db, player);
			TEST_EXPECT(!harness.admin_cookie.empty() && !harness.player_cookie.empty());
		}

		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "access", test_access },
			{ "overview", test_overview },
			{ "list", test_list },
			{ "detail", test_detail },
			{ "retry", test_retry },
			{ "cancel", test_cancel },
			{ "run_and_prune", test_run_and_prune },
		};
		rc = 0;
		for (const Case &c : cases) {
			std::printf("-- %s\n", c.name);
			if (c.fn(harness) != 0) {
				std::fprintf(stderr, "FAIL: %s\n", c.name);
				rc = 1;
				break;
			}
		}
		http.stop();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: the admin's background jobs routes\n");
	return rc;
}
