// The month's AWS costs for an admin (apps/novaworld_web/infra/cost_routes.h), driven
// in-process: a database of the run's own, a Redis namespace of its own and two Crow
// HttpListeners on ports the OS picks: the local stack's (no task role: nothing read) and a
// deployment's, whose Cost Explorer and Budgets reads go to a stub CommandRunner
// (infra_aws_stub.h).
//
//   - refused without an admin;
//   - the local stack: available false, and why; nothing called;
//   - a deployment's: the month so far by service and by day, the forecast, the budget,
//     when web read them; no-store; a second read answered from the store's cache; no
//     credential in the reply.

#include "http_listener.h"
#include "infra/cost_store.h"
#include "net_http.h"
#include "net_sockets.h"
#include "program/server_config.h"

#include <net/novaworld/db/database.h>

#include <crow/json.h>

#include "infra_aws_stub.h"

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"
#include "common/test_redis.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifndef OPENNOVA_SOURCE_DIR
#error "OPENNOVA_SOURCE_DIR must be set by CMake"
#endif

namespace net = opennova::net;
namespace nws = opennova::novaworld_server;
namespace infra = opennova::novaworld_server::infra;
using opennova::db::ConnectionPool;

namespace {

constexpr const char *kAdminToken = "cost-routes-admin-token";
constexpr const char *kBearer = "Authorization: Bearer cost-routes-admin-token";
// 2026-10-19T12:00:30Z.
constexpr int64_t kNow = 1792411230;

struct Harness {
	uint16_t local_port = 0;
	uint16_t cloud_port = 0;
	infra_stub::AwsStub *aws = nullptr;

	net::HttpReply get(uint16_t port, const std::string &path, bool admin = true) const {
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
		std::vector<std::string> headers;
		if (admin)
			headers.push_back(kBearer);
		return net::http_exchange(
				"GET", url, headers, {}, 15000, [](const std::string &host, net::Endpoint &out) {
					return net::resolve_ipv4(host, out, false);
				});
	}
};

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

crow::json::rvalue json_of(const net::HttpReply &reply, int code) {
	if (!reply.transport_ok || reply.code != code) {
		std::fprintf(stderr, "  HTTP %d %s, want %d\n", reply.code, body_text(reply).c_str(), code);
		return {};
	}
	return crow::json::load(body_text(reply));
}

std::string str(const crow::json::rvalue &v) {
	return std::string(v.s());
}

bool is_null(const crow::json::rvalue &v) {
	return v.t() == crow::json::type::Null;
}

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

// A deployment's month as Cost Explorer and Budgets answer it.
std::map<std::string, opennova::proc::CommandResult> cost_replies() {
	std::map<std::string, opennova::proc::CommandResult> r;
	r["credentials"] = infra_stub::answer(200, infra_stub::kCredentials);
	r["ce GetCostAndUsage"] = infra_stub::answer(200,
			R"({"ResultsByTime":[)"
			R"({"TimePeriod":{"Start":"2026-10-01","End":"2026-10-02"},"Groups":[)"
			R"({"Keys":["Amazon Elastic Container Service"],"Metrics":{"UnblendedCost":{"Amount":"4.5","Unit":"USD"}}}],"Estimated":true},)"
			R"({"TimePeriod":{"Start":"2026-10-19","End":"2026-10-20"},"Groups":[)"
			R"({"Keys":["Amazon Elastic Container Service"],"Metrics":{"UnblendedCost":{"Amount":"2.5","Unit":"USD"}}},)"
			R"({"Keys":["Amazon Relational Database Service"],"Metrics":{"UnblendedCost":{"Amount":"0.75","Unit":"USD"}}}],"Estimated":true}]})");
	r["ce GetCostForecast"] = infra_stub::answer(200, R"({"Total":{"Amount":"100","Unit":"USD"}})");
	r["budgets DescribeBudget"] = infra_stub::answer(200,
			R"({"Budget":{"BudgetName":"opennova-prod-monthly-cost","BudgetLimit":{"Amount":"250","Unit":"USD"},)"
			R"("TimeUnit":"MONTHLY","BudgetType":"COST","CalculatedSpend":{"ActualSpend":{"Amount":"7.75","Unit":"USD"}}}})");
	return r;
}

int test_refusals(Harness &h) {
	TEST_EXPECT(h.get(h.local_port, "/api/admin/costs", false).code == 401);
	TEST_EXPECT(h.get(h.cloud_port, "/api/admin/costs", false).code == 401);
	TEST_EXPECT(h.aws->calls().empty());
	return 0;
}

int test_local(Harness &h) {
	const auto json = json_of(h.get(h.local_port, "/api/admin/costs"), 200);
	TEST_EXPECT(json && !json["available"].b() &&
			contains(str(json["unavailable"]), "AWS costs are not read here") &&
			is_null(json["error"]) && is_null(json["read_at"]) && is_null(json["month"]) &&
			is_null(json["forecast_cents"]) && json["by_service"].size() == 0 &&
			json["days"].size() == 0 && is_null(json["budget"]));
	TEST_EXPECT(h.aws->calls().empty());
	return 0;
}

int test_deployment(Harness &h) {
	const auto reply = h.get(h.cloud_port, "/api/admin/costs");
	bool no_store = false;
	for (const std::string &line : reply.headers)
		no_store = no_store || line == "Cache-Control: no-store";
	TEST_EXPECT(no_store);
	for (const char *secret : { infra_stub::kSecret, infra_stub::kToken, infra_stub::kKeyId })
		TEST_EXPECT(!contains(body_text(reply), secret));
	const auto json = json_of(reply, 200);
	TEST_EXPECT(json && json["available"].b() && is_null(json["unavailable"]) &&
			is_null(json["error"]) && str(json["read_at"]) == "2026-10-19T12:00:30Z");
	TEST_EXPECT(str(json["currency"]) == "USD" && str(json["month"]["start"]) == "2026-10-01" &&
			str(json["month"]["end"]) == "2026-11-01" && str(json["through"]) == "2026-10-19" &&
			json["estimated"].b());
	// 4.5 + 2.5 + 0.75; the forecast the month before today (4.5) and AWS's 100.
	TEST_EXPECT(json["month_to_date_cents"].i() == 775 && json["forecast_cents"].i() == 10450 &&
			is_null(json["forecast_error"]));
	const auto &services = json["by_service"];
	TEST_EXPECT(services.size() == 2 &&
			str(services[0]["service"]) == "Amazon Elastic Container Service" &&
			services[0]["cents"].i() == 700 && services[1]["cents"].i() == 75);
	const auto &days = json["days"];
	TEST_EXPECT(days.size() == 19 && str(days[0]["date"]) == "2026-10-01" &&
			days[0]["cents"].i() == 450 && days[1]["cents"].i() == 0 &&
			days[18]["cents"].i() == 325);
	const auto &budget = json["budget"];
	TEST_EXPECT(str(budget["name"]) == "opennova-prod-monthly-cost" &&
			str(budget["currency"]) == "USD" && budget["limit_cents"].i() == 25000 &&
			budget["actual_cents"].i() == 775 && is_null(budget["forecast_cents"]) &&
			is_null(json["budget_error"]));

	// Kept: a second read calls AWS no more.
	const size_t calls = h.aws->calls().size();
	TEST_EXPECT(calls == 4 && h.aws->count("ce GetCostAndUsage") == 1);
	const auto again = json_of(h.get(h.cloud_port, "/api/admin/costs"), 200);
	TEST_EXPECT(again && again["month_to_date_cents"].i() == 775 && h.aws->calls().size() == calls);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	test_redis::init(argc, argv);
	TEST_EXPECT(net::startup() == 0);
	int rc = 1;
	{
		test_temp::TempDir temp("cost_routes");
		test_temp::TempDir scratch("cost_routes_scratch");
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

		infra_stub::AwsStub aws;
		aws.replies = cost_replies();
		infra::CostStore local_costs(aws, infra::CostConfig());
		infra::CostConfig cloud_config;
		cloud_config.aws.region = "us-east-1";
		cloud_config.aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
		cloud_config.aws.scratch_dir = scratch.root();
		cloud_config.budget_arn = "arn:aws:budgets::123456789012:budget/opennova-prod-monthly-cost";
		infra::CostStore cloud_costs(aws, cloud_config, [] { return kNow * 1000; });

		nws::HttpListener local(pool, redis.pool());
		local.set_cost_store(&local_costs);
		TEST_EXPECT(local.start(config));
		nws::HttpListener cloud(pool, redis.pool());
		cloud.set_cost_store(&cloud_costs);
		TEST_EXPECT(cloud.start(config));

		Harness harness;
		harness.local_port = local.bound_port();
		harness.cloud_port = cloud.bound_port();
		harness.aws = &aws;

		rc = 0;
		struct Case {
			const char *name;
			int (*fn)(Harness &);
		};
		const Case cases[] = {
			{ "refusals", test_refusals },
			{ "local", test_local },
			{ "deployment", test_deployment },
		};
		for (const Case &c : cases) {
			std::printf("-- %s\n", c.name);
			if (rc == 0 && c.fn(harness) != 0) {
				std::fprintf(stderr, "FAIL: %s\n", c.name);
				rc = 1;
			}
		}
		cloud.stop();
		local.stop();
	}
	net::shutdown();
	if (rc == 0)
		std::printf("OK: the month's AWS costs, for an admin\n");
	return rc;
}
