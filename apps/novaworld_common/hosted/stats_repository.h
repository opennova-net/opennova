#pragma once

// The hosted servers' stats in the database (backend/migrations/
// 0004_server_stats.sql, 0016_fleet_observability.sql): the per-minute samples
// the sampler writes (stats_sampler.h), their hourly rollups, each machine's
// egress by the calendar month, each machine's own minutes and hours (its
// agent's reports of its use), and the reads the stats routes answer from (stats_routes.h):
// one server's, an account's servers' together, or the whole fleet's, over a
// range, as a summary and a series of points; and every server's newest
// sample with its last hour summed up (the admin's usage view). Counts alone:
// no player's name is stored. Crow-free. Each function throws db::Error on a
// database failure.
// Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// How long the samples and the hourly rollups are kept (the servers' and the
// machines' alike).
inline constexpr int64_t kSampleKeepSeconds = 48 * 60 * 60;
inline constexpr int64_t kHourlyKeepSeconds = 400 * 24 * 60 * 60;
// A machine whose last report of its use is older than this has no minute.
inline constexpr int64_t kMachineStatsFreshSeconds = 2 * 60;

// One server's minute.
struct ServerSample {
	int64_t server_id = 0;
	int64_t at = 0; // Unix seconds, a whole minute
	std::string state;
	std::optional<int64_t> players;     // its listing's; unset while not listed
	std::optional<int64_t> max_players;
	std::string mission;                // UTF-8; "" while not listed
	std::string scene;                  // "game", "menus", "unknown"; "" with no link
	bool link_up = false;
	std::optional<double> cpu_percent;  // of the slot's CPUs, 0..100
	std::optional<int64_t> memory_bytes;
	int64_t egress_bytes = 0;           // sent this minute
	bool mission_started = false;
	// Its slot's network as counters since its first sample on the slot (a
	// reader takes the differences; a count below the last starts over), and
	// its server's last minute of ticks; unset without a report.
	std::optional<int64_t> net_in_bytes;
	std::optional<int64_t> net_out_bytes;
	std::optional<double> tick_p50_ms;
	std::optional<double> tick_p99_ms;
	std::optional<double> tick_max_ms;
	std::optional<int64_t> late_ticks;
};

// Writes the sample; a second one of the same server and minute is ignored.
void insert_sample(db::Database &db, const ServerSample &sample);
// Machine `machine_id`'s egress for the calendar month (UTC) holding `at`
// grows by `bytes`.
void add_machine_egress(db::Database &db, int64_t machine_id, int64_t at, int64_t bytes);
// Server `server_id`'s hour (UTC) holding `at` written again from its samples,
// its unique players at least `unique_players` (the sampler's count; a
// smaller one, the service's restart having lost its names, keeps the larger):
// its peak memory, its bytes in (the counters' differences) and out, its worst
// minute's p99 and its late ticks among the rest.
void rollup_hour(db::Database &db, int64_t server_id, int64_t at, int64_t unique_players);
// Every live machine's minute `minute` (Unix seconds), from its agent's last
// report of its use (fleet_machines: none older than kMachineStatsFreshSeconds
// by the database's clock) with how many live servers hold a slot on it; a
// second one of the same machine and minute is ignored. How many it wrote.
size_t sample_machines(db::Database &db, int64_t minute);
// The machines' hours holding `now` and the one before it written again from
// their minutes (fleet_machine_hourly: the CPU's mean and peak, the memory's
// peak, the least disk room, the bytes in and out, each the counters'
// differences, a count below the last one starting over); stats.prune's, every
// hour.
void rollup_machine_hours(db::Database &db, int64_t now);
// The samples older than kSampleKeepSeconds, the hours older than
// kHourlyKeepSeconds (the servers' and the machines') and the machine months
// more than 13 months back, at `now` (Unix seconds).
void prune_stats(db::Database &db, int64_t now);

// A range the routes take: its name, its length, its points' step, and whether
// it reads the hourly rollups (7d, 30d) rather than the samples (1h, 24h).
struct StatsRange {
	std::string name;
	int64_t seconds = 0;
	int64_t step_seconds = 0;
	bool hourly = false;
};
// "1h" (a point a minute), "24h" (5 minutes), "7d" (an hour), "30d" (4 hours);
// nullopt for any other.
std::optional<StatsRange> parse_stats_range(std::string_view text);
// The range's window at `now`: [start, end), end the end of the step holding `now`.
int64_t stats_range_end(const StatsRange &range, int64_t now);

// Whose stats: one server, an account's servers, or (neither) the fleet's.
struct StatsScope {
	std::optional<int64_t> server_id;
	std::optional<int64_t> owner_user_id;
};

struct StatsSummary {
	int64_t live_seconds = 0;    // server-seconds live in the range
	int64_t running_seconds = 0; // of them, running
	int64_t restarts = 0;        // crash restarts: starts its crashes and timeouts ended in the range
	int64_t peak_players = 0;    // the most at once (the scope's servers together)
	std::optional<double> avg_players; // players at a time while any of them ran
	double player_hours = 0.0;
	int64_t unique_players = 0;  // distinct names, an hour at a time, summed
	int64_t missions = 0;        // missions started
	int64_t egress_bytes = 0;
	int64_t egress_bytes_month = 0; // this calendar month (UTC), whatever the range
};

struct StatsPoint {
	int64_t t = 0; // the bucket's start, Unix seconds
	std::optional<double> players;
	std::optional<int64_t> max_players;
	bool running = false; // any of the scope's servers running in the bucket
	std::optional<double> cpu_percent;
	std::optional<int64_t> egress_bytes;
};

StatsSummary stats_summary(db::Database &db, const StatsScope &scope, const StatsRange &range, int64_t now);
// One point per step of the range, oldest first; a bucket with no sample has
// nulls (and running false).
std::vector<StatsPoint> stats_series(db::Database &db, const StatsScope &scope, const StatsRange &range, int64_t now);

// Machine `machine_id`'s egress this calendar month (UTC) at `now`.
int64_t machine_egress_month(db::Database &db, int64_t machine_id, int64_t now);
// Server `server_id`'s newest sample no older than `max_age_s` at `now`.
std::optional<ServerSample> latest_sample(db::Database &db, int64_t server_id, int64_t now, int64_t max_age_s);
// Every server's newest sample taken at or after `since` (Unix seconds), by
// server (the admin's usage view, GET /api/admin/hosted/usage).
std::map<int64_t, ServerSample> latest_samples_since(db::Database &db, int64_t since);

// A server's samples taken at or after a time, summed up: how many, its mean
// and peak CPU (of the slot's CPUs, 0..100) and its peak memory over the ones
// that carry them (unset with none), and the bytes its slot sent.
struct SampleWindow {
	int64_t minutes = 0;
	std::optional<double> cpu_percent_mean;
	std::optional<double> cpu_percent_peak;
	std::optional<int64_t> memory_bytes_peak;
	int64_t egress_bytes = 0;
};
// Every server's samples taken at or after `since` (Unix seconds), by server.
std::map<int64_t, SampleWindow> sample_windows_since(db::Database &db, int64_t since);
// How long server `server_id` has been running at `now`, in seconds; nullopt
// when it is not running.
std::optional<int64_t> running_seconds_of(db::Database &db, int64_t server_id, int64_t now);

// "2026-10-15T09:41:00Z": Unix seconds in ISO 8601, UTC.
std::string iso_utc(int64_t unix_seconds);

} // namespace opennova::novaworld_server::hosted
