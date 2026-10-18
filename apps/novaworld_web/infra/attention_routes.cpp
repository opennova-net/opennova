#include "attention_routes.h"

#include "hosted/model/stats_repository.h" // iso_utc
#include "http_json.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::infra {

namespace {

void time_or_null(crow::json::wvalue &out, const char *key, int64_t unix_seconds) {
	if (unix_seconds > 0)
		out[key] = hosted::iso_utc(unix_seconds);
	else
		out[key] = nullptr;
}

} // namespace

void AttentionRoutes::configure(AlarmStore *alarms, bool hosting_on,
		const hosted::PlacementPolicy &policy, hosted::ReleaseImages release) {
	alarms_ = alarms;
	hosting_on_ = hosting_on;
	policy_ = policy;
	release_ = std::move(release);
}

void AttentionRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/attention").methods("GET"_method)([this](const crow::request &req) {
		return attention(req);
	});
}

// GET /api/admin/attention — the findings and the alarms (the header's shape); 429
// rate_limited; 500 db_error. no-store.
crow::response AttentionRoutes::attention(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	const std::string who =
			access.user ? "attention|" + std::to_string(access.user->id) : "attention|token";
	if (const int64_t wait = reads_.take(who, RateLimiter::Clock::now()))
		return too_many_requests(wait);

	std::vector<hosted::Finding> findings;
	int64_t now = 0;
	try {
		auto db = pool_.acquire();
		const hosted::HealthInputs in = hosted::read_health(*db, hosting_on_ ? &policy_ : nullptr);
		now = in.now;
		findings = hosted::fleet_health(in, release_);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/attention failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	AlarmRead alarms;
	if (alarms_ != nullptr)
		alarms = alarms_->read();
	// Alarms that could not be read are no quiet empty list.
	if (!alarms.error.empty()) {
		findings.push_back({ hosted::HealthLevel::Warn, "CloudWatch alarms",
				"Not read: " + alarms.error + ".", 0, "", "unknown" });
		hosted::sort_findings(findings);
	}

	crow::json::wvalue out;
	std::vector<crow::json::wvalue> list;
	for (const hosted::Finding &f : findings) {
		crow::json::wvalue j;
		j["level"] = hosted::health_level_text(f.level);
		j["subject"] = f.subject;
		j["text"] = f.text;
		time_or_null(j, "since", f.since);
		if (f.link.empty())
			j["link"] = nullptr;
		else
			j["link"] = f.link;
		j["source"] = f.source;
		list.push_back(std::move(j));
	}
	out["findings"] = std::move(list);
	std::vector<crow::json::wvalue> firing;
	for (const FiringAlarm &a : alarms.alarms) {
		crow::json::wvalue j;
		j["name"] = a.name;
		time_or_null(j, "state_since", a.state_since);
		j["description"] = a.description;
		firing.push_back(std::move(j));
	}
	out["alarms"] = std::move(firing);
	out["alarms_read"] = alarms.read;
	out["checked_at"] = hosted::iso_utc(now);
	return no_store(json_reply(200, out));
}

} // namespace opennova::novaworld_server::infra
