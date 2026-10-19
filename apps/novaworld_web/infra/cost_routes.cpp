#include "cost_routes.h"

#include "hosted/model/stats_repository.h" // iso_utc
#include "http_json.h"

#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::infra {

namespace {

void cents_or_null(crow::json::wvalue &out, const char *key, const std::optional<int64_t> &cents) {
	if (cents)
		out[key] = *cents;
	else
		out[key] = nullptr;
}

} // namespace

void CostRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/costs").methods("GET"_method)([this](const crow::request &req) {
		return costs(req);
	});
}

// GET /api/admin/costs — the month's AWS costs (the header's shape); 429 rate_limited.
// no-store.
crow::response CostRoutes::costs(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	const std::string who =
			access.user ? "costs|" + std::to_string(access.user->id) : "costs|token";
	if (const int64_t wait = reads_.take(who, RateLimiter::Clock::now()))
		return too_many_requests(wait);

	CostRead read;
	if (store_ != nullptr)
		read = store_->read();
	else
		read.unavailable = "AWS costs are not read here: no cost store";

	crow::json::wvalue out;
	out["available"] = read.unavailable.empty();
	text_or_null(out, "unavailable", read.unavailable);
	text_or_null(out, "error", read.error);
	if (read.read_ms > 0)
		out["read_at"] = hosted::iso_utc(read.read_ms / 1000);
	else
		out["read_at"] = nullptr;
	out["currency"] = read.currency;
	if (read.month_start.empty()) {
		out["month"] = nullptr;
	} else {
		out["month"]["start"] = read.month_start;
		out["month"]["end"] = read.month_end;
	}
	text_or_null(out, "through", read.through);
	out["estimated"] = read.estimated;
	out["month_to_date_cents"] = read.month_to_date_cents;
	cents_or_null(out, "forecast_cents", read.forecast_cents);
	text_or_null(out, "forecast_error", read.forecast_error);
	std::vector<crow::json::wvalue> services;
	for (const CostLine &line : read.by_service) {
		crow::json::wvalue j;
		j["service"] = line.service;
		j["cents"] = line.cents;
		services.push_back(std::move(j));
	}
	out["by_service"] = std::move(services);
	std::vector<crow::json::wvalue> days;
	for (const CostDay &day : read.days) {
		crow::json::wvalue j;
		j["date"] = day.date;
		j["cents"] = day.cents;
		days.push_back(std::move(j));
	}
	out["days"] = std::move(days);
	if (read.budget) {
		const CostBudget &b = *read.budget;
		crow::json::wvalue j;
		j["name"] = b.name;
		j["currency"] = b.currency;
		j["limit_cents"] = b.limit_cents;
		cents_or_null(j, "actual_cents", b.actual_cents);
		cents_or_null(j, "forecast_cents", b.forecast_cents);
		out["budget"] = std::move(j);
	} else {
		out["budget"] = nullptr;
	}
	text_or_null(out, "budget_error", read.budget_error);
	return no_store(json_reply(200, out));
}

} // namespace opennova::novaworld_server::infra
