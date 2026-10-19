#pragma once

// The deployment's AWS bill this month, for an admin (apps/novaworld_common/README.md
// "AWS costs"): GET /api/admin/costs, require_admin, from the cost store (cost_store.h,
// Cost Explorer and Budgets, kept 6 h):
//
//   {"available" (false where AWS is not read: the local stack), "unavailable" (why;
//    null when read), "error" (why Cost Explorer's read failed; null when it did not),
//    "read_at" (ISO 8601, when web read AWS; null for none), "currency" ("USD"),
//    "month": {"start", "end"} (YYYY-MM-DD, UTC, the end exclusive), "through" (the
//    last day read, today), "estimated" (Cost Explorer's figures are still open),
//    "month_to_date_cents", "forecast_cents" (the month's expected total; null for
//    none), "forecast_error" (why none; null), "by_service": [{"service", "cents"}]
//    (largest first), "days": [{"date", "cents"}] (every day of the month so far),
//    "budget": {"name", "currency", "limit_cents", "actual_cents", "forecast_cents"
//    (AWS Budgets' own, null until computed)} (null for none), "budget_error" (null)}
//
// Each admin's reads are drawn from a bucket of their own. Read only; no-store. Service
// policy, not a port.

#include "cost_store.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <crow.h>

namespace opennova::novaworld_server::infra {

class CostRoutes {
public:
	// Each admin's reads: 30 at once, then one a second (the overview polls it every
	// 30 s).
	static constexpr RateLimiter::Params kReadsPerAdmin{ "cost_reads", 30, 1.0 };

	CostRoutes(WebAccess &access, RedisPool &redis) :
			access_(access), reads_(redis, kReadsPerAdmin) {}
	CostRoutes(const CostRoutes &) = delete;
	CostRoutes &operator=(const CostRoutes &) = delete;

	// The costs' reader (null: none, which the reply says; it must outlive the routes).
	// Before register_routes().
	void configure(CostStore *store) { store_ = store; }
	void register_routes(WebApp &app);

private:
	crow::response costs(const crow::request &req);

	WebAccess &access_;
	RateLimiter reads_;
	CostStore *store_ = nullptr;
};

} // namespace opennova::novaworld_server::infra
