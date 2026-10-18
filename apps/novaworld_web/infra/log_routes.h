#pragma once

// Every log on the site, for an admin (apps/novaworld_common/README.md
// "Infrastructure"): GET /api/admin/logs, require_admin, a page of one log from
// the store ONNET_LOG_STORE names (log_store.h):
//
//   ?subject=server&id=<server id>[&generation=<g>]   a hosted server's start (its
//                                                     newest without a generation)
//   ?subject=machine&id=<machine id>                  a machine's hostd
//   ?subject=program&id=<program>[&task=<task id>]    a program's task, or every task
//
// with &before=<cursor> (an earlier page's), &limit=<1..500> (200) and &q=<text>
// (the lines holding it). What the store reads comes from rows: the server's start
// and the stream it names (hosted_server_starts), the machine's (fleet_machines
// agent_log_stream), a program the service runs and a task it has sampled
// (program_samples); a request names no group, stream or path. Each admin's reads
// are drawn from a bucket of their own, at most kInFlightPerAdmin at once. Every
// refusal is an error code with our own message; AWS's own error text reaches the
// page (it names an action and a resource, never a credential). Read only.
// Service policy, not a port.

#include "log_store.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::infra {

class LogRoutes {
public:
	// Each admin's log reads: 60 at once, then two a second (a viewer follows
	// one log every 5 s).
	static constexpr RateLimiter::Params kReadsPerAdmin{ "log_reads", 60, 2.0 };
	// An admin's reads in flight at once at most (each may take a CloudWatch
	// read's deadline).
	static constexpr int kInFlightPerAdmin = 2;
	static constexpr size_t kDefaultLimit = 200;
	static constexpr size_t kMaxQueryBytes = 200;

	LogRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis);
	LogRoutes(const LogRoutes &) = delete;
	LogRoutes &operator=(const LogRoutes &) = delete;

	// The store (it must outlive the routes), or null with why there is none.
	// Before register_routes().
	void configure(LogStore *store, std::string none_why);
	void register_routes(WebApp &app);

private:
	crow::response logs(const crow::request &req);
	// Takes one of the caller's in-flight places; false when it holds every one.
	bool enter(const std::string &key);
	void leave(const std::string &key);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	RateLimiter reads_;
	LogStore *store_ = nullptr;
	std::string none_why_ = "no log store";
	// The programs the service runs (infra_status.h's list).
	const std::vector<std::string> programs_;
	std::mutex mu_;
	std::map<std::string, int> in_flight_;
};

} // namespace opennova::novaworld_server::infra
