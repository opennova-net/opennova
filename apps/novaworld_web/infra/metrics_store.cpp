#include "metrics_store.h"

#include "aws_xml.h"
#include "health.h" // Watchdog::steady_now_ms
#include "hosted/stats_repository.h" // iso_utc
#include "infra_status.h"
#include "loggable.h"
#include "text_util.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

namespace opennova::novaworld_server::infra {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// One metric a read asks for: its namespace, name, statistic and dimensions.
struct Wanted {
	std::string name; // the reply's
	std::string metric;
	std::string statistic;
	std::vector<std::pair<std::string, std::string>> dimensions;
};

int64_t wall_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// An ARN's resource part after its last ':' ("targetgroup/<name>/<id>"), a
// metric's dimension value.
std::string arn_resource(const std::string &arn) {
	const size_t colon = arn.rfind(':');
	return colon == std::string::npos ? arn : arn.substr(colon + 1);
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's days_from_civil).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const auto yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// CloudWatch's "2026-10-18T12:00:00Z" (a fraction allowed) as Unix seconds; -1
// for text that is none.
int64_t iso_seconds(std::string_view t) {
	if (t.size() < 20 || t[4] != '-' || t[7] != '-' || t[10] != 'T' || t[13] != ':' ||
			t[16] != ':' || t.back() != 'Z')
		return -1;
	for (const size_t i : { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 })
		if (t[i] < '0' || t[i] > '9')
			return -1;
	const auto num = [&t](size_t at, size_t len) {
		int64_t v = 0;
		for (size_t i = at; i < at + len; ++i)
			v = v * 10 + (t[i] - '0');
		return v;
	};
	const int64_t month = num(5, 2);
	const int64_t day = num(8, 2);
	if (month < 1 || month > 12 || day < 1 || day > 31)
		return -1;
	const int64_t days =
			days_from_civil(num(0, 4), static_cast<unsigned>(month), static_cast<unsigned>(day));
	return ((days * 24 + num(11, 2)) * 60 + num(14, 2)) * 60 + num(17, 2);
}

} // namespace

MetricsConfig metrics_config_from_env(const std::string &scratch_dir) {
	const InfraConfig infra = infra_config_from_env(scratch_dir);
	MetricsConfig c;
	c.aws = infra.aws;
	c.database = infra.database;
	c.cache = infra.cache;
	c.target_groups = infra.target_groups;
	if (const char *lb = std::getenv("ONNET_INFRA_LOAD_BALANCER"))
		c.load_balancer = std::string(strutil::trim_view(lb));
	return c;
}

MetricsStore::MetricsStore(
		proc::CommandRunner &runner, MetricsConfig config, Clock wall_ms, Clock steady_ms) :
		config_(std::move(config)),
		unavailable_([this] {
			const std::string why = AwsApi::check_config(config_.aws);
			return why.empty() ? std::string() : "AWS is not read here: " + why;
		}()),
		api_(runner, config_.aws),
		wall_(wall_ms ? std::move(wall_ms) : Clock(wall_now_ms)),
		steady_(steady_ms ? std::move(steady_ms)
						  : Clock([] { return static_cast<int64_t>(Watchdog::steady_now_ms()); })) {
}

bool MetricsStore::read(const std::string &resource, bool hourly, const std::string &cache_node,
		ResourceMetrics &out) {
	if (resource != "rds" && resource != "cache" && resource != "nlb")
		return false;
	const std::string key = resource + (hourly ? "|h|" : "|m|") + cache_node;
	{
		std::lock_guard<std::mutex> lock(mu_);
		const auto it = cache_.find(key);
		if (it != cache_.end() && steady_() - it->second.at < kCacheMs) {
			out = it->second.metrics;
			return true;
		}
	}
	out = read_uncached(resource, hourly, cache_node);
	if (out.error.empty() && out.unavailable.empty()) {
		std::lock_guard<std::mutex> lock(mu_);
		cache_[key] = { steady_(), out };
	}
	return true;
}

ResourceMetrics MetricsStore::read_uncached(
		const std::string &resource, bool hourly, const std::string &cache_node) {
	ResourceMetrics out;
	out.resource = resource;
	out.hourly = hourly;
	out.period_seconds = hourly ? 3600 : 60;
	if (!unavailable_.empty()) {
		out.unavailable = unavailable_;
		return out;
	}

	std::string ns;
	std::vector<Wanted> wanted;
	if (resource == "rds") {
		if (config_.database.empty()) {
			out.unavailable = "ONNET_INFRA_DATABASE is unset";
			return out;
		}
		ns = "AWS/RDS";
		const std::vector<std::pair<std::string, std::string>> dim = { { "DBInstanceIdentifier",
				config_.database } };
		for (const auto &[metric, stat] :
				std::vector<std::pair<const char *, const char *>>{ { "CPUUtilization", "Average" },
						{ "FreeableMemory", "Minimum" }, { "DatabaseConnections", "Maximum" },
						{ "NetworkReceiveThroughput", "Average" },
						{ "NetworkTransmitThroughput", "Average" }, { "ReadIOPS", "Average" },
						{ "WriteIOPS", "Average" } })
			wanted.push_back({ metric, metric, stat, dim });
	} else if (resource == "cache") {
		if (config_.cache.empty()) {
			out.unavailable = "ONNET_INFRA_CACHE is unset";
			return out;
		}
		ns = "AWS/ElastiCache";
		const std::string node = cache_node.empty() ? config_.cache + "-001" : cache_node;
		const std::vector<std::pair<std::string, std::string>> dim = { { "CacheClusterId", node } };
		for (const auto &[metric, stat] :
				std::vector<std::pair<const char *, const char *>>{ { "CPUUtilization", "Average" },
						{ "EngineCPUUtilization", "Average" }, { "BytesUsedForCache", "Maximum" },
						{ "DatabaseMemoryUsagePercentage", "Maximum" }, { "NetworkBytesIn", "Sum" },
						{ "NetworkBytesOut", "Sum" }, { "CurrConnections", "Maximum" } })
			wanted.push_back({ metric, metric, stat, dim });
	} else {
		if (config_.load_balancer.empty()) {
			out.unavailable = "ONNET_INFRA_LOAD_BALANCER is unset";
			return out;
		}
		ns = "AWS/NetworkELB";
		const std::pair<std::string, std::string> lb = { "LoadBalancer", config_.load_balancer };
		wanted.push_back({ "ProcessedBytes", "ProcessedBytes", "Sum", { lb } });
		wanted.push_back({ "ActiveFlowCount", "ActiveFlowCount", "Average", { lb } });
		for (const auto &[listener, arn] : config_.target_groups)
			wanted.push_back({ "HealthyHostCount:" + listener, "HealthyHostCount", "Minimum",
					{ { "TargetGroup", arn_resource(arn) }, lb } });
	}

	// The range: its whole periods to the last one begun.
	const int64_t period = out.period_seconds;
	const int64_t now_s = wall_() / 1000;
	const int64_t end = now_s - now_s % period;
	const int64_t start = end - (hourly ? kHourRangeSeconds : kMinuteRangeSeconds);
	Params params = { { "Action", "GetMetricData" }, { "Version", "2010-08-01" },
		{ "StartTime", hosted::iso_utc(start) }, { "EndTime", hosted::iso_utc(end) },
		{ "ScanBy", "TimestampAscending" } };
	for (size_t i = 0; i < wanted.size(); ++i) {
		const std::string q = "MetricDataQueries.member." + std::to_string(i + 1) + ".";
		params.emplace_back(q + "Id", "m" + std::to_string(i + 1));
		params.emplace_back(q + "MetricStat.Metric.Namespace", ns);
		params.emplace_back(q + "MetricStat.Metric.MetricName", wanted[i].metric);
		for (size_t d = 0; d < wanted[i].dimensions.size(); ++d) {
			const std::string dim =
					q + "MetricStat.Metric.Dimensions.member." + std::to_string(d + 1) + ".";
			params.emplace_back(dim + "Name", wanted[i].dimensions[d].first);
			params.emplace_back(dim + "Value", wanted[i].dimensions[d].second);
		}
		params.emplace_back(q + "MetricStat.Period", std::to_string(period));
		params.emplace_back(q + "MetricStat.Stat", wanted[i].statistic);
		MetricSeries series;
		series.name = wanted[i].name;
		series.statistic = wanted[i].statistic;
		out.metrics.push_back(std::move(series));
	}

	AwsCredentials creds;
	if (!api_.credentials(creds, out.error))
		return out;
	std::string token;
	for (int page = 0; page < kPages; ++page) {
		Params call = params;
		if (!token.empty())
			call.emplace_back("NextToken", token);
		const AwsReply reply = api_.query_call(creds, "monitoring", call);
		if (!reply.ok()) {
			out.error = AwsApi::failure("monitoring", reply);
			return out;
		}
		XmlNode root;
		std::string parse_error;
		if (!parse_xml(reply.body, root, parse_error)) {
			out.error = "monitoring answered no XML (" + parse_error + ")";
			return out;
		}
		const XmlNode *result = root.child("GetMetricDataResult");
		const XmlNode *results = result != nullptr ? result->child("MetricDataResults") : nullptr;
		if (results != nullptr)
			for (const XmlNode &m : results->children) {
				const std::string id = m.child_text("Id");
				const auto n = id.size() > 1 && id[0] == 'm' ? strutil::parse_llong(id.substr(1))
															 : std::optional<long long>();
				if (!n || *n < 1 || static_cast<size_t>(*n) > out.metrics.size())
					continue;
				MetricSeries &series = out.metrics[static_cast<size_t>(*n - 1)];
				const XmlNode *times = m.child("Timestamps");
				const XmlNode *values = m.child("Values");
				if (times == nullptr || values == nullptr)
					continue;
				const size_t count = std::min(times->children.size(), values->children.size());
				for (size_t i = 0; i < count; ++i) {
					const int64_t at = iso_seconds(strutil::trim_view(times->children[i].text));
					char *end_ptr = nullptr;
					const std::string text(strutil::trim_view(values->children[i].text));
					const double value = std::strtod(text.c_str(), &end_ptr);
					if (at >= 0 && end_ptr != text.c_str())
						series.points.push_back({ at, value });
				}
			}
		token = result != nullptr ? result->child_text("NextToken") : std::string();
		if (token.empty())
			break;
	}
	for (MetricSeries &s : out.metrics)
		std::stable_sort(s.points.begin(), s.points.end(),
				[](const MetricPoint &a, const MetricPoint &b) { return a.at < b.at; });
	return out;
}

} // namespace opennova::novaworld_server::infra
