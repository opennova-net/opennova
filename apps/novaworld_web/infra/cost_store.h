#pragma once

// The deployment's AWS bill this month, for the admin's costs view
// (cost_routes.h, GET /api/admin/costs): what its account is charged, read with
// web's task role through aws_api.h from AWS's two global endpoints:
//
//   Cost Explorer  GetCostAndUsage of the month so far (its first day, UTC,
//                  through today) by day and by AWS service, the unblended
//                  cost, every page of it (kPages at most); and
//                  GetCostForecast of the rest of the month (today to the
//                  first of the next), which AWS refuses while the account has
//                  too little history (then none, and why)
//   Budgets        DescribeBudget of the deployment's monthly cost budget
//                  (ONNET_INFRA_BUDGET, its ARN: infra/aws/observability.tf's
//                  monthly_cost): its limit and AWS's own spend and forecast
//                  against it; unset, none is read
//
// Production also reads CI's account through ONNET_INFRA_CI_COST_ROLE, using
// ONNET_INFRA_CI_BUDGET for its own budget. Combined figures are shown only
// when both usage reads succeed; account summaries survive a partial failure.
// Cost Explorer refreshes its figures a few times a day, up to a day behind,
// so a read whose usage AWS answered is kept kCacheMs however many admins look,
// a failed one
// kFailureCacheMs, and one read runs at a time (a caller arriving meanwhile
// waits for it). Without AWS (the local stack: no task role) nothing is called
// and the read says why. Service policy, not a port.

#include "aws_api.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::infra {

// Amounts are the account's currency's cents (Cost Explorer's figures rounded).
struct CostLine {
	std::string service; // AWS's name ("Amazon Elastic Compute Cloud - Compute")
	int64_t cents = 0;
};

struct CostDay {
	std::string date; // YYYY-MM-DD, UTC
	int64_t cents = 0;
};

struct CostBudget {
	std::string name;
	std::string currency;
	int64_t limit_cents = 0;
	// AWS Budgets' own figures for the month (none until it has computed them).
	std::optional<int64_t> actual_cents;
	std::optional<int64_t> forecast_cents;
};

struct CostAccount {
	std::string id; // "deployment" or "ci", not an AWS account id
	std::string label;
	std::string currency;
	std::string error;
	int64_t month_to_date_cents = 0;
	std::optional<int64_t> forecast_cents;
	std::string forecast_error;
	std::optional<CostBudget> budget;
	std::string budget_error;
};

struct CostRead {
	bool read = false; // Cost Explorer answered the month's usage
	std::string unavailable; // why AWS is not read here; "" when it is
	std::string error; // why the usage read failed (AWS's words)
	int64_t read_ms = 0; // Unix epoch ms the read began
	std::string currency = "USD";
	// The month (its first day, and the next month's, exclusive) and the
	// period's last day read (today, UTC).
	std::string month_start;
	std::string month_end;
	std::string through;
	bool estimated = false; // Cost Explorer marks the figures estimated (an open month)
	int64_t month_to_date_cents = 0;
	// The month's expected total: the month before today and AWS's forecast
	// from today, never below the month to date.
	std::optional<int64_t> forecast_cents;
	std::string forecast_error; // why there is none (AWS's words)
	std::vector<CostLine> by_service; // largest first; under a cent left out
	std::vector<CostDay> days; // every day of the period, oldest first
	std::optional<CostBudget> budget;
	std::string budget_error; // why the budget was not read ("" for none asked)
	std::vector<CostAccount> accounts; // summaries, including failed accounts
};

struct CostConfig {
	AwsApiConfig aws;
	// ONNET_INFRA_BUDGET: arn:aws:budgets::<account>:budget/<name>; "" for none.
	std::string budget_arn;
	std::string ci_role_arn; // ONNET_INFRA_CI_COST_ROLE; empty for local-only reads
	std::string ci_budget_arn; // ONNET_INFRA_CI_BUDGET
};

// The infrastructure view's AWS settings (infra_status.h) and
// ONNET_INFRA_BUDGET.
CostConfig cost_config_from_env(const std::string &scratch_dir);

// The account and name of a budget's ARN; false for anything else.
bool parse_budget_arn(const std::string &arn, std::string &account, std::string &name);

class CostStore {
public:
	static constexpr int64_t kCacheMs = 6LL * 60 * 60 * 1000;
	static constexpr int64_t kFailureCacheMs = 10LL * 60 * 1000;
	static constexpr int kRefreshMs = 45000; // below the portal's proxy timeout
	// GetCostAndUsage pages one read follows at most.
	static constexpr int kPages = 5;
	// Cost Explorer's and Budgets' endpoints and the region they are signed
	// for (the aws partition's).
	static constexpr const char *kCostExplorerHost = "ce.us-east-1.amazonaws.com";
	static constexpr const char *kBudgetsHost = "budgets.amazonaws.com";
	static constexpr const char *kSigningRegion = "us-east-1";

	// Milliseconds: the wall clock (the month) and a steady one (the cache).
	using Clock = std::function<int64_t()>;

	// `runner` must outlive the store.
	CostStore(proc::CommandRunner &runner, CostConfig config, Clock wall_ms = {},
			Clock steady_ms = {});
	CostStore(const CostStore &) = delete;
	CostStore &operator=(const CostStore &) = delete;

	// "" when the costs are read here, else why not (no region or task role:
	// the local stack's).
	const std::string &unavailable() const { return unavailable_; }

	// The month's costs; the last read within its window answered again.
	CostRead read();

private:
	CostRead read_uncached();
	CostRead read_account(const AwsCredentials &credentials, const std::string &budget_arn,
			const char *budget_setting, const CostRead &period, int64_t deadline);
	int remaining_ms(int64_t deadline) const;

	const CostConfig config_;
	const std::string unavailable_;
	AwsApi api_;
	Clock wall_;
	Clock steady_;
	std::mutex mu_; // held through a read: one at a time
	bool cached_ = false;
	int64_t cached_at_ = 0;
	CostRead cache_;
};

} // namespace opennova::novaworld_server::infra
