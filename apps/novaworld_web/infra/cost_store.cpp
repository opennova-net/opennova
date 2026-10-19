#include "cost_store.h"

#include "core/health.h" // Watchdog::steady_now_ms
#include "core/loggable.h"
#include "core/text_util.h"
#include "hosted/model/stats_repository.h" // iso_utc
#include "infra_status.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::infra {

namespace {

using io::JsonValue;

constexpr const char *kCostExplorerTarget = "AWSInsightsIndexService.";
constexpr const char *kBudgetsTarget = "AWSBudgetServiceGateway.";

int64_t wall_now_ms() {
	using namespace std::chrono;
	return static_cast<int64_t>(
			duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// `text` as a JSON string literal.
std::string json_text(std::string_view text) {
	std::string out = "\"";
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (c == '"' || c == '\\') {
			out.push_back('\\');
			out.push_back(c);
		} else if (byte < 0x20) {
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
			out += escaped;
		} else {
			out.push_back(c);
		}
	}
	out.push_back('"');
	return out;
}

const JsonValue *member(const JsonValue &object, const char *key, JsonValue::Type type) {
	const JsonValue *v = object.get(key);
	return v != nullptr && v->type == type ? v : nullptr;
}

const std::vector<JsonValue> &array_of(const JsonValue &object, const char *key) {
	static const std::vector<JsonValue> kNone;
	const JsonValue *v = member(object, key, JsonValue::Type::Array);
	return v != nullptr ? v->array : kNone;
}

const JsonValue &object_of(const JsonValue &object, const char *key) {
	static const JsonValue kNone = JsonValue::make_object();
	const JsonValue *v = member(object, key, JsonValue::Type::Object);
	return v != nullptr ? *v : kNone;
}

// An amount AWS writes as text ({"Amount": "20.2512", "Unit": "USD"}); nullopt
// when it holds none.
std::optional<double> amount_of(const JsonValue &money) {
	const JsonValue *amount = member(money, "Amount", JsonValue::Type::String);
	if (amount == nullptr)
		return std::nullopt;
	const std::optional<double> v = strutil::parse_double(amount->string);
	if (!v || !std::isfinite(*v))
		return std::nullopt;
	return v;
}

int64_t cents_of(double amount) {
	return static_cast<int64_t>(std::llround(amount * 100.0));
}

// A reply's JSON object; false with `error` (the call's failure, or the body
// not one).
bool json_reply(const char *service, const AwsReply &reply, JsonValue &out, std::string &error) {
	if (!reply.ok()) {
		error = AwsApi::failure(service, reply);
		return false;
	}
	std::string parse_error;
	if (!io::json_parse(reply.body, out, parse_error) || !out.is_object()) {
		error = std::string(service) + " answered no JSON object";
		return false;
	}
	return true;
}

// "2026-10-19": the UTC date of Unix second `s`.
std::string utc_date(int64_t s) {
	return hosted::iso_utc(s).substr(0, 10);
}

// "2026-11-01": the first day of the month after `date`'s.
std::string next_month(const std::string &date) {
	int year = strutil::parse_int(date.substr(0, 4)).value_or(1970);
	int month = strutil::parse_int(date.substr(5, 2)).value_or(1) + 1;
	if (month > 12) {
		month = 1;
		++year;
	}
	return std::to_string(year) + (month < 10 ? "-0" : "-") + std::to_string(month) + "-01";
}

bool is_budget_name_char(char c) {
	return is_alnum(c) || c == '-' || c == '_' || c == '.';
}

} // namespace

bool parse_budget_arn(const std::string &arn, std::string &account, std::string &name) {
	// arn:<partition>:budgets::<account>:budget/<name>
	static constexpr std::string_view kHead = "arn:";
	if (!starts_with(arn, kHead) || has_control_byte(arn))
		return false;
	std::vector<std::string> parts;
	size_t from = 0;
	for (int i = 0; i < 5; ++i) {
		const size_t colon = arn.find(':', from);
		if (colon == std::string::npos)
			return false;
		parts.push_back(arn.substr(from, colon - from));
		from = colon + 1;
	}
	const std::string resource = arn.substr(from);
	static constexpr std::string_view kBudget = "budget/";
	if (parts[2] != "budgets" || !parts[3].empty() || parts[4].size() != 12 ||
			!strutil::all_digits(parts[4]) || !starts_with(resource, kBudget))
		return false;
	const std::string budget = resource.substr(kBudget.size());
	if (budget.empty() || budget.size() > 100 || !all_of(budget, is_budget_name_char))
		return false;
	account = parts[4];
	name = budget;
	return true;
}

CostConfig cost_config_from_env(const std::string &scratch_dir) {
	CostConfig c;
	c.aws = infra_config_from_env(scratch_dir).aws;
	if (const char *budget = std::getenv("ONNET_INFRA_BUDGET"))
		c.budget_arn = std::string(strutil::trim_view(budget));
	return c;
}

CostStore::CostStore(
		proc::CommandRunner &runner, CostConfig config, Clock wall_ms, Clock steady_ms) :
		config_(std::move(config)),
		unavailable_([this] {
			const std::string why = AwsApi::check_config(config_.aws);
			return why.empty() ? std::string() : "AWS costs are not read here: " + why;
		}()),
		api_(runner, config_.aws),
		wall_(wall_ms ? std::move(wall_ms) : Clock(wall_now_ms)),
		steady_(steady_ms ? std::move(steady_ms)
						  : Clock([] { return static_cast<int64_t>(Watchdog::steady_now_ms()); })) {
}

CostRead CostStore::read() {
	std::lock_guard<std::mutex> lock(mu_);
	if (cached_ && steady_() - cached_at_ < (cache_.read ? kCacheMs : kFailureCacheMs))
		return cache_;
	CostRead out = read_uncached();
	if (out.unavailable.empty()) {
		cache_ = out;
		cached_at_ = steady_();
		cached_ = true;
	}
	return out;
}

CostRead CostStore::read_uncached() {
	CostRead out;
	if (!unavailable_.empty()) {
		out.unavailable = unavailable_;
		return out;
	}
	out.read_ms = wall_();
	const int64_t now_s = out.read_ms / 1000;
	const std::string today = utc_date(now_s);
	const std::string tomorrow = utc_date(now_s + 24 * 60 * 60);
	out.month_start = today.substr(0, 8) + "01";
	out.month_end = next_month(today);
	out.through = today;

	AwsCredentials creds;
	if (!api_.credentials(creds, out.error))
		return out;

	// The month so far by day and by service. Cost Explorer's dates are UTC
	// days, its End exclusive.
	std::map<std::string, double> by_service;
	std::map<std::string, double> by_day;
	bool currency_seen = false;
	std::string token;
	for (int page = 0; page < kPages; ++page) {
		std::string body = "{\"TimePeriod\":{\"Start\":" + json_text(out.month_start) +
				",\"End\":" + json_text(tomorrow) +
				"},\"Granularity\":\"DAILY\",\"Metrics\":[\"UnblendedCost\"],"
				"\"GroupBy\":[{\"Type\":\"DIMENSION\",\"Key\":\"SERVICE\"}]";
		if (!token.empty())
			body += ",\"NextPageToken\":" + json_text(token);
		body += "}";
		JsonValue reply;
		if (!json_reply("ce",
					api_.global_json_call(creds, "ce", kCostExplorerHost, kSigningRegion,
							std::string(kCostExplorerTarget) + "GetCostAndUsage", body),
					reply, out.error))
			return out;
		for (const JsonValue &result : array_of(reply, "ResultsByTime")) {
			const std::string day = object_of(result, "TimePeriod").get_string("Start", "");
			if (result.get_bool("Estimated", false))
				out.estimated = true;
			for (const JsonValue &group : array_of(result, "Groups")) {
				const std::vector<JsonValue> &keys = array_of(group, "Keys");
				const JsonValue &cost = object_of(object_of(group, "Metrics"), "UnblendedCost");
				const std::optional<double> amount = amount_of(cost);
				if (keys.empty() || !keys[0].is_string() || !amount)
					continue;
				if (!currency_seen) {
					const std::string unit = cost.get_string("Unit", "");
					if (!unit.empty() && unit.size() <= 8 && !has_control_byte(unit)) {
						out.currency = unit;
						currency_seen = true;
					}
				}
				by_service[keys[0].string] += *amount;
				by_day[day] += *amount;
			}
		}
		token = reply.get_string("NextPageToken", "");
		if (token.empty())
			break;
	}

	double month_to_date = 0;
	for (const auto &[day, amount] : by_day)
		month_to_date += amount;
	out.month_to_date_cents = cents_of(month_to_date);
	for (const auto &[service, amount] : by_service) {
		const int64_t cents = cents_of(amount);
		if (cents != 0)
			out.by_service.push_back({ service, cents });
	}
	std::stable_sort(out.by_service.begin(), out.by_service.end(),
			[](const CostLine &a, const CostLine &b) { return a.cents > b.cents; });
	// Every day of the period, a day Cost Explorer has nothing for as nothing.
	const int last = strutil::parse_int(today.substr(8, 2)).value_or(1);
	for (int d = 1; d <= last; ++d) {
		const std::string date = today.substr(0, 8) + (d < 10 ? "0" : "") + std::to_string(d);
		const auto it = by_day.find(date);
		out.days.push_back({ date, it == by_day.end() ? 0 : cents_of(it->second) });
	}
	out.read = true;

	// The rest of the month from today (today's spend so far is partial: the
	// forecast covers the whole of it).
	{
		const std::string body = "{\"TimePeriod\":{\"Start\":" + json_text(today) +
				",\"End\":" + json_text(out.month_end) +
				"},\"Metric\":\"UNBLENDED_COST\",\"Granularity\":\"MONTHLY\"}";
		JsonValue reply;
		if (json_reply("ce",
					api_.global_json_call(creds, "ce", kCostExplorerHost, kSigningRegion,
							std::string(kCostExplorerTarget) + "GetCostForecast", body),
					reply, out.forecast_error)) {
			const std::optional<double> rest = amount_of(object_of(reply, "Total"));
			if (!rest) {
				out.forecast_error = "ce answered no forecast total";
			} else {
				const auto it = by_day.find(today);
				const double before_today = month_to_date - (it == by_day.end() ? 0 : it->second);
				out.forecast_cents =
						std::max(out.month_to_date_cents, cents_of(before_today + *rest));
			}
		}
	}

	// The deployment's monthly budget.
	if (!config_.budget_arn.empty()) {
		std::string account;
		std::string name;
		if (!parse_budget_arn(config_.budget_arn, account, name)) {
			out.budget_error = "ONNET_INFRA_BUDGET is no budget ARN";
		} else {
			const std::string body = "{\"AccountId\":" + json_text(account) +
					",\"BudgetName\":" + json_text(name) + "}";
			JsonValue reply;
			if (json_reply("budgets",
						api_.global_json_call(creds, "budgets", kBudgetsHost, kSigningRegion,
								std::string(kBudgetsTarget) + "DescribeBudget", body),
						reply, out.budget_error)) {
				const JsonValue &budget = object_of(reply, "Budget");
				const JsonValue &limit = object_of(budget, "BudgetLimit");
				const std::optional<double> amount = amount_of(limit);
				if (!amount) {
					out.budget_error = "budgets answered no limit for " + loggable(name, 100);
				} else if (budget.get_string("TimeUnit", "") != "MONTHLY" ||
						budget.get_string("BudgetType", "") != "COST") {
					out.budget_error = loggable(name, 100) + " is no monthly cost budget";
				} else {
					CostBudget b;
					b.name = budget.get_string("BudgetName", name);
					b.currency = limit.get_string("Unit", out.currency);
					b.limit_cents = cents_of(*amount);
					const JsonValue &spend = object_of(budget, "CalculatedSpend");
					if (const auto actual = amount_of(object_of(spend, "ActualSpend")))
						b.actual_cents = cents_of(*actual);
					if (const auto forecast = amount_of(object_of(spend, "ForecastedSpend")))
						b.forecast_cents = cents_of(*forecast);
					out.budget = std::move(b);
				}
			}
		}
	}
	return out;
}

} // namespace opennova::novaworld_server::infra
