// The AWS resources' own metrics for the admin's usage view
// (apps/novaworld_web/infra/metrics_store.h), no database and no network: GetMetricData
// behind a scripted CommandRunner reading each call's curl config file (argv exactly {curl,
// -q, --config, <file>}, no credential on it):
//
//   - the database's: the form's action, range (48 h to the last whole minute), each
//     query's namespace, metric, dimension, period and statistic; the reply's points read
//     and put in order, each metric with its statistic;
//   - the cache's primary node by name, else <group>-001; the balancer's metrics and each
//     target group's healthy hosts by its ARN's resource; the 400 days at hourly points;
//   - a NextToken followed and its points merged; AWS's error in words, not kept;
//   - the cache: a read like one within 60 s calls nothing, past it AWS again;
//   - no task role (the local stack): nothing called, the reason given; a resource that
//     is none refused.

#include "infra/metrics_store.h"

#include "infra_aws_stub.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace infra = opennova::novaworld_server::infra;
namespace fs = std::filesystem;
using infra_stub::answer;
using opennova::proc::CommandResult;

namespace {

// 2026-10-18T12:00:30Z.
constexpr int64_t kNow = 1792324830;

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

struct Call {
	std::vector<std::string> argv;
	std::string key;
	std::string form; // decoded
};

class ScriptedAws : public opennova::proc::CommandRunner {
public:
	std::function<CommandResult(const Call &)> answer_call;
	std::vector<Call> calls;

	CommandResult run(const std::vector<std::string> &argv, int) override {
		Call call;
		call.argv = argv;
		std::string config;
		if (argv.size() == 4) {
			std::ifstream in(fs::u8path(argv[3]), std::ios::binary);
			config.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		call.key = infra_stub::key_of(config);
		call.form = infra_stub::percent_decoded(infra_stub::config_line(config, "data-raw"));
		calls.push_back(call);
		if (call.key == "credentials")
			return answer(200, infra_stub::kCredentials);
		return answer_call ? answer_call(call) : infra_stub::curl_failed(7, "no answer");
	}

	size_t count(const std::string &key) const {
		size_t n = 0;
		for (const Call &c : calls)
			n += c.key == key ? 1 : 0;
		return n;
	}
};

infra::MetricsConfig deployment(const std::string &scratch) {
	infra::MetricsConfig c;
	c.aws.region = "us-east-1";
	c.aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
	c.aws.scratch_dir = scratch;
	c.database = "opennova-prod-db";
	c.cache = "opennova-prod-cache";
	c.load_balancer = "net/opennova-prod/50dc6c495c0c9188";
	c.target_groups = {
		{ "gate",
				std::string(infra_stub::kTargetGroupArn) + "opennova-prod-gate/0123456789abcdef" },
		{ "lobby",
				std::string(infra_stub::kTargetGroupArn) + "opennova-prod-lobby/0123456789abcdef" }
	};
	return c;
}

// A GetMetricData reply: each query's points newest first, as AWS may give them.
std::string reply(
		const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>
				&results,
		const std::string &next = {}) {
	std::string out =
			R"(<GetMetricDataResponse xmlns="http://monitoring.amazonaws.com/doc/2010-08-01/">
  <GetMetricDataResult>
    <MetricDataResults>)";
	for (const auto &[id, points] : results) {
		out += "<member><Id>" + id + "</Id><Label>x</Label><Timestamps>";
		for (const auto &p : points)
			out += "<member>" + p.first + "</member>";
		out += "</Timestamps><Values>";
		for (const auto &p : points)
			out += "<member>" + p.second + "</member>";
		out += "</Values><StatusCode>Complete</StatusCode></member>";
	}
	out += "</MetricDataResults>";
	if (!next.empty())
		out += "<NextToken>" + next + "</NextToken>";
	out += "<Messages/></GetMetricDataResult><ResponseMetadata><RequestId>r</RequestId>"
		   "</ResponseMetadata></GetMetricDataResponse>";
	return out;
}

int test_database() {
	test_temp::TempDir scratch("metrics_db");
	ScriptedAws aws;
	int64_t steady = 0;
	infra::MetricsStore store(
			aws, deployment(scratch.root()), [] { return kNow * 1000; },
			[&steady] { return steady; });
	aws.answer_call = [](const Call &) {
		return answer(200,
				reply({ { "m1",
								{ { "2026-10-18T11:59:00Z", "12.5" },
										{ "2026-10-18T11:58:00Z", "10" } } },
						{ "m2", { { "2026-10-18T11:59:00Z", "734003200" } } } }));
	};
	infra::ResourceMetrics m;
	TEST_EXPECT(store.read("rds", false, "", m));
	TEST_EXPECT(m.unavailable.empty() && m.error.empty() && m.period_seconds == 60 && !m.hourly);
	TEST_EXPECT(m.metrics.size() == 7 && m.metrics[0].name == "CPUUtilization" &&
			m.metrics[0].statistic == "Average" && m.metrics[1].name == "FreeableMemory" &&
			m.metrics[6].name == "WriteIOPS");
	// In order, oldest first.
	TEST_EXPECT(m.metrics[0].points.size() == 2 && m.metrics[0].points[0].at == kNow - 30 - 120 &&
			m.metrics[0].points[0].value == 10 && m.metrics[0].points[1].value == 12.5);
	TEST_EXPECT(m.metrics[1].points.size() == 1 && m.metrics[1].points[0].value == 734003200);
	TEST_EXPECT(m.metrics[2].points.empty());

	const Call &call = aws.calls.back();
	TEST_EXPECT(call.key == "monitoring GetMetricData" && call.argv.size() == 4);
	for (const std::string &arg : call.argv)
		TEST_EXPECT(!contains(arg, infra_stub::kSecret) && !contains(arg, infra_stub::kKeyId));
	TEST_EXPECT(contains(call.form, "Action=GetMetricData&Version=2010-08-01"));
	TEST_EXPECT(contains(call.form, "StartTime=2026-10-16T12:00:00Z&EndTime=2026-10-18T12:00:00Z"));
	TEST_EXPECT(contains(call.form, "ScanBy=TimestampAscending"));
	TEST_EXPECT(contains(call.form,
			"MetricDataQueries.member.1.Id=m1&MetricDataQueries.member.1."
			"MetricStat.Metric.Namespace=AWS/RDS&MetricDataQueries.member.1."
			"MetricStat.Metric.MetricName=CPUUtilization"));
	TEST_EXPECT(contains(call.form,
			"MetricDataQueries.member.1.MetricStat.Metric.Dimensions.member.1."
			"Name=DBInstanceIdentifier&MetricDataQueries.member.1.MetricStat."
			"Metric.Dimensions.member.1.Value=opennova-prod-db"));
	TEST_EXPECT(contains(call.form,
			"MetricDataQueries.member.1.MetricStat.Period=60&"
			"MetricDataQueries.member.1.MetricStat.Stat=Average"));
	TEST_EXPECT(contains(
			call.form, "MetricDataQueries.member.7.MetricStat.Metric.MetricName=WriteIOPS"));

	// The cache: within 60 s nothing is called; past it, AWS again.
	steady += 59'000;
	TEST_EXPECT(store.read("rds", false, "", m) && aws.count("monitoring GetMetricData") == 1);
	steady += 2'000;
	TEST_EXPECT(store.read("rds", false, "", m) && aws.count("monitoring GetMetricData") == 2);
	TEST_EXPECT(!store.read("s3", false, "", m));
	std::printf("  the database: the form, the points in order, the cache\n");
	return 0;
}

int test_cache_and_balancer() {
	test_temp::TempDir scratch("metrics_lb");
	ScriptedAws aws;
	infra::MetricsStore store(
			aws, deployment(scratch.root()), [] { return kNow * 1000; },
			[] { return int64_t{ 0 }; });
	aws.answer_call = [](const Call &c) {
		// The first page names a NextToken; the second carries more of m1.
		if (!contains(c.form, "NextToken="))
			return answer(200, reply({ { "m1", { { "2026-10-18T10:00:00Z", "3" } } } }, "page2"));
		return answer(200,
				reply({ { "m1", { { "2026-10-18T11:00:00Z", "4" } } },
						{ "m4", { { "2026-10-18T11:00:00Z", "2" } } } }));
	};
	infra::ResourceMetrics m;
	TEST_EXPECT(store.read("cache", false, "opennova-prod-cache-002", m));
	TEST_EXPECT(contains(aws.calls.back().form,
			"Name=CacheClusterId&MetricDataQueries.member.1."
			"MetricStat.Metric.Dimensions.member.1.Value="
			"opennova-prod-cache-002"));
	TEST_EXPECT(contains(aws.calls.back().form, "NextToken=page2"));
	TEST_EXPECT(m.metrics.size() == 7 && m.metrics[0].points.size() == 2 &&
			m.metrics[0].points[1].value == 4 && m.metrics[4].name == "NetworkBytesIn" &&
			m.metrics[4].statistic == "Sum");
	TEST_EXPECT(store.read("cache", false, "", m) &&
			contains(aws.calls.back().form, "Value=opennova-prod-cache-001"));

	// The balancer over 400 days at hourly points.
	TEST_EXPECT(store.read("nlb", true, "", m));
	const std::string &form = aws.calls[aws.calls.size() - 2].form;
	TEST_EXPECT(m.hourly && m.period_seconds == 3600 && m.metrics.size() == 4);
	TEST_EXPECT(m.metrics[0].name == "ProcessedBytes" && m.metrics[1].name == "ActiveFlowCount" &&
			m.metrics[2].name == "HealthyHostCount:gate" && m.metrics[2].statistic == "Minimum" &&
			m.metrics[3].name == "HealthyHostCount:lobby");
	TEST_EXPECT(contains(form, "StartTime=2025-09-13T12:00:00Z&EndTime=2026-10-18T12:00:00Z"));
	TEST_EXPECT(contains(form, "Namespace=AWS/NetworkELB"));
	TEST_EXPECT(contains(form,
			"MetricDataQueries.member.3.MetricStat.Metric.Dimensions.member.1."
			"Name=TargetGroup&MetricDataQueries.member.3.MetricStat.Metric."
			"Dimensions.member.1.Value=targetgroup/opennova-prod-gate/"
			"0123456789abcdef&MetricDataQueries.member.3.MetricStat.Metric."
			"Dimensions.member.2.Name=LoadBalancer&MetricDataQueries.member.3."
			"MetricStat.Metric.Dimensions.member.2.Value=net/opennova-prod/"
			"50dc6c495c0c9188"));
	TEST_EXPECT(contains(form, "MetricDataQueries.member.1.MetricStat.Period=3600"));

	// AWS's refusal in words, not kept.
	aws.answer_call = [](const Call &) {
		return answer(403,
				R"(<ErrorResponse><Error><Type>Sender</Type><Code>AccessDenied</Code><Message>not authorized to perform: cloudwatch:GetMetricData</Message></Error></ErrorResponse>)");
	};
	TEST_EXPECT(store.read("rds", true, "", m));
	TEST_EXPECT(contains(m.error, "monitoring answered 403: AccessDenied: not authorized") &&
			!contains(m.error, infra_stub::kSecret));
	const size_t calls = aws.count("monitoring GetMetricData");
	TEST_EXPECT(
			store.read("rds", true, "", m) && aws.count("monitoring GetMetricData") == calls + 1);
	std::printf("  the cache's node, the balancer's groups, 400 days hourly, a NextToken, "
				"AWS's refusal\n");
	return 0;
}

int test_local() {
	ScriptedAws aws;
	infra::MetricsConfig c;
	c.database = "opennova-dev-db";
	infra::MetricsStore store(aws, c);
	infra::ResourceMetrics m;
	TEST_EXPECT(store.read("rds", false, "", m));
	TEST_EXPECT(!store.unavailable().empty() && m.unavailable == store.unavailable() &&
			contains(m.unavailable, "AWS is not read here"));
	TEST_EXPECT(aws.calls.empty());
	std::printf("  the local stack: nothing called, and why\n");
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "database", test_database },
		{ "cache_and_balancer", test_cache_and_balancer },
		{ "local", test_local },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the AWS resources' metrics\n");
	return 0;
}
