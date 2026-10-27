#include "cost_store.h"

#include "core/clock.h"
#include "core/loggable.h"
#include "core/text_util.h"
#include "hosted/model/stats_repository.h"
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
	return iso_utc(s).substr(0, 10);
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

std::string ci_config_error(const CostConfig &config) {
	const std::string &role = config.ci_role_arn;
	if (!starts_with(role, "arn:aws:iam::") || role.size() <= 31 ||
			!strutil::all_digits(role.substr(13, 12)) || role.substr(25, 6) != ":role/" ||
			has_control_byte(role))
		return "ONNET_INFRA_CI_COST_ROLE is no IAM role ARN";
	const std::string ci_account = role.substr(13, 12);
	std::string account;
	std::string name;
	if (!parse_budget_arn(config.ci_budget_arn, account, name) || account != ci_account)
		return "ONNET_INFRA_CI_BUDGET must name a budget in the CI role's account";
	if (!parse_budget_arn(config.budget_arn, account, name))
		return "ONNET_INFRA_BUDGET must identify production's account when CI costs are read";
	if (account == ci_account)
		return "CI and production must use different AWS accounts";
	return {};
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
	if (const char *role = std::getenv("ONNET_INFRA_CI_COST_ROLE"))
		c.ci_role_arn = std::string(strutil::trim_view(role));
	if (const char *budget = std::getenv("ONNET_INFRA_CI_BUDGET"))
		c.ci_budget_arn = std::string(strutil::trim_view(budget));
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
						  : Clock([] { return static_cast<int64_t>(steady_now_ms()); })) {}

CostRead CostStore::read() {
	std::lock_guard<std::mutex> lock(mu_);
	if (cached_ && cache_.month_start == utc_date(wall_() / 1000).substr(0, 8) + "01" &&
			steady_() - cached_at_ < (cache_.read ? kCacheMs : kFailureCacheMs))
		return cache_;
	CostRead out = read_uncached();
	if (out.unavailable.empty()) {
		cache_ = out;
		cached_at_ = steady_();
		cached_ = true;
	}
	return out;
}

int CostStore::remaining_ms(int64_t deadline) const {
	return static_cast<int>(std::clamp<int64_t>(deadline - steady_(), 0, kRefreshMs));
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
	out.month_start = today.substr(0, 8) + "01";
	out.month_end = next_month(today);
	out.through = today;

	const int64_t deadline = steady_() + kRefreshMs;
	const bool include_ci = !config_.ci_role_arn.empty() || !config_.ci_budget_arn.empty();
	AwsCredentials creds;
	std::string credentials_error;
	const bool have_credentials =
			api_.credentials(creds, credentials_error, remaining_ms(deadline));
	CostRead deployment;
	if (have_credentials)
		deployment = read_account(creds, config_.budget_arn, "ONNET_INFRA_BUDGET", out, deadline);
	else
		deployment.error = credentials_error;
	auto account = [](const char *id, const char *label, const CostRead &read) {
		return CostAccount{ id, label, read.currency, read.error, read.month_to_date_cents,
			read.forecast_cents, read.forecast_error, read.budget, read.budget_error };
	};
	if (!include_ci) {
		// Preserve the period even when reading the ECS credentials failed.
		deployment.read_ms = out.read_ms;
		deployment.month_start = out.month_start;
		deployment.month_end = out.month_end;
		deployment.through = out.through;
		deployment.accounts.push_back(account("deployment", "This deployment", deployment));
		return deployment;
	}

	CostRead ci;
	AwsCredentials ci_credentials;
	const std::string ci_config_failure = ci_config_error(config_);
	if (!have_credentials) {
		ci.error = credentials_error;
	} else if (!ci_config_failure.empty()) {
		ci.error = ci_config_failure;
	} else if (api_.assume_role(creds, config_.ci_role_arn, ci_credentials, ci.error,
					   remaining_ms(deadline))) {
		ci = read_account(
				ci_credentials, config_.ci_budget_arn, "ONNET_INFRA_CI_BUDGET", out, deadline);
	}
	out.accounts.push_back(account("deployment", "Production", deployment));
	out.accounts.push_back(account("ci", "CI", ci));
	out.currency = deployment.currency;
	out.estimated = deployment.estimated || ci.estimated;
	for (const auto &source : out.accounts) {
		if (!source.error.empty()) {
			if (!out.error.empty())
				out.error += "; ";
			out.error += source.label + ": " + source.error;
		}
	}
	if (!out.error.empty())
		return out;
	if (deployment.currency != ci.currency) {
		out.error =
				"Production and CI costs use different currencies; no combined total is available";
		return out;
	}
	out.read = true;
	out.month_to_date_cents = deployment.month_to_date_cents + ci.month_to_date_cents;
	if (deployment.forecast_cents && ci.forecast_cents) {
		out.forecast_cents = *deployment.forecast_cents + *ci.forecast_cents;
	} else {
		for (const auto &source : out.accounts) {
			if (!source.forecast_cents) {
				if (!out.forecast_error.empty())
					out.forecast_error += "; ";
				out.forecast_error += source.label + ": " + source.forecast_error;
			}
		}
	}
	std::map<std::string, int64_t> services;
	std::map<std::string, int64_t> days;
	for (const CostRead *source : { &deployment, &ci }) {
		for (const auto &line : source->by_service)
			services[line.service] += line.cents;
		for (const auto &day : source->days)
			days[day.date] += day.cents;
	}
	for (const auto &[service, cents] : services) {
		if (cents != 0)
			out.by_service.push_back({ service, cents });
	}
	std::stable_sort(out.by_service.begin(), out.by_service.end(),
			[](const CostLine &a, const CostLine &b) { return a.cents > b.cents; });
	for (const auto &[date, cents] : days)
		out.days.push_back({ date, cents });
	return out;
}

CostRead CostStore::read_account(const AwsCredentials &creds, const std::string &budget_arn,
		const char *budget_setting, const CostRead &period, int64_t deadline) {
	CostRead out;
	out.read_ms = period.read_ms;
	out.month_start = period.month_start;
	out.month_end = period.month_end;
	out.through = period.through;
	const std::string &today = out.through;
	const std::string tomorrow = utc_date(out.read_ms / 1000 + 24 * 60 * 60);

	// The month so far by day and by service. Cost Explorer's dates are UTC
	// days, its End exclusive.
	std::map<std::string, double> by_service;
	std::map<std::string, double> by_day;
	bool currency_seen = false;
	std::string token;
	for (int page = 0; page < kPages; ++page) {
		std::string body = "{\"TimePeriod\":{\"Start\":" + io::json_quote(out.month_start) +
				",\"End\":" + io::json_quote(tomorrow) +
				"},\"Granularity\":\"DAILY\",\"Metrics\":[\"UnblendedCost\"],"
				"\"GroupBy\":[{\"Type\":\"DIMENSION\",\"Key\":\"SERVICE\"}]";
		if (!token.empty())
			body += ",\"NextPageToken\":" + io::json_quote(token);
		body += "}";
		JsonValue reply;
		if (!json_reply("ce",
					api_.global_json_call(creds, "ce", kCostExplorerHost, kSigningRegion,
							std::string(kCostExplorerTarget) + "GetCostAndUsage", body,
							remaining_ms(deadline)),
					reply, out.error))
			return out;
		if (member(reply, "ResultsByTime", JsonValue::Type::Array) == nullptr) {
			out.error = "ce answered no usage results";
			return out;
		}
		for (const JsonValue &result : array_of(reply, "ResultsByTime")) {
			const std::string day = object_of(result, "TimePeriod").get_string("Start", "");
			if (!result.is_object() || day.size() != 10 ||
					day.substr(0, 8) != out.month_start.substr(0, 8) ||
					!strutil::all_digits(day.substr(8, 2)) || day < out.month_start ||
					day > today || member(result, "Groups", JsonValue::Type::Array) == nullptr) {
				out.error = "ce answered an invalid usage day";
				return out;
			}
			if (result.get_bool("Estimated", false))
				out.estimated = true;
			for (const JsonValue &group : array_of(result, "Groups")) {
				const std::vector<JsonValue> &keys = array_of(group, "Keys");
				const JsonValue &cost = object_of(object_of(group, "Metrics"), "UnblendedCost");
				const std::optional<double> amount = amount_of(cost);
				const std::string unit = cost.get_string("Unit", "");
				if (keys.empty() || !keys[0].is_string() || !amount || unit.empty() ||
						unit.size() > 8 || has_control_byte(unit)) {
					out.error = "ce answered an invalid usage amount";
					return out;
				}
				if (!currency_seen) {
					out.currency = unit;
					currency_seen = true;
				} else if (unit != out.currency) {
					out.error = "ce answered usage in different currencies";
					return out;
				}
				by_service[keys[0].string] += *amount;
				by_day[day] += *amount;
			}
		}
		const JsonValue *next = reply.get("NextPageToken");
		if (next != nullptr && !next->is_string()) {
			out.error = "ce answered an invalid pagination token";
			return out;
		}
		token = next != nullptr ? next->string : "";
		if (token.empty())
			break;
	}
	if (!token.empty()) {
		out.error = "ce usage exceeds the pagination limit; the account total is incomplete";
		return out;
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
		const std::string body = "{\"TimePeriod\":{\"Start\":" + io::json_quote(today) +
				",\"End\":" + io::json_quote(out.month_end) +
				"},\"Metric\":\"UNBLENDED_COST\",\"Granularity\":\"MONTHLY\"}";
		JsonValue reply;
		if (json_reply("ce",
					api_.global_json_call(creds, "ce", kCostExplorerHost, kSigningRegion,
							std::string(kCostExplorerTarget) + "GetCostForecast", body,
							remaining_ms(deadline)),
					reply, out.forecast_error)) {
			const std::optional<double> rest = amount_of(object_of(reply, "Total"));
			if (!rest) {
				out.forecast_error = "ce answered no forecast total";
			} else if (object_of(reply, "Total").get_string("Unit", "") != out.currency) {
				out.forecast_error = "ce forecast currency differs from usage";
			} else {
				const auto it = by_day.find(today);
				const double before_today = month_to_date - (it == by_day.end() ? 0 : it->second);
				out.forecast_cents =
						std::max(out.month_to_date_cents, cents_of(before_today + *rest));
			}
		}
	}

	// The deployment's monthly budget.
	if (!budget_arn.empty()) {
		std::string account;
		std::string name;
		if (!parse_budget_arn(budget_arn, account, name)) {
			out.budget_error = std::string(budget_setting) + " is no budget ARN";
		} else {
			const std::string body = "{\"AccountId\":" + io::json_quote(account) +
					",\"BudgetName\":" + io::json_quote(name) + "}";
			JsonValue reply;
			if (json_reply("budgets",
						api_.global_json_call(creds, "budgets", kBudgetsHost, kSigningRegion,
								std::string(kBudgetsTarget) + "DescribeBudget", body,
								remaining_ms(deadline)),
						reply, out.budget_error)) {
				const JsonValue &budget = object_of(reply, "Budget");
				const JsonValue &limit = object_of(budget, "BudgetLimit");
				const std::optional<double> amount = amount_of(limit);
				if (!amount) {
					out.budget_error = "budgets answered no limit for " + loggable(name, 100);
				} else if (budget.get_string("TimeUnit", "") != "MONTHLY" ||
						budget.get_string("BudgetType", "") != "COST") {
					out.budget_error = loggable(name, 100) + " is no monthly cost budget";
				} else if (limit.get_string("Unit", "") != out.currency) {
					out.budget_error = "budget currency differs from usage";
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
