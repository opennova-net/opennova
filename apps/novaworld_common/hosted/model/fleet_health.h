#pragma once

// The fleet's health (ADR 0065): what needs an admin, as findings read from the
// rows alone, so the admin's attention list (GET /api/admin/attention,
// apps/novaworld_web/infra/attention_routes.h) and the fleet leader's heartbeat
// (apps/novaworld_fleet/main.cpp, core/health.h) say the same. read_health reads
// the rows in one snapshot; fleet_health judges them, a pure function, so every
// rule is a test. Each finding has a level, its subject in words, what is wrong
// and since when, the admin page that shows it (a site path) and the rule that
// found it (its `source`):
//
//   stalled_start      warn: a live server requested, provisioning, booting or
//                      listing for over kStalledStartS (one waiting for its
//                      machine to show its static address says so)
//   stuck_machine      warn: launching or booting over kStuckBootS, draining over
//                      kStuckDrainS, ending over kStuckEndS, lost over kStuckLostS
//                      (one the brake holds is lost_held's)
//   flapping_machine   warn: more than kMovesPerHour moves in the last hour
//   lost_held          danger: the mass-lost brake holds a lost machine from its end
//   protocol_mismatch  danger: its agent speaks another protocol than the service's
//                      (held, acted on in nothing, ended after 10 min)
//   outdated_agent     warn: its agent runs another hostd image than the release
//                      pins (app deploy rolls hostd, and confirms it)
//   outdated_image     info: the machines on another image than the launch
//                      templates pin, one finding for the fleet (a fleet roll)
//   machine_pressure   warn: its CPU over kCpuBusyPercent, its memory over
//                      kMemoryUsedPercent, its trees' disk under kDiskFreePercent free
//   late_ticks         warn: a server on it ran more than kLateTicksPerMinute of
//                      its last minute's ticks late (the machine is short of CPU)
//   launches_held      warn: a kind's launch backoff holds, with its last refused
//                      or never booted launch's cause (the cloud's error code)
//   no_lobby           danger: no lobby instance takes clients
//   failed_server      warn: a server that failed in the last kFailedWindowS
//   data_unavailable   warn: a live server whose last kDataFailuresInARow starts
//                      ended for want of their data (hosted_repository.h
//                      StartCause::DataUnavailable, no restart counted, so it
//                      never fails), or whose run of them began over
//                      kDataFailingS ago; danger past kDataFailingDangerS
//   failed_address     warn: a static address the cloud refused (tried again
//                      automatically when the refusal passes)
//   stuck_address      warn: a releasing static address whose release has not
//                      moved for over kStuckReleaseS (no worker runs it)
//   addresses_off      warn: static addresses held while the add-on is off
//                      (ONNET_FLEET_MAX_ADDRESSES 0, or no zone name): only a
//                      worker whose address settings still read releases them
//   orphan_address     warn: an address the cloud holds that no row names
//   oversold           warn: the hosting sold is past what the fleet sells
//   unknown            warn: rows too old to judge: a ready or draining machine's
//                      last report past kStaleReportS (and not marked lost), a
//                      ready machine's use not reported for kStaleStatsS, a server
//                      running kStaleStatsS with no tick health: never healthy
//
// Service policy, not a port.

#include "address_jobs.h"
#include "fleet_repository.h"
#include "hosted_repository.h"
#include "sold_capacity.h"

#include <net/novaworld/db/database.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace opennova::novaworld_server::hosted {

// The bounds the rules hold (seconds, percents, counts).
inline constexpr int64_t kStalledStartS = 15 * 60;
inline constexpr int64_t kStuckBootS = 20 * 60; // launching or booting
inline constexpr int64_t kStuckDrainS = 60 * 60;
inline constexpr int64_t kStuckEndS = 15 * 60;
inline constexpr int64_t kStuckLostS = 10 * 60;
inline constexpr int kMovesPerHour = 6;
inline constexpr double kCpuBusyPercent = 85.0;
inline constexpr double kMemoryUsedPercent = 90.0;
inline constexpr double kDiskFreePercent = 15.0;
// 1 % of a minute's 3,750 ticks (62.5 Hz) started a whole quantum behind.
inline constexpr int64_t kLateTicksPerMinute = 37;
inline constexpr int64_t kFailedWindowS = 24 * 60 * 60;
// A server's starts that ended for want of their data, in a row: each costs a
// few minutes (the tree store's tries), so three, or a quarter of an hour, is
// data that stays out of reach.
inline constexpr int kDataFailuresInARow = 3;
inline constexpr int64_t kDataFailingS = 15 * 60;
inline constexpr int64_t kDataFailingDangerS = 60 * 60;
// A ready machine's agent syncs every few seconds and goes lost after 60 s of
// silence: three times that, unmarked, is a fleet that does not pass.
inline constexpr int64_t kStaleReportS = 3 * 60;
// A machine's use rides its syncs, a server's ticks a once-a-minute file.
inline constexpr int64_t kStaleStatsS = 5 * 60;
// A release's steps: its record's TTL and margin waited out after the delete,
// and ten minutes for the delete's sync and the job's waits between them.
inline constexpr int64_t kStuckReleaseS = kAddressTtlS + kReleaseMarginS + 10 * 60;

enum class HealthLevel : uint8_t { Info, Warn, Danger };
const char *health_level_text(HealthLevel level); // "info", "warn", "danger"

struct Finding {
	HealthLevel level = HealthLevel::Warn;
	std::string subject; // "Machine 12", "Server 34 (Smoke)", "The lobby", ...
	std::string text; // what is wrong, in a sentence
	int64_t since = 0; // Unix seconds; 0 when not known
	std::string link; // the admin page that shows it (a site path)
	std::string source; // the rule (the header's list)
};

// The images the release pins: the launch templates' (ONNET_FLEET_IMAGE_ID, an
// AMI id) and hostd's (ONNET_HOSTED_HOSTD_IMAGE, the reference its task
// definition names); "" for one not known here.
struct ReleaseImages {
	std::string image_id;
	std::string agent_image;
};

// Whether machine `m` runs another image than the release pins: its own image
// against the launch templates', its agent's against hostd's, each pair
// compared where both sides are known. True when one differs, false when every
// pair compared matches, nullopt when none compares (off EC2 and ECS, before
// its first sync). The machine JSON's `outdated` and the outdated_* rules.
std::optional<bool> machine_outdated(const FleetMachineRow &m, const ReleaseImages &release);

// A live machine as the rules read it: its row, and its times as Unix seconds
// (0: none).
struct HealthMachine {
	FleetMachineRow row;
	int64_t state_since = 0; // its move into its state (fleet_machine_events)
	int moves_last_hour = 0;
	int64_t last_report = 0;
	int64_t stats_at = 0;
	int64_t mismatch_since = 0;
	// It carries a static address its public address does not show yet (the
	// supervisor holds its server's start until it does).
	bool address_wait = false;
};

// A server's newest starts that ended for want of their data (data_unavailable),
// in a row back to its last start that ran or ended otherwise (one under way
// that has not run breaks none): how many, since when (the first's end), the
// last's reason (hostd's, the fetch's error: an admin's to read), the server's
// data set and the trees the last pinned.
struct DataFailures {
	int count = 0;
	int64_t since = 0;
	std::string reason;
	std::string data_set; // its display name; "" when its row is gone
	int64_t base_tree = 0; // 0: none recorded
	std::optional<int64_t> expansion_tree;
};

// A live server as the rules read it.
struct HealthServer {
	int64_t id = 0;
	std::string name;
	State state = State::Requested;
	int64_t state_since = 0; // its move into its state (hosted_server_events)
	int64_t machine_id = 0; // the machine its slot is on (0: none)
	std::optional<TickHealth> tick; // its container's last minute, as its agent reported
	std::optional<DataFailures> data_failures; // none in a row: unset
};

struct FailedServer {
	int64_t id = 0;
	std::string name;
	std::string reason;
	int64_t ended_at = 0;
};

// A kind's launch backoff that holds, and the launch that last failed.
struct HeldLaunches {
	MachineKind kind = MachineKind::Shared;
	LaunchBackoff backoff;
	int64_t machine_id = 0; // the newest failed machine of the kind (0: none)
	std::string cause; // its end reason
	int64_t failed_at = 0;
};

struct FailedAddress {
	int64_t id = 0;
	std::string name;
	std::string reason;
	int64_t failed_at = 0;
	bool retried = false; // tried again automatically (its retry_at set)
};

// A releasing static address, and its release's last step (its row's last
// write, Unix seconds).
struct ReleasingAddress {
	int64_t id = 0;
	std::string name;
	int64_t last_step = 0;
};

// An address the cloud holds for this deployment that no row names, as the last
// fleet.sweep found it (hosted_address_orphans).
struct OrphanAddress {
	std::string allocation_id;
	std::string public_ip;
	int64_t seen_at = 0;
};

struct HealthInputs {
	int64_t now = 0; // Unix seconds, the database's clock
	std::vector<HealthMachine> machines; // the live ones, by id
	std::vector<HealthServer> servers; // the live ones, by id
	std::vector<FailedServer> failed; // failed in the last kFailedWindowS, newest first
	std::vector<HeldLaunches> held; // the kinds whose backoff holds now
	size_t lobbies_live = 0; // lobbydb::live_lobbies
	int64_t lobby_last_seen = 0; // the newest instance's last_seen_at (0: none)
	std::vector<FailedAddress> failed_addresses;
	std::vector<ReleasingAddress> releasing_addresses;
	std::vector<OrphanAddress> orphans;
	// The static addresses the cloud may hold now (address_repository.h
	// count_held_addresses), and whether the add-on is on (hosting on and its
	// policy's max_addresses above 0).
	int held_addresses = 0;
	bool addresses_on = false;
	// With hosting on: what is sold, and what the fleet sells.
	std::optional<Capacity> sold;
	Capacity sellable;
};

// The rows the rules read, in one snapshot; `policy` is the fleet's (hosting
// on), null with hosting off (nothing sold is judged). Throws db::Error.
HealthInputs read_health(db::Database &db, const PlacementPolicy *policy);

// The findings, in sort_findings' order.
std::vector<Finding> fleet_health(const HealthInputs &in, const ReleaseImages &release);

// `findings` danger first, then warn, then info, each level's oldest first (one
// with no time after the rest), the order kept among equals.
void sort_findings(std::vector<Finding> &findings);

// What the fleet leader's heartbeat carries of them (core/health.h).
struct HealthGauges {
	int danger = 0;
	int warn = 0;
	int machines_stuck = 0; // the machines a stuck_machine or flapping_machine finding names
	// The least free share of a ready or draining machine's trees' disk, percent
	// rounded down, of the machines whose use the rules judge (reported within
	// kStaleReportS, its use within kStaleStatsS; older is the unknown rule's);
	// unset with none.
	std::optional<int> disk_free_min_percent;
};
HealthGauges health_gauges(const HealthInputs &in, const std::vector<Finding> &findings);

} // namespace opennova::novaworld_server::hosted
