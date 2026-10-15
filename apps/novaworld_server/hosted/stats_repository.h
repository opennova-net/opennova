#pragma once

// The hosted servers' stats in the database (backend/migrations/
// 0004_server_stats.sql): the per-minute samples the sampler writes
// (stats_sampler.h), their hourly rollups, each machine's egress by the
// calendar month, and the reads the stats routes answer from (stats_routes.h):
// one server's, an account's servers' together, or the whole fleet's, over a
// range, as a summary and a series of points. Counts alone: no player's name
// is stored. Crow-free. Each function throws db::Error on a database failure.
// Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// How long the samples and the hourly rollups are kept.
inline constexpr int64_t kSampleKeepSeconds = 48 * 60 * 60;
inline constexpr int64_t kHourlyKeepSeconds = 90 * 24 * 60 * 60;

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
};

// Writes the sample; a second one of the same server and minute is ignored.
void insert_sample(db::Database &db, const ServerSample &sample);
// Machine `machine_id`'s egress for the calendar month (UTC) holding `at`
// grows by `bytes`.
void add_machine_egress(db::Database &db, int64_t machine_id, int64_t at, int64_t bytes);
// Server `server_id`'s hour (UTC) holding `at` written again from its samples,
// its unique players at least `unique_players` (the sampler's count; a
// smaller one, the service's restart having lost its names, keeps the larger).
void rollup_hour(db::Database &db, int64_t server_id, int64_t at, int64_t unique_players);
// The samples older than kSampleKeepSeconds, the hours older than
// kHourlyKeepSeconds and the machine months more than 13 months back, at `now`
// (Unix seconds).
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
	int64_t restarts = 0;        // crash restarts counted in the range
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
// How long server `server_id` has been running at `now`, in seconds; nullopt
// when it is not running.
std::optional<int64_t> running_seconds_of(db::Database &db, int64_t server_id, int64_t now);

// "2026-10-15T09:41:00Z": Unix seconds in ISO 8601, UTC.
std::string iso_utc(int64_t unix_seconds);

} // namespace opennova::novaworld_server::hosted
