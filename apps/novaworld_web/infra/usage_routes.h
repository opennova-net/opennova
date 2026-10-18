#pragma once

// What everything uses, for an admin (apps/novaworld_common/README.md
// "Infrastructure"), in one shape for every subject:
//
//   GET /api/admin/usage?subject=server|machine|program|infra&id=<id>&range=48h|400d
//       [&task=<task id>] (a program's one task)
//     a hosted server (hosted_server_samples, _hourly: its container's CPU of its
//     slot's, its memory, its slot's network in and out, its tick health), a
//     machine (fleet_machine_samples, _hourly: its CPU, memory, network and disk),
//     a program's tasks together or one of them (program_samples, program_hourly,
//     program_usage.h), each at 1-minute points for 48 h or hourly ones for 400
//     days with its limits and its newest minute; or an AWS resource (rds, cache,
//     nlb) as CloudWatch keeps its metrics (metrics_store.h; none on the local
//     stack, which the reply says)
//   GET /api/admin/programs
//     every program the service runs: its tasks running and desired and the
//     revision it runs (ECS's, through infra_status.h's snapshot; none on the
//     local stack), what its reporting tasks use now, its tasks gone in 24 h,
//     and each task
//
// The byte counters the samples keep are cumulative: a rate is the growth
// between two minutes over the time between them, none across a gap past
// kGapSeconds or where a counter went back (it started over). An hour's bytes
// are the hour's (the rate, over the hour or what of it has passed). Each
// admin's reads are drawn from a bucket of their own. Read only; no-store.
// Service policy, not a port.

#include "infra_status.h"
#include "metrics_store.h"
#include "rate_limiter.h"
#include "web_access.h"

#include <net/novaworld/db/database.h>

#include <crow.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::infra {

// Two minutes further apart than this give no rate between them.
inline constexpr int64_t kRateGapSeconds = 5 * 60;

// A cumulative counter's rate since its last reading, one counter's readings
// in order (Unix seconds, its bytes): none for the first, across a gap past
// kRateGapSeconds or where it went back (it started over). The usage series'
// and the machine JSON's (hosted/fleet_routes.h) one rate.
class CounterRate {
public:
	std::optional<double> next(int64_t at, std::optional<int64_t> bytes);

private:
	std::optional<int64_t> prev_;
	int64_t prev_at_ = 0;
};

// A machine's network in and out, bytes a second, between its two newest
// minutes (fleet_machine_samples); none without such a pair. Throws db::Error.
struct NetRates {
	std::optional<double> in_bps;
	std::optional<double> out_bps;
};
NetRates machine_net_rates(db::Database &db, int64_t machine_id);

class UsageRoutes {
public:
	// Each admin's usage reads: 60 at once, then two a second (a page polls a
	// few charts).
	static constexpr RateLimiter::Params kReadsPerAdmin{ "usage_reads", 60, 2.0 };
	// A subject whose newest minute is at most this old reports now.
	static constexpr int64_t kLiveSeconds = 3 * 60;
	// Two minutes further apart than this give no rate between them.
	static constexpr int64_t kGapSeconds = kRateGapSeconds;

	UsageRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis);
	UsageRoutes(const UsageRoutes &) = delete;
	UsageRoutes &operator=(const UsageRoutes &) = delete;

	// The cloud's readers (each may be null; they must outlive the routes) and
	// each hosted container's limits (ONNET_FLEET_SLOT_MEMORY_MB, _CPUS). Before
	// register_routes().
	void configure(
			InfraStatus *status, MetricsStore *metrics, int slot_memory_mb, double slot_cpus);
	void register_routes(WebApp &app);

private:
	crow::response usage(const crow::request &req);
	crow::response programs(const crow::request &req);
	// The cloud as the infrastructure view reads it (the same machines asked
	// about, so its cache serves both); null without a reader.
	std::shared_ptr<const CloudSnapshot> cloud(db::Database &db);

	WebAccess &access_;
	db::ConnectionPool &pool_;
	RateLimiter reads_;
	InfraStatus *status_ = nullptr;
	MetricsStore *metrics_ = nullptr;
	int64_t slot_memory_bytes_ = 512LL * 1024 * 1024;
	double slot_cpus_ = 1.0;
	// The programs the service runs (infra_status.h's list).
	const std::vector<std::string> programs_;
};

} // namespace opennova::novaworld_server::infra
