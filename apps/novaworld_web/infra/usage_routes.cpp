#include "usage_routes.h"

#include "core/text_util.h"
#include "hosted/fleet_repository.h"
#include "hosted/stats_repository.h" // iso_utc
#include "http_json.h"
#include "infra_routes.h"

#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace opennova::novaworld_server::infra {

namespace {

using db::BindValue;

std::optional<double> double_at(const db::Row &row, size_t i) {
	if (i >= row.values.size())
		return std::nullopt;
	if (const auto *d = std::get_if<double>(&row.values[i]))
		return *d;
	if (const auto *n = std::get_if<int64_t>(&row.values[i]))
		return static_cast<double>(*n);
	return std::nullopt;
}

template <typename T>
void value_or_null(crow::json::wvalue &out, const char *key, const std::optional<T> &value) {
	if (value)
		out[key] = *value;
	else
		out[key] = nullptr;
}

// One point of a subject's series.
struct Point {
	int64_t at = 0; // Unix seconds
	std::optional<double> cpu_percent;
	std::optional<int64_t> memory_bytes;
	std::optional<double> net_in_bps;
	std::optional<double> net_out_bps;
	std::optional<double> tick_p99_ms; // a server's
	std::optional<int64_t> late_ticks; // a server's
	std::optional<int64_t> disk_free_bytes; // a machine's
};

// An hour's bytes a second: over the hour, or what of it has passed by `now`.
std::optional<double> hour_rate(int64_t hour, int64_t now, std::optional<int64_t> bytes) {
	if (!bytes)
		return std::nullopt;
	const int64_t seconds = std::clamp<int64_t>(now - hour, 60, 3600);
	return static_cast<double>(*bytes) / static_cast<double>(seconds);
}

crow::json::wvalue point_json(const Point &p, bool server, bool machine) {
	crow::json::wvalue j;
	j["at"] = hosted::iso_utc(p.at);
	value_or_null(j, "cpu_percent", p.cpu_percent);
	value_or_null(j, "memory_bytes", p.memory_bytes);
	value_or_null(j, "net_in_bps", p.net_in_bps);
	value_or_null(j, "net_out_bps", p.net_out_bps);
	if (server) {
		value_or_null(j, "tick_p99_ms", p.tick_p99_ms);
		value_or_null(j, "late_ticks", p.late_ticks);
	}
	if (machine)
		value_or_null(j, "disk_free_bytes", p.disk_free_bytes);
	return j;
}

// ---- the series ------------------------------------------------------------------------

// A server's minutes since `since` (one earlier read for the first rate).
std::vector<Point> server_minutes(db::Database &db, int64_t id, int64_t since) {
	std::vector<Point> out;
	CounterRate in;
	CounterRate sent;
	for (const db::Row &r : db.query("SELECT CAST(extract(epoch FROM at) AS bigint), cpu_percent, "
									 "memory_bytes, net_in_bytes, net_out_bytes, tick_p99_ms, "
									 "late_ticks FROM hosted_server_samples WHERE server_id = $1 "
									 "AND at >= to_timestamp($2::double precision) ORDER BY at;",
				 { BindValue(id), BindValue(since - UsageRoutes::kGapSeconds) })) {
		Point p;
		p.at = r.as_int(0).value_or(0);
		p.cpu_percent = double_at(r, 1);
		p.memory_bytes = r.as_int(2);
		p.net_in_bps = in.next(p.at, r.as_int(3));
		p.net_out_bps = sent.next(p.at, r.as_int(4));
		p.tick_p99_ms = double_at(r, 5);
		p.late_ticks = r.as_int(6);
		if (p.at >= since)
			out.push_back(p);
	}
	return out;
}

std::vector<Point> server_hours(db::Database &db, int64_t id, int64_t since, int64_t now) {
	std::vector<Point> out;
	for (const db::Row &r : db.query("SELECT CAST(extract(epoch FROM hour) AS bigint), avg_cpu, "
									 "peak_memory_bytes, net_in_bytes, net_out_bytes, "
									 "tick_p99_max_ms, late_ticks FROM hosted_server_hourly "
									 "WHERE server_id = $1 AND hour >= to_timestamp($2::double "
									 "precision) ORDER BY hour;",
				 { BindValue(id), BindValue(since) })) {
		Point p;
		p.at = r.as_int(0).value_or(0);
		p.cpu_percent = double_at(r, 1);
		p.memory_bytes = r.as_int(2);
		p.net_in_bps = hour_rate(p.at, now, r.as_int(3));
		p.net_out_bps = hour_rate(p.at, now, r.as_int(4));
		p.tick_p99_ms = double_at(r, 5);
		p.late_ticks = r.as_int(6);
		out.push_back(p);
	}
	return out;
}

std::vector<Point> machine_minutes(db::Database &db, int64_t id, int64_t since) {
	std::vector<Point> out;
	CounterRate in;
	CounterRate sent;
	for (const db::Row &r : db.query("SELECT CAST(extract(epoch FROM at) AS bigint), cpu_percent, "
									 "memory_used_bytes, net_in_bytes, net_out_bytes, "
									 "disk_free_bytes FROM fleet_machine_samples WHERE "
									 "machine_id = $1 AND at >= to_timestamp($2::double "
									 "precision) ORDER BY at;",
				 { BindValue(id), BindValue(since - UsageRoutes::kGapSeconds) })) {
		Point p;
		p.at = r.as_int(0).value_or(0);
		p.cpu_percent = double_at(r, 1);
		p.memory_bytes = r.as_int(2);
		p.net_in_bps = in.next(p.at, r.as_int(3));
		p.net_out_bps = sent.next(p.at, r.as_int(4));
		p.disk_free_bytes = r.as_int(5);
		if (p.at >= since)
			out.push_back(p);
	}
	return out;
}

std::vector<Point> machine_hours(db::Database &db, int64_t id, int64_t since, int64_t now) {
	std::vector<Point> out;
	for (const db::Row &r : db.query("SELECT CAST(extract(epoch FROM hour) AS bigint), cpu_avg, "
									 "memory_peak_bytes, net_in_bytes, net_out_bytes, "
									 "disk_free_min_bytes FROM fleet_machine_hourly WHERE "
									 "machine_id = $1 AND hour >= to_timestamp($2::double "
									 "precision) ORDER BY hour;",
				 { BindValue(id), BindValue(since) })) {
		Point p;
		p.at = r.as_int(0).value_or(0);
		p.cpu_percent = double_at(r, 1);
		p.memory_bytes = r.as_int(2);
		p.net_in_bps = hour_rate(p.at, now, r.as_int(3));
		p.net_out_bps = hour_rate(p.at, now, r.as_int(4));
		p.disk_free_bytes = r.as_int(5);
		out.push_back(p);
	}
	return out;
}

// What a program's tasks used in one minute or hour, added up.
struct Sum {
	double cpu_weighted = 0; // each task's percent times its CPU limit
	double cpu_limit = 0; // of the tasks with a percent
	double cpu_mean = 0; // an hour's: the tasks' means added (over `cpu_tasks`)
	int cpu_tasks = 0;
	std::optional<int64_t> memory_bytes;
	std::optional<double> in_bps;
	std::optional<double> out_bps;
};

void add(std::optional<int64_t> &to, std::optional<int64_t> v) {
	if (v)
		to = to.value_or(0) + *v;
}
void add(std::optional<double> &to, std::optional<double> v) {
	if (v)
		to = to.value_or(0) + *v;
}

// A program's tasks (or its one `task`) minute by minute since `since`, added up:
// the CPU in percent of the CPUs of the tasks that report it.
std::vector<Point> program_minutes(
		db::Database &db, const std::string &program, const std::string &task, int64_t since) {
	std::map<int64_t, Sum> by_minute;
	std::string last_task;
	CounterRate in;
	CounterRate sent;
	for (const db::Row &r :
			db.query("SELECT task_id, CAST(extract(epoch FROM at) AS bigint), cpu_percent, "
					 "cpu_limit, memory_used_bytes, net_in_bytes, net_out_bytes FROM "
					 "program_samples WHERE program = $1 AND ($2 = '' OR task_id = $2) AND at >= "
					 "to_timestamp($3::double precision) ORDER BY task_id, at;",
					{ BindValue(program), BindValue(task),
							BindValue(since - UsageRoutes::kGapSeconds) })) {
		const std::string t = r.as_text(0).value_or("");
		if (t != last_task) {
			in = CounterRate();
			sent = CounterRate();
			last_task = t;
		}
		const int64_t at = r.as_int(1).value_or(0);
		const std::optional<double> in_bps = in.next(at, r.as_int(5));
		const std::optional<double> out_bps = sent.next(at, r.as_int(6));
		if (at < since)
			continue;
		Sum &s = by_minute[at];
		const std::optional<double> cpu = double_at(r, 2);
		const std::optional<double> limit = double_at(r, 3);
		if (cpu && limit && *limit > 0) {
			s.cpu_weighted += *cpu * *limit;
			s.cpu_limit += *limit;
		}
		add(s.memory_bytes, r.as_int(4));
		add(s.in_bps, in_bps);
		add(s.out_bps, out_bps);
	}
	std::vector<Point> out;
	for (const auto &[at, s] : by_minute) {
		Point p;
		p.at = at;
		if (s.cpu_limit > 0)
			p.cpu_percent = s.cpu_weighted / s.cpu_limit;
		p.memory_bytes = s.memory_bytes;
		p.net_in_bps = s.in_bps;
		p.net_out_bps = s.out_bps;
		out.push_back(p);
	}
	return out;
}

std::vector<Point> program_hours(db::Database &db, const std::string &program,
		const std::string &task, int64_t since, int64_t now) {
	std::map<int64_t, Sum> by_hour;
	for (const db::Row &r :
			db.query("SELECT CAST(extract(epoch FROM hour) AS bigint), cpu_avg, "
					 "memory_peak_bytes, net_in_bytes, net_out_bytes FROM program_hourly WHERE "
					 "program = $1 AND ($2 = '' OR task_id = $2) AND hour >= "
					 "to_timestamp($3::double precision) ORDER BY hour;",
					{ BindValue(program), BindValue(task), BindValue(since) })) {
		const int64_t hour = r.as_int(0).value_or(0);
		Sum &s = by_hour[hour];
		if (const std::optional<double> cpu = double_at(r, 1)) {
			s.cpu_mean += *cpu;
			++s.cpu_tasks;
		}
		add(s.memory_bytes, r.as_int(2));
		add(s.in_bps, hour_rate(hour, now, r.as_int(3)));
		add(s.out_bps, hour_rate(hour, now, r.as_int(4)));
	}
	std::vector<Point> out;
	for (const auto &[hour, s] : by_hour) {
		Point p;
		p.at = hour;
		if (s.cpu_tasks > 0)
			p.cpu_percent = s.cpu_mean / s.cpu_tasks;
		p.memory_bytes = s.memory_bytes;
		p.net_in_bps = s.in_bps;
		p.net_out_bps = s.out_bps;
		out.push_back(p);
	}
	return out;
}

// A task's newest minute and its rates since the one before.
struct TaskNow {
	std::string program;
	std::string id;
	int64_t at = 0;
	std::optional<double> cpu_percent;
	std::optional<double> cpu_limit;
	std::optional<int64_t> memory_bytes;
	std::optional<int64_t> memory_limit_bytes;
	std::optional<double> in_bps;
	std::optional<double> out_bps;
};

// Every task's newest minute in the last 24 h (its two newest, for the rates),
// by program, then task.
std::vector<TaskNow> tasks_now(db::Database &db, const std::string &program, int64_t now) {
	std::vector<TaskNow> out;
	std::string last;
	CounterRate in;
	CounterRate sent;
	for (const db::Row &r :
			db.query("SELECT program, task_id, CAST(extract(epoch FROM at) AS bigint), "
					 "cpu_percent, cpu_limit, memory_used_bytes, memory_limit_bytes, "
					 "net_in_bytes, net_out_bytes FROM (SELECT *, row_number() OVER (PARTITION BY "
					 "program, task_id ORDER BY at DESC) AS n FROM program_samples WHERE at >= "
					 "to_timestamp($1::double precision) AND ($2 = '' OR program = $2)) s WHERE "
					 "n <= 2 ORDER BY program, task_id, at;",
					{ BindValue(now - 24 * 60 * 60), BindValue(program) })) {
		const std::string key = r.as_text(0).value_or("") + "/" + r.as_text(1).value_or("");
		if (key != last) {
			in = CounterRate();
			sent = CounterRate();
			last = key;
			out.emplace_back();
		}
		TaskNow &t = out.back();
		t.program = r.as_text(0).value_or("");
		t.id = r.as_text(1).value_or("");
		t.at = r.as_int(2).value_or(0);
		t.cpu_percent = double_at(r, 3);
		t.cpu_limit = double_at(r, 4);
		t.memory_bytes = r.as_int(5);
		t.memory_limit_bytes = r.as_int(6);
		t.in_bps = in.next(t.at, r.as_int(7));
		t.out_bps = sent.next(t.at, r.as_int(8));
	}
	return out;
}

// What reporting tasks use now, added up: the CPU in percent of their CPUs.
struct Now {
	std::optional<double> cpu_percent;
	std::optional<double> cpu_limit;
	std::optional<int64_t> memory_bytes;
	std::optional<int64_t> memory_limit_bytes;
	std::optional<double> in_bps;
	std::optional<double> out_bps;
	int64_t at = 0;
};

Now sum_now(const std::vector<const TaskNow *> &tasks) {
	Now n;
	double weighted = 0;
	double limit = 0;
	for (const TaskNow *t : tasks) {
		if (t->cpu_percent && t->cpu_limit && *t->cpu_limit > 0) {
			weighted += *t->cpu_percent * *t->cpu_limit;
			limit += *t->cpu_limit;
		}
		add(n.cpu_limit, t->cpu_limit);
		add(n.memory_bytes, t->memory_bytes);
		add(n.memory_limit_bytes, t->memory_limit_bytes);
		add(n.in_bps, t->in_bps);
		add(n.out_bps, t->out_bps);
		n.at = std::max(n.at, t->at);
	}
	if (limit > 0)
		n.cpu_percent = weighted / limit;
	return n;
}

void put_now(crow::json::wvalue &out, const Now &n) {
	value_or_null(out, "cpu_percent", n.cpu_percent);
	value_or_null(out, "cpu_limit", n.cpu_limit);
	value_or_null(out, "memory_bytes", n.memory_bytes);
	value_or_null(out, "memory_limit_bytes", n.memory_limit_bytes);
	value_or_null(out, "net_in_bps", n.in_bps);
	value_or_null(out, "net_out_bps", n.out_bps);
}

std::string short_arn(const std::string &arn) {
	const size_t slash = arn.rfind('/');
	return slash == std::string::npos ? arn : arn.substr(slash + 1);
}

bool is_task_char(char c) {
	return is_alnum(c) || c == '.' || c == '_' || c == '-';
}

std::optional<int64_t> id_param(const std::string &v) {
	if (v.empty() || v.size() > 18 || !strutil::all_digits(v))
		return std::nullopt;
	const auto n = strutil::parse_llong(v);
	if (!n || *n < 1)
		return std::nullopt;
	return static_cast<int64_t>(*n);
}

} // namespace

std::optional<double> CounterRate::next(int64_t at, std::optional<int64_t> bytes) {
	std::optional<double> out;
	if (bytes && prev_ && at > prev_at_ && at - prev_at_ <= kRateGapSeconds && *bytes >= *prev_)
		out = static_cast<double>(*bytes - *prev_) / static_cast<double>(at - prev_at_);
	prev_ = bytes;
	prev_at_ = at;
	return out;
}

NetRates machine_net_rates(db::Database &db, int64_t machine_id) {
	const auto rows = db.query("SELECT CAST(extract(epoch FROM at) AS bigint), net_in_bytes, "
							   "net_out_bytes FROM fleet_machine_samples WHERE machine_id = $1 "
							   "ORDER BY at DESC LIMIT 2;",
			{ BindValue(machine_id) });
	NetRates out;
	CounterRate in;
	CounterRate sent;
	for (auto r = rows.rbegin(); r != rows.rend(); ++r) {
		const int64_t at = r->as_int(0).value_or(0);
		out.in_bps = in.next(at, r->as_int(1));
		out.out_bps = sent.next(at, r->as_int(2));
	}
	return out;
}

UsageRoutes::UsageRoutes(WebAccess &access, db::ConnectionPool &pool, RedisPool &redis) :
		access_(access),
		pool_(pool),
		reads_(redis, kReadsPerAdmin),
		programs_(InfraConfig().programs) {}

void UsageRoutes::configure(
		InfraStatus *status, MetricsStore *metrics, int slot_memory_mb, double slot_cpus) {
	status_ = status;
	metrics_ = metrics;
	slot_memory_bytes_ =
			static_cast<int64_t>(slot_memory_mb > 0 ? slot_memory_mb : 512) * 1024 * 1024;
	slot_cpus_ = slot_cpus > 0 ? slot_cpus : 1.0;
}

void UsageRoutes::register_routes(WebApp &app) {
	CROW_ROUTE(app, "/api/admin/usage").methods("GET"_method)([this](const crow::request &req) {
		return usage(req);
	});
	CROW_ROUTE(app, "/api/admin/programs").methods("GET"_method)([this](const crow::request &req) {
		return programs(req);
	});
}

std::shared_ptr<const CloudSnapshot> UsageRoutes::cloud(db::Database &db) {
	if (status_ == nullptr)
		return nullptr;
	std::vector<std::string> instance_ids;
	for (const hosted::FleetMachineRow &m :
			hosted::list_machines(db, InfraRoutes::kEndedMachinesShown))
		if (!m.cloud_id.empty())
			instance_ids.push_back(m.cloud_id);
	return status_->snapshot(instance_ids);
}

// GET /api/admin/usage — for a server, a machine or a program: {"subject", "id",
// "task" (a program's one task; null for all), "range", "step_seconds",
// "limit" ({"cpu" (CPUs; null where none is known), "memory_bytes"}),
// "series" (oldest first, each {"at", "cpu_percent" (of the limit's CPUs),
// "memory_bytes", "net_in_bps", "net_out_bps"}, a server's with "tick_p99_ms"
// and "late_ticks", a machine's with "disk_free_bytes"; null where nothing was
// read), "now" (the newest minute when at most kLiveSeconds old, else null;
// a machine's with "disk_total_bytes")}. For an AWS resource (id rds, cache or
// nlb): {"subject", "id", "range", "available", "unavailable" (why not, the
// local stack's), "error" (AWS's words), "period_seconds", "statistics"
// ({"<metric>": "Average"|"Sum"|"Minimum"|"Maximum"}), "metrics"
// ({"<metric>": [{"at", "value"}]})}. 400 invalid_field; 404 not_found;
// 429 rate_limited. no-store.
crow::response UsageRoutes::usage(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	const std::string who =
			access.user ? "usage|" + std::to_string(access.user->id) : "usage|token";
	if (const int64_t wait = reads_.take(who, RateLimiter::Clock::now()))
		return too_many_requests(wait);

	const auto param = [&req](const char *name) {
		const char *v = req.url_params.get(name);
		return v != nullptr ? std::string(v) : std::string();
	};
	const std::string subject = param("subject");
	const std::string id = param("id");
	const std::string range = param("range").empty() ? std::string("48h") : param("range");
	if (subject != "server" && subject != "machine" && subject != "program" && subject != "infra")
		return json_error(400, "invalid_field", "subject is server, machine, program or infra");
	if (range != "48h" && range != "400d")
		return json_error(400, "invalid_field", "range is 48h or 400d");
	const bool hourly = range == "400d";

	crow::json::wvalue out;
	out["subject"] = subject;
	out["range"] = range;
	if (subject == "infra") {
		if (id != "rds" && id != "cache" && id != "nlb")
			return json_error(404, "not_found", "the infrastructure is rds, cache or nlb");
		out["id"] = id;
		std::string node;
		if (id == "cache" && metrics_ != nullptr && metrics_->unavailable().empty()) {
			try {
				auto db_conn = pool_.acquire();
				if (const auto snap = cloud(*db_conn); snap && snap->cache)
					for (const CacheNode &n : snap->cache->nodes)
						if (n.role == "primary")
							node = n.id;
			} catch (const db::Error &e) {
				std::fprintf(stderr, "[http] GET /api/admin/usage failed: %s\n", e.what());
				return json_error(500, "db_error");
			}
		}
		ResourceMetrics m;
		if (metrics_ != nullptr)
			metrics_->read(id, hourly, node, m);
		else
			m.unavailable = "AWS is not read here: this web process has no metrics reader";
		out["available"] = m.unavailable.empty();
		text_or_null(out, "unavailable", m.unavailable);
		text_or_null(out, "error", m.error);
		out["period_seconds"] = hourly ? int64_t{ 3600 } : int64_t{ 60 };
		crow::json::wvalue stats = crow::json::wvalue::object();
		crow::json::wvalue metrics = crow::json::wvalue::object();
		for (const MetricSeries &s : m.metrics) {
			stats[s.name] = s.statistic;
			std::vector<crow::json::wvalue> points;
			points.reserve(s.points.size());
			for (const MetricPoint &p : s.points) {
				crow::json::wvalue j;
				j["at"] = hosted::iso_utc(p.at);
				j["value"] = p.value;
				points.push_back(std::move(j));
			}
			metrics[s.name] = std::move(points);
		}
		out["statistics"] = std::move(stats);
		out["metrics"] = std::move(metrics);
		return no_store(json_reply(200, out));
	}

	out["task"] = nullptr;
	try {
		auto db_conn = pool_.acquire();
		db::Database &db = *db_conn;
		db::ReadSnapshot snapshot(db);
		const int64_t now = db.query("SELECT CAST(extract(epoch FROM now()) AS bigint);")
									.front()
									.as_int(0)
									.value_or(0);
		const int64_t since = now - (hourly ? 400LL * 24 * 60 * 60 : 48LL * 60 * 60);
		const int64_t live_since = now - kLiveSeconds;
		std::vector<Point> series;
		std::vector<Point> recent; // the minutes "now" comes from
		crow::json::wvalue limit;
		const bool server = subject == "server";
		const bool machine = subject == "machine";
		if (server) {
			const std::optional<int64_t> sid = id_param(id);
			if (!sid ||
					db.query("SELECT 1 FROM hosted_servers WHERE id = $1;", { BindValue(*sid) })
							.empty())
				return json_error(404, "not_found", "no hosted server has this id");
			out["id"] = *sid;
			series = hourly ? server_hours(db, *sid, since, now) : server_minutes(db, *sid, since);
			recent = hourly ? server_minutes(db, *sid, live_since) : series;
			limit["cpu"] = slot_cpus_;
			limit["memory_bytes"] = slot_memory_bytes_;
		} else if (machine) {
			const std::optional<int64_t> mid = id_param(id);
			const auto rows = mid ? db.query("SELECT memory_total_bytes, disk_total_bytes FROM "
											 "fleet_machines WHERE id = $1;",
											{ BindValue(*mid) })
								  : std::vector<db::Row>{};
			if (rows.empty())
				return json_error(404, "not_found", "no fleet machine has this id");
			out["id"] = *mid;
			series =
					hourly ? machine_hours(db, *mid, since, now) : machine_minutes(db, *mid, since);
			recent = hourly ? machine_minutes(db, *mid, live_since) : series;
			limit["cpu"] = nullptr;
			value_or_null(limit, "memory_bytes", rows.front().as_int(0));
			value_or_null(out, "disk_total_bytes", rows.front().as_int(1));
		} else {
			if (std::find(programs_.begin(), programs_.end(), id) == programs_.end())
				return json_error(404, "not_found", "the service runs no program of this name");
			out["id"] = id;
			const std::string task = param("task");
			if (!task.empty()) {
				if (task.size() > 64 || !all_of(task, is_task_char) ||
						db.query("SELECT 1 FROM program_samples WHERE program = $1 AND task_id = "
								 "$2 "
								 "UNION ALL SELECT 1 FROM program_hourly WHERE program = $1 AND "
								 "task_id = $2 LIMIT 1;",
								  { BindValue(id), BindValue(task) })
								.empty())
					return json_error(404, "not_found", "this program has run no such task");
				out["task"] = task;
			}
			series = hourly ? program_hours(db, id, task, since, now)
							: program_minutes(db, id, task, since);
			recent = hourly ? program_minutes(db, id, task, live_since) : series;
			// The limits: the reporting tasks' together (the newest one's when none
			// reports now).
			std::vector<TaskNow> tasks = tasks_now(db, id, now);
			std::vector<const TaskNow *> live;
			const TaskNow *newest = nullptr;
			for (const TaskNow &t : tasks) {
				if (!task.empty() && t.id != task)
					continue;
				if (t.at >= live_since)
					live.push_back(&t);
				if (newest == nullptr || t.at > newest->at)
					newest = &t;
			}
			if (live.empty() && newest != nullptr)
				live.push_back(newest);
			const Now n = sum_now(live);
			value_or_null(limit, "cpu", n.cpu_limit);
			value_or_null(limit, "memory_bytes", n.memory_limit_bytes);
		}
		out["limit"] = std::move(limit);
		out["step_seconds"] = hourly ? int64_t{ 3600 } : int64_t{ 60 };
		std::vector<crow::json::wvalue> points;
		points.reserve(series.size());
		for (const Point &p : series)
			points.push_back(point_json(p, server, machine));
		out["series"] = std::move(points);
		if (!recent.empty() && recent.back().at >= live_since) {
			crow::json::wvalue now_json = point_json(recent.back(), server, machine);
			out["now"] = std::move(now_json);
		} else {
			out["now"] = nullptr;
		}
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/usage failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	return no_store(json_reply(200, out));
}

// GET /api/admin/programs — {"at", "cloud" ({"configured", "unavailable",
// "error"}: whether ECS was read), "programs": each program the service runs,
// in its order, {"name", "running" and "desired" (ECS's counts; with no cloud,
// the tasks reporting now and null), "release" (the revision its service runs,
// family:revision; null without the cloud), "on_release" (whether it is the
// release's; null when unknown), "cpu_percent" (of "cpu_limit", the reporting
// tasks' CPUs together), "memory_bytes", "memory_limit_bytes", "net_in_bps",
// "net_out_bps" (each null when no task reports), "restarts_24h" (its tasks
// that reported in the last 24 h and no longer do: a crash's, a deploy's
// replaced ones, a scale-in's), "tasks" (the reporting ones and the ones ECS
// runs: {"id", "reporting", "sampled_at", the usage fields, and ECS's
// "last_status", "health", "started_at", "task_definition", each null where
// unknown})}. no-store.
crow::response UsageRoutes::programs(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	const std::string who =
			access.user ? "usage|" + std::to_string(access.user->id) : "usage|token";
	if (const int64_t wait = reads_.take(who, RateLimiter::Clock::now()))
		return too_many_requests(wait);

	crow::json::wvalue out;
	std::vector<TaskNow> tasks;
	std::map<std::string, int64_t> gone;
	int64_t now = 0;
	std::shared_ptr<const CloudSnapshot> snap;
	try {
		auto db_conn = pool_.acquire();
		db::Database &db = *db_conn;
		now = db.query("SELECT CAST(extract(epoch FROM now()) AS bigint);")
					  .front()
					  .as_int(0)
					  .value_or(0);
		tasks = tasks_now(db, "", now);
		for (const db::Row &r :
				db.query("SELECT program, COUNT(*) FILTER (WHERE last_at < to_timestamp($1::double "
						 "precision)) FROM (SELECT program, task_id, MAX(at) AS last_at FROM "
						 "program_samples WHERE at >= to_timestamp($2::double precision) GROUP BY "
						 "program, task_id) t GROUP BY program;",
						{ BindValue(now - kLiveSeconds), BindValue(now - 24 * 60 * 60) }))
			gone[r.as_text(0).value_or("")] = r.as_int(1).value_or(0);
		snap = cloud(db);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/programs failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	const bool read_cloud = snap && snap->unavailable.empty() && snap->error.empty();

	out["at"] = hosted::iso_utc(now);
	crow::json::wvalue cloud_json;
	cloud_json["configured"] = snap != nullptr && snap->unavailable.empty();
	text_or_null(cloud_json, "unavailable",
			snap == nullptr ? std::string("AWS is not read here: this web process has no "
										  "infrastructure reader")
							: snap->unavailable);
	text_or_null(cloud_json, "error",
			snap == nullptr ? std::string()
							: (!snap->error.empty() ? snap->error : snap->services_error));
	out["cloud"] = std::move(cloud_json);

	std::vector<crow::json::wvalue> rows;
	for (const std::string &name : programs_) {
		crow::json::wvalue p;
		p["name"] = name;
		std::vector<const TaskNow *> live;
		for (const TaskNow &t : tasks)
			if (t.program == name && t.at >= now - kLiveSeconds)
				live.push_back(&t);
		const EcsService *service = nullptr;
		if (read_cloud)
			for (const EcsService &s : snap->services)
				if (s.name == name)
					service = &s;
		if (service != nullptr) {
			p["running"] = service->running;
			p["desired"] = service->desired;
			p["release"] = short_arn(service->task_definition_arn);
			p["on_release"] = nullptr;
			if (snap->release) {
				const auto &defs = snap->release->task_definitions;
				if (const auto it = defs.find(name); it != defs.end())
					p["on_release"] = it->second == service->task_definition_arn;
			}
		} else {
			p["running"] = static_cast<int64_t>(live.size());
			p["desired"] = nullptr;
			p["release"] = nullptr;
			p["on_release"] = nullptr;
		}
		put_now(p, sum_now(live));
		const auto g = gone.find(name);
		p["restarts_24h"] = g == gone.end() ? int64_t{ 0 } : g->second;

		// The tasks: the reporting ones, then the ones ECS runs that do not report.
		std::vector<crow::json::wvalue> task_rows;
		std::vector<std::string> listed;
		const auto ecs_task = [&](const std::string &id) -> const EcsTask * {
			if (!read_cloud)
				return nullptr;
			for (const EcsTask &t : snap->tasks)
				if (t.id == id && t.program == name)
					return &t;
			return nullptr;
		};
		const auto put_ecs = [](crow::json::wvalue &j, const EcsTask *t) {
			text_or_null(j, "last_status", t ? t->last_status : std::string());
			text_or_null(j, "health", t ? t->health : std::string());
			text_or_null(j, "started_at", t ? t->started_at : std::string());
			text_or_null(
					j, "task_definition", t ? short_arn(t->task_definition_arn) : std::string());
		};
		for (const TaskNow *t : live) {
			crow::json::wvalue j;
			j["id"] = t->id;
			j["reporting"] = true;
			j["sampled_at"] = hosted::iso_utc(t->at);
			put_now(j, sum_now({ t }));
			put_ecs(j, ecs_task(t->id));
			task_rows.push_back(std::move(j));
			listed.push_back(t->id);
		}
		if (read_cloud)
			for (const EcsTask &t : snap->tasks) {
				if (t.program != name || t.desired_status == "STOPPED" ||
						std::find(listed.begin(), listed.end(), t.id) != listed.end())
					continue;
				crow::json::wvalue j;
				j["id"] = t.id;
				j["reporting"] = false;
				j["sampled_at"] = nullptr;
				put_now(j, Now());
				put_ecs(j, &t);
				task_rows.push_back(std::move(j));
			}
		p["tasks"] = std::move(task_rows);
		rows.push_back(std::move(p));
	}
	out["programs"] = std::move(rows);
	return no_store(json_reply(200, out));
}

} // namespace opennova::novaworld_server::infra
