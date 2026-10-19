#include "stats_repository.h"

#include "hosted_repository.h"

#include <cstdio>
#include <map>
#include <utility>
#include <variant>

namespace opennova::novaworld_server::hosted {

namespace {

using db::BindValue;

constexpr const char *kSampleColumns =
		"server_id, CAST(extract(epoch FROM at) AS bigint), state, players, max_players, mission, scene, "
		"link_up, cpu_percent, memory_bytes, egress_bytes, mission_started, net_in_bytes, net_out_bytes, "
		"tick_p50_ms, tick_p99_ms, tick_max_ms, late_ticks";

// A counter's growth between consecutive rows, `cur` and the one before it
// `prev` (SQL over a LAG): none for the first, the whole count for one below the
// last (a counter that started over).
std::string counter_delta(const char *cur, const char *prev) {
	return std::string("CASE WHEN ") + prev + " IS NULL OR " + cur + " IS NULL THEN 0 WHEN " + cur +
			" >= " + prev + " THEN " + cur + " - " + prev + " ELSE " + cur + " END";
}

BindValue text_or_null(const std::string &text) {
	return text.empty() ? BindValue(std::monostate{}) : BindValue(text);
}

BindValue int_or_null(const std::optional<int64_t> &value) {
	return value ? BindValue(*value) : BindValue(std::monostate{});
}

BindValue double_or_null(const std::optional<double> &value) {
	return value ? BindValue(*value) : BindValue(std::monostate{});
}

std::optional<double> double_at(const db::Row &row, size_t i) {
	if (i >= row.values.size()) return std::nullopt;
	if (const auto *d = std::get_if<double>(&row.values[i])) return *d;
	if (const auto *n = std::get_if<int64_t>(&row.values[i])) return static_cast<double>(*n);
	return std::nullopt;
}

int64_t int_at(const db::Row &row, size_t i) {
	if (const auto v = row.as_int(i)) return *v;
	if (const auto d = double_at(row, i)) return static_cast<int64_t>(*d);
	return 0;
}

int64_t floor_to(int64_t value, int64_t step) {
	const int64_t q = value / step;
	return (value % step != 0 && value < 0 ? q - 1 : q) * step;
}

// The scope as a test of a `server_id` column: its SQL, binding its id (if
// any) as the next parameter of `binds`.
std::string scope_sql(const StatsScope &scope, std::vector<BindValue> &binds) {
	if (scope.server_id) {
		binds.push_back(BindValue(*scope.server_id));
		return "server_id = $" + std::to_string(binds.size());
	}
	if (scope.owner_user_id) {
		binds.push_back(BindValue(*scope.owner_user_id));
		return "server_id IN (SELECT id FROM hosted_servers WHERE owner_user_id = $" + std::to_string(binds.size()) +
				")";
	}
	return "TRUE";
}

// The window's binds ($1 the start, $2 the end) and its test of a `column`.
std::string window_sql(const char *column) {
	return std::string(column) + " >= to_timestamp($1::double precision) AND " + column +
			" < to_timestamp($2::double precision)";
}

ServerSample row_to_sample(const db::Row &r) {
	ServerSample s;
	s.server_id = int_at(r, 0);
	s.at = int_at(r, 1);
	s.state = r.as_text(2).value_or("");
	s.players = r.as_int(3);
	s.max_players = r.as_int(4);
	s.mission = r.as_text(5).value_or("");
	s.scene = r.as_text(6).value_or("");
	s.link_up = r.as_int(7).value_or(0) != 0;
	s.cpu_percent = double_at(r, 8);
	s.memory_bytes = r.as_int(9);
	s.egress_bytes = int_at(r, 10);
	s.mission_started = r.as_int(11).value_or(0) != 0;
	s.net_in_bytes = r.as_int(12);
	s.net_out_bytes = r.as_int(13);
	s.tick_p50_ms = double_at(r, 14);
	s.tick_p99_ms = double_at(r, 15);
	s.tick_max_ms = double_at(r, 16);
	s.late_ticks = r.as_int(17);
	return s;
}

} // namespace

void insert_sample(db::Database &db, const ServerSample &s) {
	db.exec(std::string("INSERT INTO hosted_server_samples (server_id, at, state, players, max_players, mission, "
						"scene, link_up, cpu_percent, memory_bytes, egress_bytes, mission_started, net_in_bytes, "
						"net_out_bytes, tick_p50_ms, tick_p99_ms, tick_max_ms, late_ticks) "
						"VALUES ($1, to_timestamp($2::double precision), $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, "
						"$13, $14, $15, $16, $17, $18) "
						"ON CONFLICT (server_id, at) DO NOTHING;"),
			{ BindValue(s.server_id), BindValue(s.at), BindValue(s.state), int_or_null(s.players),
					int_or_null(s.max_players), text_or_null(s.mission), text_or_null(s.scene),
					BindValue(db::Bool{ s.link_up }), double_or_null(s.cpu_percent), int_or_null(s.memory_bytes),
					BindValue(s.egress_bytes < 0 ? 0 : s.egress_bytes), BindValue(db::Bool{ s.mission_started }),
					int_or_null(s.net_in_bytes), int_or_null(s.net_out_bytes), double_or_null(s.tick_p50_ms),
					double_or_null(s.tick_p99_ms), double_or_null(s.tick_max_ms), int_or_null(s.late_ticks) });
}

void add_machine_egress(db::Database &db, int64_t machine_id, int64_t at, int64_t bytes) {
	if (bytes <= 0) return;
	db.exec("INSERT INTO fleet_machine_egress (machine_id, month, egress_bytes) "
			"VALUES ($1, CAST(date_trunc('month', to_timestamp($2::double precision) AT TIME ZONE 'UTC') AS date), $3) "
			"ON CONFLICT (machine_id, month) DO UPDATE SET "
			"egress_bytes = fleet_machine_egress.egress_bytes + EXCLUDED.egress_bytes;",
			{ BindValue(machine_id), BindValue(at), BindValue(bytes) });
}

void rollup_hour(db::Database &db, int64_t server_id, int64_t at, int64_t unique_players) {
	// The bytes in: the counters' differences, each minute's from the one
	// before it (the hour's first from the hour before's last).
	const std::string bytes_in = "(SELECT COALESCE(SUM(" + counter_delta("w.cur", "w.prev") +
			"), 0) FROM (SELECT at, net_in_bytes AS cur, LAG(net_in_bytes) OVER (ORDER BY at) AS prev "
			"FROM hosted_server_samples WHERE server_id = $1 AND net_in_bytes IS NOT NULL "
			"AND at >= h.hour - interval '1 hour' AND at < h.hour + interval '1 hour') w "
			"WHERE w.at >= h.hour)";
	db.exec("INSERT INTO hosted_server_hourly (server_id, hour, live_minutes, running_minutes, player_minutes, "
			"peak_players, max_players, unique_players, missions, egress_bytes, avg_cpu, peak_memory_bytes, "
			"net_in_bytes, net_out_bytes, tick_p99_max_ms, late_ticks) "
			"SELECT $1, h.hour, COUNT(s.at), COUNT(s.at) FILTER (WHERE s.state = 'running'), "
			"COALESCE(SUM(s.players), 0), COALESCE(MAX(s.players), 0), COALESCE(MAX(s.max_players), 0), $3, "
			"COUNT(s.at) FILTER (WHERE s.mission_started), COALESCE(SUM(s.egress_bytes), 0), AVG(s.cpu_percent), "
			"MAX(s.memory_bytes), " +
					bytes_in +
					", COALESCE(SUM(s.egress_bytes), 0), MAX(s.tick_p99_ms), COALESCE(SUM(s.late_ticks), 0) "
					"FROM (SELECT date_trunc('hour', to_timestamp($2::double precision)) AS hour) h "
					"LEFT JOIN hosted_server_samples s ON s.server_id = $1 AND s.at >= h.hour "
					"AND s.at < h.hour + interval '1 hour' GROUP BY h.hour "
					"ON CONFLICT (server_id, hour) DO UPDATE SET live_minutes = EXCLUDED.live_minutes, "
					"running_minutes = EXCLUDED.running_minutes, player_minutes = EXCLUDED.player_minutes, "
					"peak_players = EXCLUDED.peak_players, max_players = EXCLUDED.max_players, "
					"unique_players = GREATEST(hosted_server_hourly.unique_players, EXCLUDED.unique_players), "
					"missions = EXCLUDED.missions, egress_bytes = EXCLUDED.egress_bytes, avg_cpu = EXCLUDED.avg_cpu, "
					"peak_memory_bytes = EXCLUDED.peak_memory_bytes, net_in_bytes = EXCLUDED.net_in_bytes, "
					"net_out_bytes = EXCLUDED.net_out_bytes, tick_p99_max_ms = EXCLUDED.tick_p99_max_ms, "
					"late_ticks = EXCLUDED.late_ticks;",
			{ BindValue(server_id), BindValue(at), BindValue(unique_players) });
}

size_t sample_machines(db::Database &db, int64_t minute) {
	db.exec(std::string("INSERT INTO fleet_machine_samples (machine_id, at, cpu_percent, memory_used_bytes, "
						"memory_total_bytes, disk_free_bytes, net_in_bytes, net_out_bytes, servers) "
						"SELECT m.id, to_timestamp($1::double precision), m.cpu_percent, m.memory_used_bytes, "
						"m.memory_total_bytes, m.disk_free_bytes, m.net_in_bytes, m.net_out_bytes, "
						"(SELECT COUNT(*) FROM hosted_servers s WHERE s.machine_ref = m.id AND s.slot IS NOT NULL "
						"AND s.") +
					kLiveServerSql +
					") FROM fleet_machines m WHERE m.state NOT IN ('ended', 'failed') AND m.stats_at >= now() - "
					"make_interval(secs => $2) ON CONFLICT (machine_id, at) DO NOTHING;",
			{ BindValue(minute), BindValue(static_cast<double>(kMachineStatsFreshSeconds)) });
	return static_cast<size_t>(db.changes());
}

void rollup_machine_hours(db::Database &db, int64_t now) {
	// Every hour whose minutes are all still kept, from the first after
	// kSampleKeepSeconds back to `now`'s own (as program/program_usage.h
	// rollup_usage_hours does), so a pass that never ran leaves no hour
	// unwritten; each minute's counters taken from the minute before it (the
	// window opens an hour earlier for the first's).
	const int64_t kept = now - kSampleKeepSeconds;
	const int64_t first = kept - ((kept % 3600) + 3600) % 3600 + 3600;
	db.exec("INSERT INTO fleet_machine_hourly (machine_id, hour, cpu_avg, cpu_max, memory_peak_bytes, "
			"disk_free_min_bytes, net_in_bytes, net_out_bytes) "
			"SELECT machine_id, date_trunc('hour', at), AVG(cpu_percent), MAX(cpu_percent), MAX(memory_used_bytes), "
			"MIN(disk_free_bytes), COALESCE(SUM(" +
					counter_delta("net_in_bytes", "prev_in") + "), 0), COALESCE(SUM(" +
					counter_delta("net_out_bytes", "prev_out") +
					"), 0) FROM (SELECT *, LAG(net_in_bytes) OVER w AS prev_in, LAG(net_out_bytes) OVER w AS "
					"prev_out FROM fleet_machine_samples WHERE at >= to_timestamp($1::double precision) - "
					"interval '1 hour' WINDOW w AS (PARTITION BY machine_id ORDER BY at)) minutes "
					"WHERE at >= to_timestamp($1::double precision) "
					"GROUP BY machine_id, date_trunc('hour', at) "
					"ON CONFLICT (machine_id, hour) DO UPDATE SET cpu_avg = EXCLUDED.cpu_avg, "
					"cpu_max = EXCLUDED.cpu_max, memory_peak_bytes = EXCLUDED.memory_peak_bytes, "
					"disk_free_min_bytes = EXCLUDED.disk_free_min_bytes, net_in_bytes = EXCLUDED.net_in_bytes, "
					"net_out_bytes = EXCLUDED.net_out_bytes;",
			{ BindValue(first) });
}

void prune_stats(db::Database &db, int64_t now) {
	db.exec("DELETE FROM hosted_server_samples WHERE at < to_timestamp($1::double precision);",
			{ BindValue(now - kSampleKeepSeconds) });
	db.exec("DELETE FROM hosted_server_hourly WHERE hour < to_timestamp($1::double precision);",
			{ BindValue(now - kHourlyKeepSeconds) });
	db.exec("DELETE FROM fleet_machine_samples WHERE at < to_timestamp($1::double precision);",
			{ BindValue(now - kSampleKeepSeconds) });
	db.exec("DELETE FROM fleet_machine_hourly WHERE hour < to_timestamp($1::double precision);",
			{ BindValue(now - kHourlyKeepSeconds) });
	db.exec("DELETE FROM fleet_machine_egress WHERE month < CAST(date_trunc('month', "
			"to_timestamp($1::double precision) AT TIME ZONE 'UTC') - interval '13 months' AS date);",
			{ BindValue(now) });
}

std::optional<StatsRange> parse_stats_range(std::string_view text) {
	struct Known {
		const char *name;
		int64_t seconds;
		int64_t step;
		bool hourly;
	};
	static constexpr Known kRanges[] = {
		{ "1h", 60 * 60, 60, false },
		{ "24h", 24 * 60 * 60, 5 * 60, false },
		{ "7d", 7 * 24 * 60 * 60, 60 * 60, true },
		{ "30d", 30 * 24 * 60 * 60, 4 * 60 * 60, true },
	};
	for (const Known &k : kRanges) {
		if (text == k.name) return StatsRange{ k.name, k.seconds, k.step, k.hourly };
	}
	return std::nullopt;
}

int64_t stats_range_end(const StatsRange &range, int64_t now) {
	return floor_to(now, range.step_seconds) + range.step_seconds;
}

StatsSummary stats_summary(db::Database &db, const StatsScope &scope, const StatsRange &range, int64_t now) {
	db::ReadSnapshot snapshot(db);
	StatsSummary out;
	const int64_t end = stats_range_end(range, now);
	const int64_t start = end - range.seconds;
	const auto window_binds = [&] { return std::vector<BindValue>{ BindValue(start), BindValue(end) }; };
	int64_t running_minutes_together = 0;
	int64_t player_minutes = 0;
	if (!range.hourly) {
		auto binds = window_binds();
		const std::string scope_test = scope_sql(scope, binds);
		const std::string where = " WHERE " + window_sql("at") + " AND " + scope_test;
		const auto rows = db.query("SELECT COUNT(*), COUNT(*) FILTER (WHERE state = 'running'), "
								   "CAST(COALESCE(SUM(players), 0) AS bigint), "
								   "CAST(COALESCE(SUM(egress_bytes), 0) AS bigint), "
								   "COUNT(*) FILTER (WHERE mission_started), "
								   "COUNT(DISTINCT at) FILTER (WHERE state = 'running') "
								   "FROM hosted_server_samples" +
						where + ";",
				binds);
		if (!rows.empty()) {
			out.live_seconds = int_at(rows[0], 0) * 60;
			out.running_seconds = int_at(rows[0], 1) * 60;
			player_minutes = int_at(rows[0], 2);
			out.egress_bytes = int_at(rows[0], 3);
			out.missions = int_at(rows[0], 4);
			running_minutes_together = int_at(rows[0], 5);
		}
		const auto peak = db.query("SELECT CAST(COALESCE(MAX(p), 0) AS bigint) FROM (SELECT SUM(players) AS p "
								   "FROM hosted_server_samples" +
						where + " GROUP BY at) per;",
				binds);
		out.peak_players = peak.empty() ? 0 : int_at(peak[0], 0);
	} else {
		auto binds = window_binds();
		const std::string scope_test = scope_sql(scope, binds);
		const std::string where = " WHERE " + window_sql("hour") + " AND " + scope_test;
		const auto rows = db.query("SELECT CAST(COALESCE(SUM(live_minutes), 0) AS bigint), "
								   "CAST(COALESCE(SUM(running_minutes), 0) AS bigint), "
								   "CAST(COALESCE(SUM(player_minutes), 0) AS bigint), "
								   "CAST(COALESCE(SUM(egress_bytes), 0) AS bigint), "
								   "CAST(COALESCE(SUM(missions), 0) AS bigint) FROM hosted_server_hourly" +
						where + ";",
				binds);
		if (!rows.empty()) {
			out.live_seconds = int_at(rows[0], 0) * 60;
			out.running_seconds = int_at(rows[0], 1) * 60;
			player_minutes = int_at(rows[0], 2);
			out.egress_bytes = int_at(rows[0], 3);
			out.missions = int_at(rows[0], 4);
		}
		const auto per_hour = db.query("SELECT CAST(COALESCE(MAX(p), 0) AS bigint), CAST(COALESCE(SUM(r), 0) AS bigint) "
									   "FROM (SELECT SUM(peak_players) AS p, MAX(running_minutes) AS r "
									   "FROM hosted_server_hourly" +
						where + " GROUP BY hour) per;",
				binds);
		if (!per_hour.empty()) {
			out.peak_players = int_at(per_hour[0], 0);
			running_minutes_together = int_at(per_hour[0], 1);
		}
	}
	out.player_hours = static_cast<double>(player_minutes) / 60.0;
	if (running_minutes_together > 0)
		out.avg_players = static_cast<double>(player_minutes) / static_cast<double>(running_minutes_together);
	{
		// The distinct players, an hour at a time: every hour the window touches.
		std::vector<BindValue> binds = { BindValue(floor_to(start, 3600)), BindValue(end) };
		const std::string scope_test = scope_sql(scope, binds);
		const auto rows = db.query("SELECT CAST(COALESCE(SUM(unique_players), 0) AS bigint) FROM hosted_server_hourly "
								   "WHERE " +
						window_sql("hour") + " AND " + scope_test + ";",
				binds);
		out.unique_players = rows.empty() ? 0 : int_at(rows[0], 0);
	}
	{
		// The crash restarts: the starts the server's own cause ended (a crash, a
		// start or a listing that did not come in time).
		auto binds = window_binds();
		const std::string scope_test = scope_sql(scope, binds);
		const auto rows = db.query("SELECT COUNT(*) FROM hosted_server_starts WHERE " + window_sql("ended_at") +
						" AND " + scope_test + " AND cause IN ('crash', 'timeout');",
				binds);
		out.restarts = rows.empty() ? 0 : int_at(rows[0], 0);
	}
	{
		std::vector<BindValue> binds = { BindValue(now) };
		const std::string scope_test = scope_sql(scope, binds);
		const auto rows = db.query("SELECT CAST(COALESCE(SUM(egress_bytes), 0) AS bigint) FROM hosted_server_hourly "
								   "WHERE hour >= date_trunc('month', to_timestamp($1::double precision)) AND " +
						scope_test + ";",
				binds);
		out.egress_bytes_month = rows.empty() ? 0 : int_at(rows[0], 0);
	}
	return out;
}

std::vector<StatsPoint> stats_series(db::Database &db, const StatsScope &scope, const StatsRange &range, int64_t now) {
	const int64_t end = stats_range_end(range, now);
	const int64_t start = end - range.seconds;
	std::vector<BindValue> binds = { BindValue(start), BindValue(end), BindValue(range.step_seconds) };
	const std::string scope_test = scope_sql(scope, binds);
	std::string per;
	if (!range.hourly) {
		per = "SELECT server_id, CAST(floor(extract(epoch FROM at) / $3) * $3 AS bigint) AS b, "
			  "CAST(AVG(players) AS double precision) AS p, MAX(max_players) AS mp, "
			  "BOOL_OR(state = 'running') AS run, AVG(cpu_percent) AS cpu, SUM(egress_bytes) AS eg "
			  "FROM hosted_server_samples WHERE " +
				window_sql("at") + " AND " + scope_test + " GROUP BY server_id, b";
	} else {
		per = "SELECT server_id, CAST(floor(extract(epoch FROM hour) / $3) * $3 AS bigint) AS b, "
			  "CAST(SUM(player_minutes) AS double precision) / "
			  "NULLIF(CAST(SUM(live_minutes) AS double precision), 0) AS p, "
			  "NULLIF(MAX(max_players), 0) AS mp, SUM(running_minutes) > 0 AS run, AVG(avg_cpu) AS cpu, "
			  "SUM(egress_bytes) AS eg FROM hosted_server_hourly WHERE " +
				window_sql("hour") + " AND " + scope_test + " GROUP BY server_id, b";
	}
	const auto rows = db.query("SELECT b, SUM(p), CAST(SUM(mp) AS bigint), BOOL_OR(run), AVG(cpu), "
							   "CAST(SUM(eg) AS bigint) FROM (" +
					per + ") per GROUP BY b ORDER BY b;",
			binds);
	std::map<int64_t, StatsPoint> by_bucket;
	for (const db::Row &r : rows) {
		StatsPoint p;
		p.t = int_at(r, 0);
		p.players = double_at(r, 1);
		p.max_players = r.as_int(2);
		p.running = r.as_int(3).value_or(0) != 0;
		p.cpu_percent = double_at(r, 4);
		p.egress_bytes = r.as_int(5);
		by_bucket[p.t] = p;
	}
	std::vector<StatsPoint> out;
	out.reserve(static_cast<size_t>(range.seconds / range.step_seconds));
	for (int64_t t = start; t < end; t += range.step_seconds) {
		const auto it = by_bucket.find(t);
		if (it != by_bucket.end()) {
			out.push_back(it->second);
		} else {
			StatsPoint empty;
			empty.t = t;
			out.push_back(empty);
		}
	}
	return out;
}

int64_t machine_egress_month(db::Database &db, int64_t machine_id, int64_t now) {
	const auto rows = db.query("SELECT egress_bytes FROM fleet_machine_egress WHERE machine_id = $1 AND month = "
							   "CAST(date_trunc('month', to_timestamp($2::double precision) AT TIME ZONE 'UTC') AS date);",
			{ BindValue(machine_id), BindValue(now) });
	return rows.empty() ? 0 : int_at(rows[0], 0);
}

std::optional<ServerSample> latest_sample(db::Database &db, int64_t server_id, int64_t now, int64_t max_age_s) {
	const auto rows = db.query(std::string("SELECT ") + kSampleColumns +
					" FROM hosted_server_samples WHERE server_id = $1 AND at >= to_timestamp($2::double precision) "
					"ORDER BY at DESC LIMIT 1;",
			{ BindValue(server_id), BindValue(now - max_age_s) });
	if (rows.empty()) return std::nullopt;
	return row_to_sample(rows[0]);
}

std::map<int64_t, ServerSample> latest_samples_since(db::Database &db, int64_t since) {
	const std::string sql = std::string("SELECT DISTINCT ON (server_id) ") + kSampleColumns +
			" FROM hosted_server_samples WHERE at >= to_timestamp($1::double precision) "
			"ORDER BY server_id, at DESC;";
	std::map<int64_t, ServerSample> out;
	for (const db::Row &r : db.query(sql, { BindValue(since) })) {
		ServerSample s = row_to_sample(r);
		out[s.server_id] = std::move(s);
	}
	return out;
}

std::map<int64_t, SampleWindow> sample_windows_since(db::Database &db, int64_t since) {
	const char *sql =
			"SELECT server_id, COUNT(*), AVG(cpu_percent), MAX(cpu_percent), MAX(memory_bytes), "
			"CAST(COALESCE(SUM(egress_bytes), 0) AS bigint) FROM hosted_server_samples "
			"WHERE at >= to_timestamp($1::double precision) GROUP BY server_id;";
	std::map<int64_t, SampleWindow> out;
	for (const db::Row &r : db.query(sql, { BindValue(since) })) {
		SampleWindow w;
		w.minutes = int_at(r, 1);
		w.cpu_percent_mean = double_at(r, 2);
		w.cpu_percent_peak = double_at(r, 3);
		w.memory_bytes_peak = r.as_int(4);
		w.egress_bytes = int_at(r, 5);
		out[int_at(r, 0)] = w;
	}
	return out;
}

std::optional<int64_t> running_seconds_of(db::Database &db, int64_t server_id, int64_t now) {
	const auto rows = db.query("SELECT CAST(extract(epoch FROM (to_timestamp($2::double precision) - running_at)) AS bigint) "
							   "FROM hosted_servers WHERE id = $1 AND state = 'running' AND running_at IS NOT NULL;",
			{ BindValue(server_id), BindValue(now) });
	if (rows.empty()) return std::nullopt;
	const int64_t seconds = int_at(rows[0], 0);
	return seconds < 0 ? 0 : seconds;
}

std::string iso_utc(int64_t unix_seconds) {
	// The civil date of a day count (days since 1970-01-01), the proleptic
	// Gregorian calendar's.
	int64_t days = unix_seconds / 86400;
	int64_t secs = unix_seconds % 86400;
	if (secs < 0) {
		secs += 86400;
		--days;
	}
	days += 719468;
	const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	const int64_t doe = days - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int64_t day = doy - (153 * mp + 2) / 5 + 1;
	const int64_t month = mp < 10 ? mp + 3 : mp - 9;
	const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
	char text[96]; // room for any year the arithmetic can reach
	std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(year),
			static_cast<long long>(month), static_cast<long long>(day), static_cast<long long>(secs / 3600),
			static_cast<long long>(secs % 3600 / 60), static_cast<long long>(secs % 60));
	return text;
}

} // namespace opennova::novaworld_server::hosted
