#pragma once

// The deployment's AWS resources' own metrics, for the admin's usage view
// (usage_routes.h, GET /api/admin/usage?subject=infra): the free vendor metrics
// CloudWatch keeps for them, read with web's task role through aws_api.h
// (cloudwatch:GetMetricData, a Query API: the form in, XML back), one call per
// resource and range, kept for kCacheMs however many admins look:
//
//   rds     AWS/RDS of the database (ONNET_INFRA_DATABASE): CPUUtilization,
//           FreeableMemory, DatabaseConnections, NetworkReceiveThroughput and
//           NetworkTransmitThroughput (bytes a second), ReadIOPS, WriteIOPS
//   cache   AWS/ElastiCache of the cache's primary node (ONNET_INFRA_CACHE's,
//           <group>-001 unless the caller names the primary): CPUUtilization,
//           EngineCPUUtilization, BytesUsedForCache,
//           DatabaseMemoryUsagePercentage, NetworkBytesIn and NetworkBytesOut
//           (bytes a period), CurrConnections
//   nlb     AWS/NetworkELB of the load balancer (ONNET_INFRA_LOAD_BALANCER, its
//           ARN suffix): ProcessedBytes (bytes a period), ActiveFlowCount, and
//           HealthyHostCount:<listener> of each target group
//           (ONNET_INFRA_TARGET_GROUPS)
//
// over 48 hours at 1-minute points or 400 days at 1-hour points (what CloudWatch
// keeps at each: 15 and 455 days). Each metric's statistic is the one its
// meaning takes (an Average, a Sum, a Minimum: kStatistics). Without AWS (the
// local stack) nothing is called and the reply says why. Service policy, not a
// port.

#include "aws_api.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::infra {

struct MetricPoint {
	int64_t at = 0; // Unix seconds, the period's start
	double value = 0;
};

struct MetricSeries {
	std::string name; // AWS's metric name (HealthyHostCount:<listener> for a group's)
	std::string statistic; // Average, Sum, Minimum
	std::vector<MetricPoint> points; // oldest first
};

struct ResourceMetrics {
	std::string resource; // rds, cache, nlb
	bool hourly = false;
	int64_t period_seconds = 60;
	std::vector<MetricSeries> metrics; // in the order above
	std::string unavailable; // why the cloud is not read here; "" when it is
	std::string error; // why the read failed (AWS's words)
};

struct MetricsConfig {
	AwsApiConfig aws;
	std::string database; // ONNET_INFRA_DATABASE
	std::string cache; // ONNET_INFRA_CACHE
	std::string load_balancer; // ONNET_INFRA_LOAD_BALANCER: net/<name>/<id>
	std::vector<std::pair<std::string, std::string>> target_groups; // listener -> ARN
};

// The settings from the infrastructure view's (infra_config_from_env, the
// target groups and the resources) and ONNET_INFRA_LOAD_BALANCER.
MetricsConfig metrics_config_from_env(const std::string &scratch_dir);

class MetricsStore {
public:
	static constexpr int64_t kCacheMs = 60 * 1000;
	static constexpr int64_t kMinuteRangeSeconds = 48 * 60 * 60;
	static constexpr int64_t kHourRangeSeconds = 400LL * 24 * 60 * 60;
	// GetMetricData pages one read follows at most.
	static constexpr int kPages = 4;

	// Milliseconds: the wall clock (the range's end) and a steady one (the cache).
	using Clock = std::function<int64_t()>;

	// `runner` must outlive the store.
	MetricsStore(proc::CommandRunner &runner, MetricsConfig config, Clock wall_ms = {},
			Clock steady_ms = {});
	MetricsStore(const MetricsStore &) = delete;
	MetricsStore &operator=(const MetricsStore &) = delete;

	// "" when the cloud is read here, else why not (no region or task role, the
	// local stack's).
	const std::string &unavailable() const { return unavailable_; }

	// `resource`'s metrics (rds, cache or nlb; another is none: false) over the
	// minute or the hour range; `cache_node` the cache's primary node where the
	// caller knows it. The last read within kCacheMs is answered again.
	bool read(const std::string &resource, bool hourly, const std::string &cache_node,
			ResourceMetrics &out);

private:
	ResourceMetrics read_uncached(
			const std::string &resource, bool hourly, const std::string &cache_node);

	const MetricsConfig config_;
	const std::string unavailable_;
	AwsApi api_;
	Clock wall_;
	Clock steady_;
	std::mutex mu_;
	struct Cached {
		int64_t at = 0;
		ResourceMetrics metrics;
	};
	std::map<std::string, Cached> cache_;
};

} // namespace opennova::novaworld_server::infra
