// The month's AWS costs for the admin's costs view (apps/novaworld_web/infra/cost_store.h),
// no database and no network: Cost Explorer and Budgets behind a scripted CommandRunner
// reading each call's curl config file (argv exactly {curl, -q, --config, <file>}, no
// credential on it):
//
//   - the global endpoints, signed for us-east-1 whatever the deployment's region;
//   - GetCostAndUsage's body (the month so far by day and by service, the unblended
//     cost), a NextPageToken followed, the groups added up by service (largest first, a
//     line under a cent left out) and by day (every day of the month, none as nothing);
//   - GetCostForecast from today to the next month's first, the month's total the month
//     before today and the forecast; DescribeBudget of ONNET_INFRA_BUDGET's account and
//     name, its limit and AWS's spend;
//   - AWS refusing the forecast (too little history) or the budget: the month still read,
//     each refusal in words; a month's last day and December;
//   - the cache: a read within 6 h calls nothing, a failed one is kept 10 min;
//   - no task role (the local stack): nothing called, the reason given; budget ARNs.

#include "infra/cost_store.h"

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

// 2026-10-19T12:00:30Z.
constexpr int64_t kNow = 1792411230;
constexpr const char *kBudgetArn =
		"arn:aws:budgets::123456789012:budget/opennova-prod-monthly-cost";
constexpr const char *kCiRole = "arn:aws:iam::444444444444:role/opennova-ci-cost-read";
constexpr const char *kCiBudget = "arn:aws:budgets::444444444444:budget/opennova-ci-monthly-cost";
constexpr const char *kCiCredentials =
		"<AssumeRoleResponse><AssumeRoleResult><Credentials>"
		"<AccessKeyId>ASIACICOSTKEY</AccessKeyId><SecretAccessKey>ci-secret</SecretAccessKey>"
		"<SessionToken>ci-token</SessionToken><Expiration>2026-10-19T12:15:30Z</Expiration>"
		"</Credentials></AssumeRoleResult></AssumeRoleResponse>";

bool contains(const std::string &text, const std::string &part) {
	return text.find(part) != std::string::npos;
}

struct Call {
	std::vector<std::string> argv;
	std::string key;
	std::string url;
	std::string sigv4;
	std::string body;
	std::string user;
	int timeout_ms = 0;
};

class ScriptedAws : public opennova::proc::CommandRunner {
public:
	std::function<CommandResult(const Call &)> answer_call;
	std::vector<Call> calls;

	CommandResult run(const std::vector<std::string> &argv, int timeout_ms) override {
		Call call;
		call.argv = argv;
		std::string config;
		if (argv.size() == 4) {
			std::ifstream in(fs::u8path(argv[3]), std::ios::binary);
			config.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		call.key = infra_stub::key_of(config);
		call.url = infra_stub::config_line(config, "url");
		call.sigv4 = infra_stub::config_line(config, "aws-sigv4");
		call.body = infra_stub::config_line(config, "data-raw");
		call.user = infra_stub::config_line(config, "user");
		call.timeout_ms = timeout_ms;
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
	const Call *last(const std::string &key) const {
		for (auto it = calls.rbegin(); it != calls.rend(); ++it)
			if (it->key == key)
				return &*it;
		return nullptr;
	}
};

infra::CostConfig deployment(const std::string &scratch) {
	infra::CostConfig c;
	// Not us-east-1: Cost Explorer and Budgets are signed for it whatever the region.
	c.aws.region = "us-west-2";
	c.aws.credentials_uri = "/v2/credentials/0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0";
	c.aws.scratch_dir = scratch;
	c.budget_arn = kBudgetArn;
	return c;
}

std::string group(const std::string &service, const std::string &amount) {
	return R"({"Keys":[")" + service + R"("],"Metrics":{"UnblendedCost":{"Amount":")" + amount +
			R"(","Unit":"USD"}}})";
}

std::string day(const std::string &start, const std::string &end, const std::string &groups) {
	return R"({"TimePeriod":{"Start":")" + start + R"(","End":")" + end +
			R"("},"Total":{},"Groups":[)" + groups + R"(],"Estimated":true})";
}

// The month as Cost Explorer pages it: the 1st and 2nd, then (page 2) today.
CommandResult usage_page(const Call &c) {
	if (!contains(c.body, "NextPageToken"))
		return answer(200,
				R"({"GroupDefinitions":[{"Type":"DIMENSION","Key":"SERVICE"}],"ResultsByTime":[)" +
						day("2026-10-01", "2026-10-02",
								group("Amazon Elastic Container Service", "3.1") + "," +
										group("Amazon Relational Database Service", "0.5")) +
						"," +
						day("2026-10-02", "2026-10-03",
								group("Amazon Elastic Container Service", "3.2") + "," +
										group("Tax", "0.004")) +
						R"(],"NextPageToken":"page2"})");
	return answer(200,
			R"({"ResultsByTime":[)" +
					day("2026-10-19", "2026-10-20",
							group("Amazon Elastic Container Service", "1.25") + "," +
									group("Amazon Elastic Compute Cloud - Compute", "9.75")) +
					"]}");
}

CommandResult deployment_reply(const Call &c) {
	if (c.key == "ce GetCostAndUsage")
		return usage_page(c);
	if (c.key == "ce GetCostForecast")
		return answer(200,
				R"({"Total":{"Amount":"110.4","Unit":"USD"},"ForecastResultsByTime":[{"TimePeriod":{"Start":"2026-10-19","End":"2026-11-01"},"MeanValue":"110.4"}]})");
	if (c.key == "budgets DescribeBudget")
		return answer(200,
				R"({"Budget":{"BudgetName":"opennova-prod-monthly-cost","BudgetLimit":{"Amount":"250.0","Unit":"USD"},)"
				R"("TimeUnit":"MONTHLY","BudgetType":"COST","CalculatedSpend":{"ActualSpend":{"Amount":"16.81","Unit":"USD"},)"
				R"("ForecastedSpend":{"Amount":"124.2","Unit":"USD"}},"LastUpdatedTime":1792400000}})");
	return infra_stub::curl_failed(7, "no answer");
}

infra::CostConfig combined(const std::string &scratch) {
	auto c = deployment(scratch);
	c.ci_role_arn = kCiRole;
	c.ci_budget_arn = kCiBudget;
	return c;
}

bool is_ci(const Call &c) {
	return contains(c.user, "ASIACICOSTKEY");
}

CommandResult combined_reply(const Call &c) {
	if (c.key == "sts AssumeRole")
		return answer(200, kCiCredentials);
	if (!is_ci(c))
		return deployment_reply(c);
	if (c.key == "ce GetCostAndUsage")
		return answer(200,
				R"({"ResultsByTime":[)" +
						day("2026-10-01", "2026-10-02",
								group("Amazon Elastic Compute Cloud - Compute", "10.25") + "," +
										group("Amazon Simple Storage Service", "2.50")) +
						"]}");
	if (c.key == "ce GetCostForecast")
		return answer(200, R"({"Total":{"Amount":"20","Unit":"USD"}})");
	if (c.key == "budgets DescribeBudget")
		return answer(200,
				R"({"Budget":{"BudgetName":"opennova-ci-monthly-cost","BudgetLimit":{"Amount":"300","Unit":"USD"},)"
				R"("TimeUnit":"MONTHLY","BudgetType":"COST","CalculatedSpend":{"ForecastedSpend":{"Amount":"90","Unit":"USD"}}}})");
	return infra_stub::curl_failed(7, "unexpected CI call");
}

int test_combined() {
	test_temp::TempDir scratch("costs_combined");
	ScriptedAws aws;
	aws.answer_call = combined_reply;
	infra::CostStore store(aws, combined(scratch.root()), [] { return kNow * 1000; });
	const auto r = store.read();
	TEST_EXPECT(r.read && r.error.empty() && r.accounts.size() == 2 && !r.budget);
	TEST_EXPECT(r.accounts[0].id == "deployment" && r.accounts[0].label == "Production" &&
			r.accounts[1].id == "ci" && r.accounts[1].label == "CI");
	TEST_EXPECT(r.month_to_date_cents == 3055 && r.accounts[0].month_to_date_cents == 1780 &&
			r.accounts[1].month_to_date_cents == 1275);
	TEST_EXPECT(r.forecast_cents == 14995 && r.accounts[1].forecast_cents == 3275);
	TEST_EXPECT(r.accounts[0].budget->limit_cents == 25000 &&
			r.accounts[1].budget->limit_cents == 30000);
	TEST_EXPECT(r.by_service.size() == 4 && r.by_service[0].cents == 2000 &&
			r.by_service[2].service == "Amazon Simple Storage Service" &&
			r.by_service[2].cents == 250);
	TEST_EXPECT(r.days[0].cents == 1635 && r.days[18].cents == 1100);
	TEST_EXPECT(aws.count("credentials") == 1 && aws.count("sts AssumeRole") == 1 &&
			aws.count("ce GetCostAndUsage") == 3);
	TEST_EXPECT(aws.last("sts AssumeRole")->url == "https://sts.us-west-2.amazonaws.com/" &&
			contains(aws.last("sts AssumeRole")->user, infra_stub::kKeyId));
	TEST_EXPECT(is_ci(*aws.last("budgets DescribeBudget")) &&
			contains(aws.last("budgets DescribeBudget")->body, "444444444444"));
	const auto calls = aws.calls.size();
	TEST_EXPECT(store.read().read && aws.calls.size() == calls);
	return 0;
}

int test_account_failures() {
	for (int fail = 1; fail <= 3; ++fail) {
		test_temp::TempDir scratch("costs_account_failure");
		ScriptedAws aws;
		aws.answer_call = [fail](const Call &c) {
			if (c.key == "ce GetCostAndUsage" && ((is_ci(c) ? 2 : 1) & fail))
				return answer(
						400, R"({"__type":"AccessDeniedException","Message":"usage denied"})");
			return combined_reply(c);
		};
		int64_t steady = 0;
		infra::CostStore store(
				aws, combined(scratch.root()), [] { return kNow * 1000; }, [&] { return steady; });
		const auto r = store.read();
		TEST_EXPECT(!r.read && !r.error.empty() && r.month_to_date_cents == 0 &&
				r.by_service.empty() && r.days.empty() && !r.forecast_cents);
		TEST_EXPECT(r.accounts.size() == 2 && r.accounts[0].error.empty() == !(fail & 1) &&
				r.accounts[1].error.empty() == !(fail & 2));
		if (!(fail & 1))
			TEST_EXPECT(r.accounts[0].month_to_date_cents == 1780);
		if (!(fail & 2))
			TEST_EXPECT(r.accounts[1].month_to_date_cents == 1275);
		const auto calls = aws.calls.size();
		steady = infra::CostStore::kFailureCacheMs - 1;
		TEST_EXPECT(!store.read().read && aws.calls.size() == calls);
		steady += 2;
		aws.answer_call = combined_reply;
		TEST_EXPECT(store.read().read && aws.calls.size() > calls);
	}
	return 0;
}

int test_ci_refusals() {
	for (const std::string failure :
			{ "sts AssumeRole", "ce GetCostForecast", "budgets DescribeBudget" }) {
		test_temp::TempDir scratch("costs_ci_refusal");
		ScriptedAws aws;
		aws.answer_call = [&failure](const Call &c) {
			if (c.key == failure && (failure == "sts AssumeRole" || is_ci(c)))
				return answer(400, R"({"__type":"AccessDeniedException","Message":"refused"})");
			return combined_reply(c);
		};
		infra::CostStore store(aws, combined(scratch.root()), [] { return kNow * 1000; });
		const auto r = store.read();
		TEST_EXPECT(r.accounts[0].error.empty() && r.accounts[0].month_to_date_cents == 1780);
		if (failure == "sts AssumeRole") {
			TEST_EXPECT(!r.read && contains(r.error, "CI:") && !r.accounts[1].error.empty());
		} else {
			TEST_EXPECT(r.read && r.error.empty() && r.month_to_date_cents == 3055);
			if (failure == "ce GetCostForecast")
				TEST_EXPECT(!r.forecast_cents && contains(r.forecast_error, "CI:") &&
						r.accounts[1].budget);
			else
				TEST_EXPECT(!r.accounts[1].budget && !r.accounts[1].budget_error.empty() &&
						r.forecast_cents);
		}
	}
	return 0;
}

int test_incomplete_usage() {
	for (const std::string failure :
			{ "pages", "currencies", "budget_currency", "forecast_currency", "malformed" }) {
		test_temp::TempDir scratch("costs_incomplete");
		ScriptedAws aws;
		aws.answer_call = [&failure](const Call &c) {
			if (is_ci(c) && c.key == "ce GetCostAndUsage" && failure == "pages")
				return answer(200, R"({"ResultsByTime":[],"NextPageToken":"more"})");
			if (is_ci(c) && c.key == "ce GetCostAndUsage" && failure == "malformed")
				return answer(200, "{}");
			auto result = combined_reply(c);
			if (is_ci(c) &&
					((failure == "currencies" && c.key == "ce GetCostAndUsage") ||
							(failure == "budget_currency" && c.key == "budgets DescribeBudget") ||
							(failure == "forecast_currency" && c.key == "ce GetCostForecast"))) {
				size_t at = 0;
				while ((at = result.out.find("USD", at)) != std::string::npos) {
					result.out.replace(at, 3, "EUR");
					at += 3;
				}
			}
			return result;
		};
		infra::CostStore store(aws, combined(scratch.root()), [] { return kNow * 1000; });
		const auto r = store.read();
		if (failure == "budget_currency")
			TEST_EXPECT(r.read && !r.accounts[1].budget && !r.accounts[1].budget_error.empty());
		else if (failure == "forecast_currency")
			TEST_EXPECT(r.read && !r.forecast_cents && !r.accounts[1].forecast_error.empty());
		else
			TEST_EXPECT(!r.read && !r.error.empty() && r.by_service.empty() &&
					r.month_to_date_cents == 0);
	}
	return 0;
}

int test_deadline_and_rollover() {
	test_temp::TempDir scratch("costs_deadline");
	ScriptedAws aws;
	int64_t steady = 0;
	aws.answer_call = [&](const Call &c) {
		steady += 12000;
		return combined_reply(c);
	};
	infra::CostStore store(
			aws, combined(scratch.root()), [] { return kNow * 1000; }, [&] { return steady; });
	const auto r = store.read();
	TEST_EXPECT(!r.read && !r.error.empty() && r.accounts[0].month_to_date_cents == 1780 &&
			!r.accounts[1].error.empty() && aws.count("sts AssumeRole") == 0);
	TEST_EXPECT(aws.last("budgets DescribeBudget")->timeout_ms == 9000);

	ScriptedAws rollover;
	rollover.answer_call = [](const Call &c) {
		if (c.key == "ce GetCostAndUsage")
			return answer(200, R"({"ResultsByTime":[]})");
		return deployment_reply(c);
	};
	int64_t wall = 1798761540000LL; // December 31, 23:59 UTC
	infra::CostStore next(
			rollover, deployment(scratch.root()), [&] { return wall; }, [] { return 0LL; });
	TEST_EXPECT(next.read().month_start == "2026-12-01");
	wall += 120000;
	TEST_EXPECT(
			next.read().month_start == "2027-01-01" && rollover.count("ce GetCostAndUsage") == 2);
	return 0;
}

int test_invalid_usage_and_config() {
	for (const std::string body : { R"({"ResultsByTime":[null]})",
				 R"({"ResultsByTime":[{"TimePeriod":{"Start":"2026-10-01"}}]})",
				 R"({"ResultsByTime":[{"Groups":[]}]})",
				 R"({"ResultsByTime":[{"TimePeriod":{"Start":"2026-09-30"},"Groups":[]}]})",
				 R"({"ResultsByTime":[],"NextPageToken":12})" }) {
		test_temp::TempDir scratch("costs_invalid_usage");
		ScriptedAws aws;
		aws.answer_call = [&](const Call &c) {
			if (is_ci(c) && c.key == "ce GetCostAndUsage")
				return answer(200, body);
			return combined_reply(c);
		};
		infra::CostStore store(aws, combined(scratch.root()), [] { return kNow * 1000; });
		const auto r = store.read();
		TEST_EXPECT(!r.read && !r.accounts[1].error.empty() &&
				r.accounts[0].month_to_date_cents == 1780);
	}
	for (int invalid = 0; invalid < 5; ++invalid) {
		test_temp::TempDir scratch("costs_invalid_config");
		ScriptedAws aws;
		aws.answer_call = combined_reply;
		auto config = combined(scratch.root());
		if (invalid == 0)
			config.ci_role_arn.clear();
		if (invalid == 1)
			config.ci_budget_arn.clear();
		if (invalid == 2)
			config.ci_role_arn = "not-a-role";
		if (invalid == 3)
			config.ci_budget_arn = kBudgetArn;
		if (invalid == 4) {
			config.ci_role_arn = "arn:aws:iam::123456789012:role/opennova-ci-cost-read";
			config.ci_budget_arn = kBudgetArn;
		}
		infra::CostStore store(aws, config, [] { return kNow * 1000; });
		const auto r = store.read();
		TEST_EXPECT(!r.read && !r.accounts[1].error.empty() &&
				r.accounts[0].month_to_date_cents == 1780 && aws.count("sts AssumeRole") == 0);
	}
	return 0;
}

int test_month() {
	test_temp::TempDir scratch("costs_month");
	ScriptedAws aws;
	aws.answer_call = deployment_reply;
	infra::CostStore store(
			aws, deployment(scratch.root()), [] { return kNow * 1000; },
			[] { return int64_t{ 0 }; });
	TEST_EXPECT(store.unavailable().empty());
	const infra::CostRead r = store.read();
	TEST_EXPECT(r.read && r.error.empty() && r.unavailable.empty() && r.read_ms == kNow * 1000);
	TEST_EXPECT(r.currency == "USD" && r.estimated);
	TEST_EXPECT(r.month_start == "2026-10-01" && r.month_end == "2026-11-01" &&
			r.through == "2026-10-19");

	// The month so far: 3.1 + 0.5 + 3.2 + 0.004 + 1.25 + 9.75 = 17.804.
	TEST_EXPECT(r.month_to_date_cents == 1780);
	TEST_EXPECT(r.by_service.size() == 3);
	TEST_EXPECT(r.by_service[0].service == "Amazon Elastic Compute Cloud - Compute" &&
			r.by_service[0].cents == 975);
	TEST_EXPECT(r.by_service[1].service == "Amazon Elastic Container Service" &&
			r.by_service[1].cents == 755);
	TEST_EXPECT(r.by_service[2].service == "Amazon Relational Database Service" &&
			r.by_service[2].cents == 50);
	// Every day of the month so far, the ones Cost Explorer had nothing for as nothing.
	TEST_EXPECT(r.days.size() == 19 && r.days[0].date == "2026-10-01" && r.days[0].cents == 360 &&
			r.days[1].cents == 320 && r.days[2].date == "2026-10-03" && r.days[2].cents == 0 &&
			r.days[18].date == "2026-10-19" && r.days[18].cents == 1100);
	// The forecast: the month before today (6.804) and AWS's from today (110.4).
	TEST_EXPECT(r.forecast_cents && *r.forecast_cents == 11720 && r.forecast_error.empty());
	TEST_EXPECT(r.budget && r.budget_error.empty() &&
			r.budget->name == "opennova-prod-monthly-cost" && r.budget->currency == "USD" &&
			r.budget->limit_cents == 25000 && r.budget->actual_cents &&
			*r.budget->actual_cents == 1681 && r.budget->forecast_cents &&
			*r.budget->forecast_cents == 12420);

	// The calls: Cost Explorer's and Budgets' global endpoints, signed for us-east-1.
	TEST_EXPECT(aws.count("ce GetCostAndUsage") == 2 && aws.count("ce GetCostForecast") == 1 &&
			aws.count("budgets DescribeBudget") == 1);
	const Call *usage = aws.last("ce GetCostAndUsage");
	TEST_EXPECT(usage->url == "https://ce.us-east-1.amazonaws.com/" &&
			usage->sigv4 == "aws:amz:us-east-1:ce");
	TEST_EXPECT(contains(usage->body,
			R"({"TimePeriod":{"Start":"2026-10-01","End":"2026-10-20"},"Granularity":"DAILY",)"
			R"("Metrics":["UnblendedCost"],"GroupBy":[{"Type":"DIMENSION","Key":"SERVICE"}],)"
			R"("NextPageToken":"page2"})"));
	const Call *forecast = aws.last("ce GetCostForecast");
	TEST_EXPECT(forecast->body ==
			R"({"TimePeriod":{"Start":"2026-10-19","End":"2026-11-01"},"Metric":"UNBLENDED_COST","Granularity":"MONTHLY"})");
	const Call *budget = aws.last("budgets DescribeBudget");
	TEST_EXPECT(budget->url == "https://budgets.amazonaws.com/" &&
			budget->sigv4 == "aws:amz:us-east-1:budgets");
	TEST_EXPECT(budget->body ==
			R"({"AccountId":"123456789012","BudgetName":"opennova-prod-monthly-cost"})");
	for (const Call &c : aws.calls) {
		TEST_EXPECT(c.argv.size() == 4 && c.argv[1] == "-q" && c.argv[2] == "--config");
		for (const std::string &arg : c.argv)
			TEST_EXPECT(!contains(arg, infra_stub::kSecret) && !contains(arg, infra_stub::kKeyId));
	}
	std::printf("  the month by day and by service, two pages, the forecast, the budget\n");
	return 0;
}

int test_refusals() {
	test_temp::TempDir scratch("costs_refusals");
	ScriptedAws aws;
	aws.answer_call = [](const Call &c) {
		if (c.key == "ce GetCostForecast")
			return answer(400,
					R"({"__type":"DataUnavailableException","Message":"Insufficient amount of historical data to generate forecast."})");
		if (c.key == "budgets DescribeBudget")
			return answer(400,
					R"({"__type":"AccessDeniedException","Message":"User is not authorized to perform: budgets:ViewBudget"})");
		return deployment_reply(c);
	};
	infra::CostStore store(
			aws, deployment(scratch.root()), [] { return kNow * 1000; },
			[] { return int64_t{ 0 }; });
	const infra::CostRead r = store.read();
	TEST_EXPECT(r.read && r.error.empty() && r.month_to_date_cents == 1780);
	TEST_EXPECT(!r.forecast_cents &&
			contains(r.forecast_error, "ce answered 400: DataUnavailableException: Insufficient"));
	TEST_EXPECT(!r.budget &&
			contains(r.budget_error,
					"budgets answered 400: AccessDeniedException: User is not authorized"));
	TEST_EXPECT(!contains(r.budget_error, infra_stub::kSecret));

	// A budget ARN that is none: nothing asked of Budgets.
	test_temp::TempDir scratch2("costs_bad_budget");
	ScriptedAws aws2;
	aws2.answer_call = deployment_reply;
	infra::CostConfig bad = deployment(scratch2.root());
	bad.budget_arn = "arn:aws:s3:::opennova-prod-ops";
	infra::CostStore store2(aws2, bad, [] { return kNow * 1000; }, [] { return int64_t{ 0 }; });
	const infra::CostRead r2 = store2.read();
	TEST_EXPECT(r2.read && !r2.budget && r2.budget_error == "ONNET_INFRA_BUDGET is no budget ARN" &&
			aws2.count("budgets DescribeBudget") == 0);
	// None named: no budget, and no error.
	test_temp::TempDir scratch3("costs_no_budget");
	ScriptedAws aws3;
	aws3.answer_call = deployment_reply;
	infra::CostConfig none = deployment(scratch3.root());
	none.budget_arn.clear();
	infra::CostStore store3(aws3, none, [] { return kNow * 1000; }, [] { return int64_t{ 0 }; });
	const infra::CostRead r3 = store3.read();
	TEST_EXPECT(r3.read && !r3.budget && r3.budget_error.empty() &&
			aws3.count("budgets DescribeBudget") == 0);
	std::printf("  a refused forecast and budget, the month still read; a budget that is none\n");
	return 0;
}

int test_month_ends() {
	// 2026-12-31T23:59:00Z: December's last day, the next month a new year's.
	test_temp::TempDir scratch("costs_december");
	ScriptedAws aws;
	aws.answer_call = [](const Call &c) {
		if (c.key == "ce GetCostAndUsage")
			return answer(200, R"({"ResultsByTime":[]})");
		return deployment_reply(c);
	};
	infra::CostStore store(
			aws, deployment(scratch.root()), [] { return int64_t{ 1798761540 } * 1000; },
			[] { return int64_t{ 0 }; });
	const infra::CostRead r = store.read();
	TEST_EXPECT(r.read && r.month_start == "2026-12-01" && r.month_end == "2027-01-01" &&
			r.through == "2026-12-31");
	TEST_EXPECT(r.month_to_date_cents == 0 && r.by_service.empty() && r.days.size() == 31 &&
			r.days[30].date == "2026-12-31" && r.days[30].cents == 0);
	TEST_EXPECT(contains(aws.last("ce GetCostAndUsage")->body,
			R"("TimePeriod":{"Start":"2026-12-01","End":"2027-01-01"})"));
	TEST_EXPECT(contains(aws.last("ce GetCostForecast")->body,
			R"("TimePeriod":{"Start":"2026-12-31","End":"2027-01-01"})"));
	// AWS's forecast of the rest is all of it: never below the month so far.
	TEST_EXPECT(r.forecast_cents && *r.forecast_cents == 11040);
	std::printf("  December's last day: the month and the next year's first\n");
	return 0;
}

int test_cache() {
	test_temp::TempDir scratch("costs_cache");
	ScriptedAws aws;
	bool failing = true;
	aws.answer_call = [&failing](const Call &c) {
		if (failing && c.key == "ce GetCostAndUsage")
			return answer(400,
					R"({"__type":"AccessDeniedException","Message":"User is not authorized to perform: ce:GetCostAndUsage"})");
		return deployment_reply(c);
	};
	int64_t steady = 0;
	infra::CostStore store(
			aws, deployment(scratch.root()), [] { return kNow * 1000; },
			[&steady] { return steady; });
	// A failed read: AWS's words, kept 10 minutes.
	infra::CostRead r = store.read();
	TEST_EXPECT(!r.read &&
			contains(r.error, "ce answered 400: AccessDeniedException: User is not authorized"));
	TEST_EXPECT(aws.count("ce GetCostAndUsage") == 1 && aws.count("ce GetCostForecast") == 0);
	steady += infra::CostStore::kFailureCacheMs - 1;
	r = store.read();
	TEST_EXPECT(!r.read && aws.count("ce GetCostAndUsage") == 1);
	steady += 2;
	failing = false;
	r = store.read();
	TEST_EXPECT(r.read && aws.count("ce GetCostAndUsage") == 3);
	// A read: kept 6 hours.
	steady += infra::CostStore::kCacheMs - 1;
	r = store.read();
	TEST_EXPECT(
			r.read && aws.count("ce GetCostAndUsage") == 3 && aws.count("ce GetCostForecast") == 1);
	steady += 2;
	r = store.read();
	TEST_EXPECT(
			r.read && aws.count("ce GetCostAndUsage") == 5 && aws.count("ce GetCostForecast") == 2);
	std::printf("  the cache: a read kept 6 h, a failed one 10 min\n");
	return 0;
}

int test_local() {
	ScriptedAws aws;
	infra::CostConfig c;
	c.budget_arn = kBudgetArn;
	infra::CostStore store(aws, c);
	const infra::CostRead r = store.read();
	TEST_EXPECT(!store.unavailable().empty() && r.unavailable == store.unavailable() &&
			contains(r.unavailable, "AWS costs are not read here") && !r.read);
	TEST_EXPECT(aws.calls.empty());
	std::printf("  the local stack: nothing called, and why\n");
	return 0;
}

int test_budget_arns() {
	std::string account;
	std::string name;
	TEST_EXPECT(infra::parse_budget_arn(kBudgetArn, account, name) && account == "123456789012" &&
			name == "opennova-prod-monthly-cost");
	for (const char *bad : { "", "opennova-prod-monthly-cost", "arn:aws:budgets::12345:budget/x",
				 "arn:aws:budgets:us-east-1:123456789012:budget/x",
				 "arn:aws:budgets::123456789012:action/x", "arn:aws:budgets::123456789012:budget/",
				 "arn:aws:budgets::123456789012:budget/a\"b", "arn:aws:ce::123456789012:budget/x" })
		TEST_EXPECT(!infra::parse_budget_arn(bad, account, name));
	std::printf("  budget ARNs\n");
	return 0;
}

} // namespace

int main() {
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "combined", test_combined },
		{ "account_failures", test_account_failures },
		{ "ci_refusals", test_ci_refusals },
		{ "incomplete_usage", test_incomplete_usage },
		{ "deadline_and_rollover", test_deadline_and_rollover },
		{ "invalid_usage_and_config", test_invalid_usage_and_config },
		{ "month", test_month },
		{ "refusals", test_refusals },
		{ "month_ends", test_month_ends },
		{ "cache", test_cache },
		{ "local", test_local },
		{ "budget_arns", test_budget_arns },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the month's AWS costs\n");
	return 0;
}
