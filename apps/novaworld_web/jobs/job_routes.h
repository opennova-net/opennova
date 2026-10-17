#pragma once

// The admin's background jobs routes (README "Background jobs", the site's
// admin page /admin/jobs), over job_admin.h: GET /api/admin/jobs/overview
// (the counts by kind, the queue's latency, the failures to look at grouped
// by kind and failure key, the worker processes with their threads and
// leases, the schedules and the controller that runs them), GET
// /api/admin/jobs (a page of jobs by state, kind, time, key and failure,
// newest first), GET /api/admin/jobs/<id> (one job with its payload), and the
// actions: POST /api/admin/jobs/<id>/retry (a failed job, queued again now
// with one more attempt), POST /api/admin/jobs/retry-group (each job of a
// failure group, so), /<id>/cancel (a queued job), POST /api/admin/jobs/run
// (a periodic kind now) and POST /api/admin/jobs/prune (the finished jobs
// past a keep, as a jobs.prune job). Every one
// require_admin (a state change with the CSRF header unless the Bearer token
// carries it, and logged with its actor by AccessMiddleware, and here with
// what it did); they read the jobs table and the controller's row alone, so
// any web replica answers alike. Never a job's secret: only whether it holds
// one, or held one. Every refusal is an error code with our own message,
// never database text. The HttpListener owns one.

#include "job_admin.h"
#include "jobs/scheduler.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <vector>

namespace opennova::novaworld_server::jobs {

class JobRoutes {
public:
	// The keeps POST /api/admin/jobs/prune takes, in days.
	static constexpr int64_t kPruneKeepDaysMin = 1;
	static constexpr int64_t kPruneKeepDaysMax = kFinishedKeepS / (24 * 60 * 60);

	JobRoutes(WebAccess &access, db::ConnectionPool &pool) : access_(access), pool_(pool) {}
	JobRoutes(const JobRoutes &) = delete;
	JobRoutes &operator=(const JobRoutes &) = delete;

	// The periodic jobs this deployment's controller schedules (its settings
	// read alike, parts.h periodic_jobs): what the schedules show and the
	// kinds POST /api/admin/jobs/run takes. Before register_routes().
	void set_schedules(std::vector<PeriodicJob> schedules) { schedules_ = std::move(schedules); }
	void register_routes(WebApp &app);

private:
	crow::response overview_route(const crow::request &req);
	crow::response list_route(const crow::request &req);
	crow::response job_route(const crow::request &req, uint64_t id);
	crow::response retry_route(const crow::request &req, uint64_t id);
	crow::response retry_group_route(const crow::request &req);
	crow::response dismiss_group_route(const crow::request &req);
	crow::response cancel_route(const crow::request &req, uint64_t id);
	crow::response run_route(const crow::request &req);
	crow::response prune_route(const crow::request &req);
	// The periodic job of `kind`; nullptr for none.
	const PeriodicJob *schedule_of(const std::string &kind) const;

	WebAccess &access_;
	db::ConnectionPool &pool_;
	std::vector<PeriodicJob> schedules_;
};

} // namespace opennova::novaworld_server::jobs
