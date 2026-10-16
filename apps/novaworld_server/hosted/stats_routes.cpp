#include "stats_routes.h"

#include "auth.h"
#include "billing/entitlement.h"
#include "fleet_repository.h"
#include "http_json.h"

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <net/novaworld/host_repository.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

int64_t id_of(uint64_t id) {
	return id <= static_cast<uint64_t>(INT64_MAX) ? static_cast<int64_t>(id) : 0;
}

template <typename T>
void value_or_null(crow::json::wvalue &out, const char *key, const std::optional<T> &value) {
	if (value)
		out[key] = *value;
	else
		out[key] = nullptr;
}

void text_or_null(crow::json::wvalue &out, const char *key, const std::string &text) {
	if (text.empty())
		out[key] = nullptr;
	else
		out[key] = text;
}

// The listing's TimeLeft (whole minutes of the round clock; below 0 none), in seconds.
std::optional<int64_t> time_left_seconds(const std::string &minutes) {
	const std::string text = strutil::trim(minutes);
	if (text.empty() || text.size() > 9 || !strutil::all_digits(text)) return std::nullopt;
	const auto value = strutil::parse_llong(text);
	if (!value || *value < 0) return std::nullopt;
	return static_cast<int64_t>(*value) * 60;
}

// One server's live view: its state, how long it has run, its listing's
// players, slots, mission, game type and round clock, its admin link's scene
// and state, and its newest sample's CPU and memory; null for an ended server.
crow::json::wvalue live_json(db::Database &db, const HostedServer &s, const AdminLinks *links, int64_t now) {
	crow::json::wvalue live;
	if (!is_live(s.state)) {
		live = nullptr;
		return live;
	}
	live["state"] = state_name(s.state);
	value_or_null(live, "uptime_seconds", running_seconds_of(db, s.id, now));
	std::optional<hostdb::HostRow> listing;
	if (s.service_user_id) {
		if (const auto rid = listing_rid_of(db, *s.service_user_id)) listing = hostdb::find_host_by_rid(db, *rid);
	}
	if (listing) {
		live["players"] = listing->player_count;
		live["max_players"] = listing->max_players;
		text_or_null(live, "mission", retail_text_to_utf8(listing->mission_name));
		text_or_null(live, "game_type", retail_text_to_utf8(listing->game_type));
		value_or_null(live, "time_left", time_left_seconds(listing->time_left));
	} else {
		live["players"] = nullptr;
		live["max_players"] = nullptr;
		live["mission"] = nullptr;
		live["game_type"] = nullptr;
		live["time_left"] = nullptr;
	}
	const auto link = links != nullptr ? links->find(s.id) : nullptr;
	if (link) {
		const AdminLinkStatus status = link->status();
		std::string scene = status.game_state;
		for (char &c : scene) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
		text_or_null(live, "scene", scene);
		live["link_state"] = admin_link_state_name(status.state);
	} else {
		live["scene"] = nullptr;
		live["link_state"] = nullptr;
	}
	const auto sample = latest_sample(db, s.id, now, StatsRoutes::kLiveSampleMaxAge);
	value_or_null(live, "cpu_percent", sample ? sample->cpu_percent : std::nullopt);
	value_or_null(live, "memory_bytes", sample ? sample->memory_bytes : std::nullopt);
	return live;
}

// What a set of live servers use of their slots, from their newest samples
// and their last hour: the usage view's totals, the fleet's and each
// machine's. A server reports when its newest sample carries its agent's
// numbers (its container's CPU and memory); each that does adds its slot's
// limits.
struct UsageTotals {
	int64_t reporting = 0;
	int64_t players = 0;
	int64_t memory_bytes = 0;
	int64_t memory_limit_bytes = 0;
	double cpus_used = 0.0;
	double cpus_limit = 0.0;
	double egress_bytes_per_second = 0.0;
	int64_t egress_bytes_window = 0;
};

void add_usage(UsageTotals &totals, const ServerSample *sample, const SampleWindow *window,
		int64_t slot_memory_bytes, double slot_cpus) {
	if (sample) {
		if (sample->players)
			totals.players += *sample->players;
		totals.egress_bytes_per_second += static_cast<double>(sample->egress_bytes) / 60.0;
		if (sample->memory_bytes) {
			++totals.reporting;
			totals.memory_bytes += *sample->memory_bytes;
			totals.memory_limit_bytes += slot_memory_bytes;
			totals.cpus_used += sample->cpu_percent.value_or(0.0) / 100.0 * slot_cpus;
			totals.cpus_limit += slot_cpus;
		}
	}
	if (window)
		totals.egress_bytes_window += window->egress_bytes;
}

void put_usage(crow::json::wvalue &out, const UsageTotals &totals) {
	out["reporting"] = totals.reporting;
	out["players"] = totals.players;
	out["memory_bytes"] = totals.memory_bytes;
	out["memory_limit_bytes"] = totals.memory_limit_bytes;
	out["cpus_used"] = totals.cpus_used;
	out["cpus_limit"] = totals.cpus_limit;
	out["egress_bytes_per_second"] = totals.egress_bytes_per_second;
	out["egress_bytes_window"] = totals.egress_bytes_window;
}

std::vector<crow::json::wvalue> text_list(const std::vector<std::string> &texts) {
	std::vector<crow::json::wvalue> out;
	for (const std::string &t : texts) {
		crow::json::wvalue v;
		v = t;
		out.push_back(std::move(v));
	}
	return out;
}

} // namespace

void StatsRoutes::configure(int slot_memory_mb, double slot_cpus) {
	slot_memory_mb_ = slot_memory_mb > 0 ? slot_memory_mb : 512;
	slot_cpus_ = slot_cpus > 0 ? slot_cpus : 1.0;
}

void StatsRoutes::register_routes(WebApp &app, const AdminLinks *links) {
	links_ = links;
	CROW_ROUTE(app, "/api/me/servers/stats")
			.methods("GET"_method)([this](const crow::request &req) { return me_servers(req); });
	CROW_ROUTE(app, "/api/me/servers/<uint>/stats")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) { return me_server(req, id); });
	CROW_ROUTE(app, "/api/admin/hosted/stats")
			.methods("GET"_method)([this](const crow::request &req) { return admin_fleet(req); });
	CROW_ROUTE(app, "/api/admin/hosted/usage")
			.methods("GET"_method)([this](const crow::request &req) { return admin_usage(req); });
	CROW_ROUTE(app, "/api/admin/hosted/<uint>/stats")
			.methods("GET"_method)([this](const crow::request &req, uint64_t id) { return admin_server(req, id); });
}

std::optional<crow::response> StatsRoutes::take(const RouteAccess &access) {
	const std::string key = access.user ? "stats|" + std::to_string(access.user->id) : std::string("stats|token");
	if (const int64_t wait = reads_.take(key, RateLimiter::Clock::now())) return too_many_requests(wait);
	return std::nullopt;
}

// The reply, for every route: {"server_id" (one server's stats alone), "range"
// (the range read: 1h, 24h by default, 7d or 30d; another is 400
// invalid_field), "live" (one server's live view, null for an ended server and
// for the account's and the fleet's totals: {state, uptime_seconds (null while
// not running), players, max_players, mission, game_type, time_left (seconds of
// the round's clock), scene (game, menus, unknown), link_state, cpu_percent
// (0..100 of the slot's CPUs), memory_bytes}, each null where nothing is
// known), "summary" ({live_seconds and running_seconds (server-seconds),
// availability (running over live, 0..1, null with none), restarts (crash
// restarts), peak_players, avg_players (players at a time while any ran; null
// with none), player_hours, unique_players, missions, egress_bytes,
// egress_bytes_month (this calendar month, UTC), approximate (the names of the
// fields counted approximately)}), "series" ({step_seconds, points: one per
// step, oldest first, {t (the step's start, ISO 8601 UTC), players, max_players,
// running (any of the servers running), cpu_percent, egress_bytes}, null where
// no sample fell})}. no-store.
crow::response StatsRoutes::reply(const crow::request &req, const StatsScope &scope, const HostedServer *server) {
	const char *range_text = req.url_params.get("range");
	const auto range = parse_stats_range(range_text ? range_text : "24h");
	if (!range) return json_error(400, "invalid_field", "range is 1h, 24h, 7d or 30d");
	const int64_t now = billing::unix_now();
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::Database &db = *db_conn;
		if (server) out["server_id"] = server->id;
		out["range"] = range->name;
		if (server)
			out["live"] = live_json(db, *server, links_, now);
		else
			out["live"] = nullptr;
		const StatsSummary s = stats_summary(db, scope, *range, now);
		crow::json::wvalue summary;
		summary["live_seconds"] = s.live_seconds;
		summary["running_seconds"] = s.running_seconds;
		if (s.live_seconds > 0)
			summary["availability"] = static_cast<double>(s.running_seconds) / static_cast<double>(s.live_seconds);
		else
			summary["availability"] = nullptr;
		summary["restarts"] = s.restarts;
		summary["peak_players"] = s.peak_players;
		value_or_null(summary, "avg_players", s.avg_players);
		summary["player_hours"] = s.player_hours;
		summary["unique_players"] = s.unique_players;
		summary["missions"] = s.missions;
		summary["egress_bytes"] = s.egress_bytes;
		summary["egress_bytes_month"] = s.egress_bytes_month;
		std::vector<std::string> approximate = { "unique_players", "missions" };
		if (!server && range->hourly) {
			// The hours' peaks and running minutes added up across servers.
			approximate.push_back("peak_players");
			approximate.push_back("avg_players");
		}
		summary["approximate"] = text_list(approximate);
		out["summary"] = std::move(summary);
		crow::json::wvalue series;
		series["step_seconds"] = range->step_seconds;
		std::vector<crow::json::wvalue> points;
		for (const StatsPoint &p : stats_series(db, scope, *range, now)) {
			crow::json::wvalue point;
			point["t"] = iso_utc(p.t);
			value_or_null(point, "players", p.players);
			value_or_null(point, "max_players", p.max_players);
			point["running"] = p.running;
			value_or_null(point, "cpu_percent", p.cpu_percent);
			value_or_null(point, "egress_bytes", p.egress_bytes);
			points.push_back(std::move(point));
		}
		series["points"] = std::move(points);
		out["series"] = std::move(series);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET %s failed: %s\n", req.url.c_str(), e.what());
		return json_error(500, "db_error");
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

// GET /api/me/servers/<id>/stats — one of the account's servers' stats (any
// state); 404 not_found for an id that is not the account's.
crow::response StatsRoutes::me_server(const crow::request &req, uint64_t id) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	if (auto refused = take(access)) return std::move(*refused);
	std::optional<HostedServer> server;
	try {
		server = owned_server(*pool_.acquire(), id_of(id), access.user->id);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/me/servers/%llu/stats failed: %s\n", static_cast<unsigned long long>(id),
				e.what());
		return json_error(500, "db_error");
	}
	if (!server) return json_error(404, "not_found", "this account has no hosted server with this id");
	StatsScope scope;
	scope.server_id = server->id;
	return reply(req, scope, &*server);
}

// GET /api/me/servers/stats — the account's servers together (each it owns, in
// any state).
crow::response StatsRoutes::me_servers(const crow::request &req) {
	auto access = access_.require_user(req);
	if (access.refusal) return std::move(*access.refusal);
	if (auto refused = take(access)) return std::move(*refused);
	StatsScope scope;
	scope.owner_user_id = access.user->id;
	return reply(req, scope, nullptr);
}

// GET /api/admin/hosted/<id>/stats — any server's stats; 404 not_found.
crow::response StatsRoutes::admin_server(const crow::request &req, uint64_t id) {
	auto access = access_.require_admin(req);
	if (access.refusal) return std::move(*access.refusal);
	if (auto refused = take(access)) return std::move(*refused);
	std::optional<HostedServer> server;
	try {
		server = find_server(*pool_.acquire(), id_of(id));
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/hosted/%llu/stats failed: %s\n",
				static_cast<unsigned long long>(id), e.what());
		return json_error(500, "db_error");
	}
	if (!server) return json_error(404, "not_found", "no hosted server has this id");
	StatsScope scope;
	scope.server_id = server->id;
	return reply(req, scope, &*server);
}

// GET /api/admin/hosted/stats — the fleet's: every server together.
crow::response StatsRoutes::admin_fleet(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal) return std::move(*access.refusal);
	if (auto refused = take(access)) return std::move(*refused);
	return reply(req, StatsScope{}, nullptr);
}

// GET /api/admin/hosted/usage — what every live hosted server (any state but
// stopped and failed, on any machine) uses of its slot, from the stats'
// samples (stats_sampler.h, one a minute): {"at" (now, ISO 8601 UTC),
// "window_seconds" (kUsageWindowSeconds), "slot_memory_bytes" and "slot_cpus"
// (each container's limits), "totals" (the fleet's: servers_live,
// machines_live, slots and slots_used over the live machines, then the usage
// fields below over every live server), "machines" (each live machine by id:
// id, kind, state, instance_type, slots, slots_used, and the usage fields over
// the servers on it), "servers" (by machine, then slot, the ones placed
// nowhere last, by id: id, name, state, own_machine, owner ({id, username} or
// null), machine_ref and slot (null with none), uptime_seconds (while
// running, else null), its newest sample within kLiveSampleMaxAge (sample_at,
// players, max_players, cpu_percent (0..100 of the slot's CPUs),
// memory_bytes, egress_bytes_per_second (that minute's bytes over 60 s); each
// null without one), and "window", its samples in the last
// kUsageWindowSeconds summed up (minutes, cpu_percent_mean, cpu_percent_peak,
// memory_bytes_peak, egress_bytes))}. The usage fields: reporting (the
// servers whose newest sample carries their agent's CPU and memory), players,
// memory_bytes and memory_limit_bytes (the reporting servers' memory and
// their slots' limits), cpus_used and cpus_limit (likewise, in CPUs),
// egress_bytes_per_second (the newest samples' rates added up) and
// egress_bytes_window (the window's bytes). Reads only; no-store.
crow::response StatsRoutes::admin_usage(const crow::request &req) {
	auto access = access_.require_admin(req);
	if (access.refusal)
		return std::move(*access.refusal);
	if (auto refused = take(access))
		return std::move(*refused);
	const int64_t now = billing::unix_now();
	const int64_t slot_memory_bytes = static_cast<int64_t>(slot_memory_mb_) * 1024 * 1024;
	crow::json::wvalue out;
	try {
		auto db_conn = pool_.acquire();
		db::Database &db = *db_conn;
		db::ReadSnapshot snapshot(db);
		std::vector<HostedServer> servers = list_live(db);
		const std::vector<FleetMachineRow> machines = list_live_machines(db);
		const std::map<int64_t, ServerSample> latest =
				latest_samples_since(db, now - kLiveSampleMaxAge);
		const std::map<int64_t, SampleWindow> windows =
				sample_windows_since(db, now - kUsageWindowSeconds);
		std::stable_sort(
				servers.begin(), servers.end(), [](const HostedServer &a, const HostedServer &b) {
					if (a.machine_ref.has_value() != b.machine_ref.has_value())
						return a.machine_ref.has_value();
					if (a.machine_ref && *a.machine_ref != *b.machine_ref)
						return *a.machine_ref < *b.machine_ref;
					if (a.slot != b.slot)
						return a.slot < b.slot;
					return a.id < b.id;
				});

		UsageTotals fleet;
		std::map<int64_t, UsageTotals> by_machine;
		std::map<int64_t, int64_t> slots_used;
		std::vector<crow::json::wvalue> rows;
		for (const HostedServer &s : servers) {
			const auto sample_it = latest.find(s.id);
			const ServerSample *sample = sample_it != latest.end() ? &sample_it->second : nullptr;
			const auto window_it = windows.find(s.id);
			const SampleWindow *window = window_it != windows.end() ? &window_it->second : nullptr;
			add_usage(fleet, sample, window, slot_memory_bytes, slot_cpus_);
			if (s.machine_ref) {
				add_usage(
						by_machine[*s.machine_ref], sample, window, slot_memory_bytes, slot_cpus_);
				if (s.slot >= 0)
					++slots_used[*s.machine_ref];
			}

			crow::json::wvalue row;
			row["id"] = s.id;
			row["name"] = s.settings.game_name;
			row["state"] = state_name(s.state);
			row["own_machine"] = s.own_machine;
			const auto owner =
					s.owner_user_id ? get_user_by_id(db, *s.owner_user_id) : std::nullopt;
			if (owner) {
				crow::json::wvalue who;
				who["id"] = owner->id;
				who["username"] = owner->username;
				row["owner"] = std::move(who);
			} else {
				row["owner"] = nullptr;
			}
			value_or_null(row, "machine_ref", s.machine_ref);
			value_or_null(row, "slot",
					s.machine_ref && s.slot >= 0 ? std::optional<int64_t>(s.slot) : std::nullopt);
			value_or_null(row, "uptime_seconds", running_seconds_of(db, s.id, now));
			if (sample) {
				row["sample_at"] = iso_utc(sample->at);
				value_or_null(row, "players", sample->players);
				value_or_null(row, "max_players", sample->max_players);
				value_or_null(row, "cpu_percent", sample->cpu_percent);
				value_or_null(row, "memory_bytes", sample->memory_bytes);
				row["egress_bytes_per_second"] = static_cast<double>(sample->egress_bytes) / 60.0;
			} else {
				for (const char *key : { "sample_at", "players", "max_players", "cpu_percent",
							 "memory_bytes", "egress_bytes_per_second" })
					row[key] = nullptr;
			}
			crow::json::wvalue last;
			last["minutes"] = window ? window->minutes : 0;
			value_or_null(
					last, "cpu_percent_mean", window ? window->cpu_percent_mean : std::nullopt);
			value_or_null(
					last, "cpu_percent_peak", window ? window->cpu_percent_peak : std::nullopt);
			value_or_null(
					last, "memory_bytes_peak", window ? window->memory_bytes_peak : std::nullopt);
			last["egress_bytes"] = window ? window->egress_bytes : 0;
			row["window"] = std::move(last);
			rows.push_back(std::move(row));
		}

		int64_t slots = 0;
		int64_t used = 0;
		std::vector<crow::json::wvalue> machine_rows;
		for (const FleetMachineRow &m : machines) {
			crow::json::wvalue row;
			row["id"] = m.id;
			row["kind"] = machine_kind_text(m.kind);
			row["state"] = fleet_machine_state_text(m.state);
			text_or_null(row, "instance_type", m.instance_type);
			row["slots"] = m.slots;
			row["slots_used"] = slots_used[m.id];
			put_usage(row, by_machine[m.id]);
			machine_rows.push_back(std::move(row));
			slots += m.slots;
			used += slots_used[m.id];
		}

		out["at"] = iso_utc(now);
		out["window_seconds"] = kUsageWindowSeconds;
		out["slot_memory_bytes"] = slot_memory_bytes;
		out["slot_cpus"] = slot_cpus_;
		crow::json::wvalue totals;
		totals["servers_live"] = static_cast<int64_t>(servers.size());
		totals["machines_live"] = static_cast<int64_t>(machines.size());
		totals["slots"] = slots;
		totals["slots_used"] = used;
		put_usage(totals, fleet);
		out["totals"] = std::move(totals);
		out["machines"] = std::move(machine_rows);
		out["servers"] = std::move(rows);
	} catch (const db::Error &e) {
		std::fprintf(stderr, "[http] GET /api/admin/hosted/usage failed: %s\n", e.what());
		return json_error(500, "db_error");
	}
	crow::response res = json_reply(200, out);
	res.set_header("Cache-Control", "no-store");
	return res;
}

} // namespace opennova::novaworld_server::hosted
