#pragma once

// What needs an admin, in one list (apps/novaworld_common/hosted/README.md "The fleet's
// health"): GET /api/admin/attention, require_admin, the fleet's health read from the rows
// at each request (hosted/model/fleet_health.h: the same findings the fleet leader's heartbeat
// counts) and the deployment's CloudWatch alarms that fire now (alarm_store.h, kept 20 s):
//
//   {"findings": [{"level" (danger, warn, info), "subject", "text", "since" (ISO 8601 UTC;
//      null when not known), "link" (the admin page that shows it; null for none),
//      "source" (the rule)}] (danger first, then warn, then info, each level's oldest
//      first),
//    "alarms": [{"name", "state_since" (ISO 8601; null when not known), "description"}],
//    "alarms_read" (false where none are read: the local stack, or a read that failed,
//      which is a warn finding of its own, never a quiet list),
//    "checked_at" (ISO 8601, the database's clock)}
//
// Each admin's reads are drawn from a bucket of their own. Read only; no-store. Service
// policy, not a port.

#include "alarm_store.h"
#include "hosted/model/fleet_health.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

namespace opennova::novaworld_server::infra {

class AttentionRoutes {
public:
	// Each admin's reads: 30 at once, then one a second (the admin pages poll it every
	// 60 s, the nav badge with them).
	static constexpr RateLimiter::Params kReadsPerAdmin{ "attention_reads", 30, 1.0 };

	AttentionRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis) :
			access_(access), pool_(pool), reads_(redis, kReadsPerAdmin) {}
	AttentionRoutes(const AttentionRoutes &) = delete;
	AttentionRoutes &operator=(const AttentionRoutes &) = delete;

	// The alarms' reader (null: none; it must outlive the routes), whether hosting is on
	// with the fleet's placement policy (what is sold is judged against it), and the
	// images the release pins. Before register_routes().
	void configure(AlarmStore *alarms, bool hosting_on, const hosted::PlacementPolicy &policy,
			hosted::ReleaseImages release);
	void register_routes(WebApp &app);

private:
	crow::response attention(const crow::request &req);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	RateLimiter reads_;
	AlarmStore *alarms_ = nullptr;
	bool hosting_on_ = false;
	hosted::PlacementPolicy policy_;
	hosted::ReleaseImages release_;
};

} // namespace opennova::novaworld_server::infra
