#include "log_routes.h"

#include "http_json.h"
#include "infra_status.h"
#include "loggable.h"
#include "text_util.h"

#include <base/io/strutil.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <utility>

namespace opennova::novaworld_server::infra {

namespace {

using db::BindValue;

// A row id from the query string; nullopt for none (a number 1..2^63-1).
std::optional<int64_t> id_param(const char *text) {
	if (text == nullptr)
		return std::nullopt;
	const std::string v(text);
	if (v.empty() || v.size() > 18 || !strutil::all_digits(v))
		return std::nullopt;
	const auto n = strutil::parse_llong(v);
	if (!n || *n < 1)
		return std::nullopt;
	return static_cast<int64_t>(*n);
}

bool is_task_char(char c) {
	return is_alnum(c) || c == '.' || c == '_' || c == '-';
}

// Unix milliseconds as ISO 8601 UTC with them ("2026-10-18T14:05:03.123Z").
std::string iso_ms(int64_t ms) {
	const std::time_t secs = static_cast<std::time_t>(ms / 1000);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &secs);
#else
	gmtime_r(&secs, &tm);
#endif
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
			tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			static_cast<int>(ms % 1000));
	return buf;
}

} // namespace

LogRoutes::LogRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis) :
		access_(access),
		pool_(pool),
		reads_(redis, kReadsPerAdmin),
		programs_(InfraConfig().programs) {}

void LogRoutes::configure(LogStore *store, std::string none_why) {
	store_ = store;
	none_why_ = std::move(none_why);
}

void LogRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/logs").methods("GET"_method)([this](const crow::request &req) {
		return logs(req);
	});
}

bool LogRoutes::enter(const std::string &key) {
	std::lock_guard<std::mutex> lock(mu_);
	int &n = in_flight_[key];
	if (n >= kInFlightPerAdmin)
		return false;
	++n;
	return true;
}

void LogRoutes::leave(const std::string &key) {
	std::lock_guard<std::mutex> lock(mu_);
	if (const auto it = in_flight_.find(key); it != in_flight_.end() && --it->second <= 0)
		in_flight_.erase(it);
}

// GET /api/admin/logs — {"subject", "id", "generation" (a server's start read;
// null for none yet), "task" (a program's; null for every task), "source"
// ("cloudwatch" or "file"), "group" and "stream" (where the store read: a
// CloudWatch group and stream, a program's every task their prefix; a
// directory and the file under it; null with nothing to read yet), "lines"
// (oldest first, each {"at" (ISO 8601 UTC with milliseconds; null for a line
// with no time), "text"}), "before" (the cursor of the lines older than these;
// null for none)}. 400 invalid_field; 404 not_found (no such server, start,
// machine, program or task); 429 rate_limited; 502 logs_unavailable (the store
// failed: AWS's words); 503 no_log_store. no-store.
crow::response LogRoutes::logs(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	const std::string who = access.user ? "logs|" + std::to_string(access.user->id) : "logs|token";
	if (const int64_t wait = reads_.take(who, RateLimiter::Clock::now()))
		return too_many_requests(wait);

	const char *subject_text = req.url_params.get("subject");
	const std::string subject = subject_text != nullptr ? subject_text : "";
	LogQuery query;
	if (subject == "server")
		query.source.subject = LogSource::Subject::Server;
	else if (subject == "machine")
		query.source.subject = LogSource::Subject::Machine;
	else if (subject == "program")
		query.source.subject = LogSource::Subject::Program;
	else
		return json_error(400, "invalid_field", "subject is server, machine or program");

	const char *id_text = req.url_params.get("id");
	const std::string id = id_text != nullptr ? id_text : "";
	if (const char *limit = req.url_params.get("limit")) {
		const std::optional<int64_t> n = id_param(limit);
		if (!n || *n > static_cast<int64_t>(LogStore::kMaxLimit))
			return json_error(400, "invalid_field", "limit is 1 to 500");
		query.limit = static_cast<size_t>(*n);
	} else {
		query.limit = kDefaultLimit;
	}
	if (const char *before = req.url_params.get("before")) {
		query.before = before;
		if (query.before.size() > 1024 || has_control_byte(query.before))
			return json_error(400, "invalid_field", "before is no cursor of this log");
	}
	if (const char *q = req.url_params.get("q")) {
		query.q = q;
		// A filter pattern's term is quoted text: no quote or backslash in it.
		if (query.q.size() > kMaxQueryBytes || has_control_byte(query.q) ||
				query.q.find_first_of("\"\\") != std::string::npos)
			return json_error(400, "invalid_field",
					"q is at most 200 bytes of text with no quote or backslash");
	}

	crow::json::wvalue out;
	out["subject"] = subject;
	out["generation"] = nullptr;
	out["task"] = nullptr;
	try {
		auto db_conn = pool_.acquire();
		db::Database &db = *db_conn;
		switch (query.source.subject) {
			case LogSource::Subject::Server: {
				const std::optional<int64_t> server = id_param(id.c_str());
				const auto rows = server ? db.query("SELECT generation FROM hosted_servers "
													"WHERE id = $1;",
												   { BindValue(*server) })
										 : std::vector<db::Row>{};
				if (rows.empty())
					return json_error(404, "not_found", "no hosted server has this id");
				const int64_t newest = rows.front().as_int(0).value_or(0);
				int64_t generation = newest;
				if (const char *g = req.url_params.get("generation")) {
					const std::optional<int64_t> asked = id_param(g);
					if (!asked)
						return json_error(400, "invalid_field", "generation is a start's number");
					if (*asked > newest)
						return json_error(404, "not_found", "this server has no such start");
					generation = *asked;
				}
				out["id"] = *server;
				query.source.id = *server;
				query.source.generation = generation;
				if (generation > 0) {
					out["generation"] = generation;
					const auto start = db.query("SELECT log_stream FROM hosted_server_starts "
												"WHERE server_id = $1 AND generation = $2;",
							{ BindValue(*server), BindValue(generation) });
					if (!start.empty())
						query.source.stream = start.front().as_text(0).value_or("");
				}
				break;
			}
			case LogSource::Subject::Machine: {
				const std::optional<int64_t> machine = id_param(id.c_str());
				const auto rows = machine ? db.query("SELECT agent_log_stream FROM "
													 "fleet_machines WHERE id = $1;",
													{ BindValue(*machine) })
										  : std::vector<db::Row>{};
				if (rows.empty())
					return json_error(404, "not_found", "no fleet machine has this id");
				out["id"] = *machine;
				query.source.id = *machine;
				query.source.stream = rows.front().as_text(0).value_or("");
				break;
			}
			case LogSource::Subject::Program: {
				bool known = false;
				for (const std::string &p : programs_)
					known = known || p == id;
				if (!known)
					return json_error(404, "not_found", "the service runs no program of this name");
				out["id"] = id;
				query.source.program = id;
				if (const char *task = req.url_params.get("task")) {
					const std::string t = task;
					// A task this program has run: one that sampled itself.
					if (t.empty() || t.size() > 64 || !all_of(t, is_task_char) ||
							db.query("SELECT 1 FROM program_samples WHERE program = $1 AND "
									 "task_id = $2 UNION ALL SELECT 1 FROM program_hourly "
									 "WHERE program = $1 AND task_id = $2 LIMIT 1;",
									  { BindValue(id), BindValue(t) })
									.empty())
						return json_error(404, "not_found", "this program has run no such task");
					out["task"] = t;
					query.source.task = t;
				}
				break;
			}
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/logs failed: %s\n", e.what());
		return json_error(500, "db_error");
	}

	if (store_ == nullptr)
		return json_error(503, "no_log_store", none_why_);
	if (!enter(who))
		return too_many_requests(1);
	LogPage page;
	try {
		page = store_->read(query);
	} catch (...) {
		leave(who);
		throw;
	}
	leave(who);
	if (page.bad_cursor)
		return json_error(400, "invalid_field", "before is no cursor of this log");
	if (!page.error.empty()) {
		std::fprintf(stderr, "[http] GET /api/admin/logs: %s\n", loggable(page.error, 300).c_str());
		return json_error(502, "logs_unavailable", page.error);
	}

	out["source"] = store_->source();
	text_or_null(out, "group", page.group);
	text_or_null(out, "stream", page.stream);
	std::vector<crow::json::wvalue> lines;
	lines.reserve(page.lines.size());
	for (const LogLine &l : page.lines) {
		crow::json::wvalue line;
		if (l.at_ms >= 0)
			line["at"] = iso_ms(l.at_ms);
		else
			line["at"] = nullptr;
		line["text"] = l.text;
		lines.push_back(std::move(line));
	}
	out["lines"] = std::move(lines);
	text_or_null(out, "before", page.before);
	return no_store(json_reply(200, out));
}

} // namespace opennova::novaworld_server::infra
