#pragma once

// What each program's tasks use (backend/migrations/0018_program_usage.sql):
// every program's task samples itself once a minute on its main loop
// (program.h run_main_loop) and writes its own program_samples row: its CPU
// over the minute in percent of its CPU limit, its memory and the most it may
// use, and its network namespace's bytes in and out, through core/proc_stats.h's
// readers (its own cgroup, /proc/net/dev), alike on ECS and in the local stack's
// containers. The worker's stats.prune (hosted/stats_jobs.h) rolls the minutes
// up by the hour and prunes both (48 hours, 400 days). The admin's usage view
// reads them (apps/novaworld_web/infra/usage_routes.h).
//
// A task is named by its ECS task id (the task metadata's TaskARN, its last
// segment), else by its hostname (a local container's id), and measured
// against its limits: the cgroup's own where it sets one, else the task's
// Limits on ECS (Fargate sets them on the task, not on its containers), else
// the machine's (its CPUs, its memory). Service policy, not a port.

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <optional>
#include <string>

namespace opennova::novaworld_server::program {

// How long the minutes and the hours are kept.
inline constexpr int64_t kUsageSampleKeepSeconds = 48 * 60 * 60;
inline constexpr int64_t kUsageHourlyKeepSeconds = 400 * 24 * 60 * 60;

// The task a program runs as: its id and its limits.
struct UsageTask {
	std::string id;
	double cpu_limit = 0; // CPUs
	uint64_t memory_limit_bytes = 0; // 0: none found (the cgroup's, read with each sample)
};

// The hostname as a task id (a local container's: its 12-hex id): its letters,
// digits, '.', '_' and '-', at most 64; "local" for none. The local log store's
// program files are named by it too (core/program_log.h).
std::string local_task_id();

// This process's task: on ECS (ECS_CONTAINER_METADATA_URI_V4 set) its metadata's
// id and Limits, else the hostname; its CPU limit the cgroup's quota where one
// is set, else the task's, else the machine's CPUs; its memory limit the task's
// on ECS. `root` is where the kernel's files are ("" here; a fixture tree in
// the tests). Never fails: a metadata endpoint that does not answer leaves the
// hostname and the cgroup's limits, with a WARN line.
UsageTask find_usage_task(const std::string &root = "");

// The cgroup's CPU quota in CPUs (v2's cpu.max, else v1's cfs quota over its
// period) under `root`; 0 when it sets none.
double read_cgroup_cpu_limit(const std::string &root = "");

// One minute of a task, as program_samples holds it.
struct UsageSample {
	std::string program;
	std::string task_id;
	int64_t at = 0; // Unix seconds, a whole minute
	std::optional<double> cpu_percent; // of cpu_limit, over the minute before
	std::optional<double> cpu_limit;
	std::optional<int64_t> memory_used_bytes;
	std::optional<int64_t> memory_limit_bytes;
	std::optional<int64_t> net_in_bytes; // since the namespace came up
	std::optional<int64_t> net_out_bytes;
};

// A program's sampler of its own task. Not thread-safe: the main loop's alone.
class UsageSampler {
public:
	UsageSampler(std::string program, UsageTask task, std::string root = "");

	// A sample at `now_s` (Unix seconds, the wall clock; floored to its minute)
	// with its CPU since the last take() (`steady_us` a steady clock in
	// microseconds, the CPU time's measure): none for the first.
	UsageSample take(int64_t now_s, uint64_t steady_us);

	const UsageTask &task() const { return task_; }

private:
	std::string program_;
	UsageTask task_;
	std::string root_;
	bool have_prev_ = false;
	uint64_t prev_cpu_usec_ = 0;
	uint64_t prev_steady_us_ = 0;
};

// Writes the sample; a second one of the same task and minute is ignored.
void insert_usage_sample(db::Database &db, const UsageSample &sample);

// Every task's hours still wholly kept as minutes at `now` (Unix seconds),
// the running one among them, written again from their minutes.
void rollup_usage_hours(db::Database &db, int64_t now);

// The minutes older than kUsageSampleKeepSeconds and the hours older than
// kUsageHourlyKeepSeconds at `now`.
void prune_usage(db::Database &db, int64_t now);

} // namespace opennova::novaworld_server::program
