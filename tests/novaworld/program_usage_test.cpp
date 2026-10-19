// What each program's tasks use (apps/novaworld_common/program/program_usage.h), over fixture
// trees standing in for a container's kernel files and a migrated database of the run's own
// (common/test_database.h):
//
//   - the cgroup's CPU limit: v2's cpu.max (a quota, "max"), v1's cfs quota over its period
//     (-1 none), none at all;
//   - the sampler: its first minute without CPU, then the CPU over the minute in percent of
//     the task's limit; the memory and its limit (the cgroup's, else the task's, else the
//     machine's); /proc/net/dev's bytes; a file that does not read leaves its field null;
//   - the task found off ECS: the hostname, the cgroup's quota as its CPU limit;
//   - the rows: a second sample of a task's minute ignored; the hours rolled up from the
//     minutes (the mean and peak CPU, the peak memory, the bytes in and out with a counter
//     that started over), twice alike; an hour whose minutes are partly pruned left as it
//     was; the prunes at 48 hours and 400 days.

#include "program/program_usage.h"

#include <net/novaworld/db/database.h>

#include "common/temp_dir.h"
#include "common/test_database.h"
#include "common/test_expect.h"

#include <base/io/file_io.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace io = opennova::io;
namespace program = opennova::novaworld_server::program;
using opennova::db::BindValue;
using opennova::db::Database;

namespace {

// 2026-10-18T12:00:00Z.
constexpr int64_t kNoon = 1792324800;

bool put(const test_temp::TempDir &dir, const std::string &relative, const std::string &text) {
	const std::string path = dir.root() + "/" + relative;
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::string error;
	return io::write_file_atomic(path, text, error);
}

bool near(double a, double b) {
	return std::fabs(a - b) < 1e-4;
}

double double_at(const opennova::db::Row &row, size_t i) {
	if (const auto *d = std::get_if<double>(&row.values[i]))
		return *d;
	if (const auto *n = std::get_if<int64_t>(&row.values[i]))
		return static_cast<double>(*n);
	return -1;
}

std::string net_dev(uint64_t in, uint64_t out) {
	return "Inter-|   Receive |  Transmit\n face |bytes packets|bytes packets\n"
		   "    lo: 500 5 0 0 0 0 0 0 500 5 0 0 0 0 0 0\n"
		   "  eth1: " +
			std::to_string(in) + " 10 0 0 0 0 0 0 " + std::to_string(out) + " 12 0 0 0 0 0 0\n";
}

int test_cpu_limit() {
	test_temp::TempDir dir("program_usage_limit");
	TEST_EXPECT(program::read_cgroup_cpu_limit(dir.root()) == 0);
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu/cpu.cfs_quota_us", "25000\n") &&
			put(dir, "sys/fs/cgroup/cpu/cpu.cfs_period_us", "100000\n"));
	TEST_EXPECT(near(program::read_cgroup_cpu_limit(dir.root()), 0.25));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu/cpu.cfs_quota_us", "-1\n"));
	TEST_EXPECT(program::read_cgroup_cpu_limit(dir.root()) == 0);
	// v2's file first where it is.
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu.max", "max 100000\n"));
	TEST_EXPECT(program::read_cgroup_cpu_limit(dir.root()) == 0);
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu.max", "150000 100000\n"));
	TEST_EXPECT(near(program::read_cgroup_cpu_limit(dir.root()), 1.5));
	std::printf("  the cgroup's CPU limit: v2's cpu.max, v1's quota over its period, none\n");
	return 0;
}

int test_sampler() {
	test_temp::TempDir dir("program_usage_sampler");
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu.stat", "usage_usec 1000000\nuser_usec 800000\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.current", "300000000\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.max", "max\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.stat", "anon 1\ninactive_file 100000000\n"));
	TEST_EXPECT(put(dir, "proc/net/dev", net_dev(12000, 34000)));
	TEST_EXPECT(put(dir, "proc/meminfo", "MemTotal: 4000000 kB\nMemAvailable: 1000000 kB\n"));

	program::UsageTask task;
	task.id = "0123456789abcdef0123456789abcdef";
	task.cpu_limit = 0.5;
	task.memory_limit_bytes = 1024ull * 1024 * 1024;
	program::UsageSampler sampler("web", task, dir.root());
	const program::UsageSample first = sampler.take(kNoon + 37, 5'000'000);
	TEST_EXPECT(first.program == "web" && first.task_id == task.id && first.at == kNoon);
	TEST_EXPECT(!first.cpu_percent && first.cpu_limit && near(*first.cpu_limit, 0.5));
	// The cgroup sets no limit: the task's.
	TEST_EXPECT(first.memory_used_bytes == 200000000 &&
			first.memory_limit_bytes == int64_t(1024) * 1024 * 1024);
	TEST_EXPECT(first.net_in_bytes == 12000 && first.net_out_bytes == 34000);

	// A minute later: 15 s of CPU over 60 s is half a CPU, all of the task's half.
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu.stat", "usage_usec 16000000\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.max", "536870912\n"));
	TEST_EXPECT(put(dir, "proc/net/dev", net_dev(20000, 90000)));
	const program::UsageSample second = sampler.take(kNoon + 97, 65'000'000);
	TEST_EXPECT(second.at == kNoon + 60);
	TEST_EXPECT(second.cpu_percent && near(*second.cpu_percent, 50.0));
	// The cgroup's own limit stands over the task's.
	TEST_EXPECT(second.memory_limit_bytes == 536870912);
	TEST_EXPECT(second.net_in_bytes == 20000 && second.net_out_bytes == 90000);

	// No cgroup memory and no limit anywhere: the machine's memory; no network: null.
	program::UsageTask bare;
	bare.id = "bare";
	bare.cpu_limit = 2;
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.max", "max\n"));
	std::filesystem::remove(std::filesystem::path(dir.root() + "/proc/net/dev"));
	program::UsageSampler machine("lobby", bare, dir.root());
	const program::UsageSample third = machine.take(kNoon, 1);
	TEST_EXPECT(third.memory_limit_bytes == int64_t(4000000) * 1024);
	TEST_EXPECT(!third.net_in_bytes && !third.net_out_bytes);
	// Nothing reads at all: every field null but the limit.
	test_temp::TempDir empty("program_usage_empty");
	program::UsageSampler none("gate", bare, empty.root());
	const program::UsageSample nothing = none.take(kNoon, 1);
	TEST_EXPECT(!nothing.cpu_percent && !nothing.memory_used_bytes && !nothing.memory_limit_bytes &&
			!nothing.net_in_bytes && near(*nothing.cpu_limit, 2));
	std::printf("  the sampler: CPU of the task's limit after its first minute, memory and its "
				"limit, the network's bytes, null where nothing reads\n");
	return 0;
}

int test_find_task() {
	test_temp::TempDir dir("program_usage_task");
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu.max", "50000 100000\n"));
	const program::UsageTask task = program::find_usage_task(dir.root());
	TEST_EXPECT(task.id == program::local_task_id() && !task.id.empty());
	TEST_EXPECT(near(task.cpu_limit, 0.5) && task.memory_limit_bytes == 0);
	// No quota: the machine's CPUs.
	test_temp::TempDir empty("program_usage_task_none");
	TEST_EXPECT(program::find_usage_task(empty.root()).cpu_limit >= 1);
	std::printf(
			"  off ECS: the hostname (%s), the cgroup's quota, else the CPUs\n", task.id.c_str());
	return 0;
}

program::UsageSample minute(
		const char *task, int64_t at, std::optional<double> cpu, int64_t memory, int64_t in) {
	program::UsageSample s;
	s.program = "web";
	s.task_id = task;
	s.at = at;
	s.cpu_percent = cpu;
	s.cpu_limit = 0.5;
	s.memory_used_bytes = memory;
	s.memory_limit_bytes = 1 << 30;
	s.net_in_bytes = in;
	s.net_out_bytes = 500;
	return s;
}

int test_rows() {
	test_db::TestDatabase database;
	Database db = database.open();

	// Task a: 11:00 to 12:02, its in-counter growing 100 a minute and starting over at
	// 11:30; its CPU 40 % but its first minute's (none) and 11:59's (100 %).
	double cpu_sum = 0;
	int cpu_minutes = 0;
	for (int k = 0; k < 63; ++k) {
		const int64_t at = kNoon - 3600 + 60 * k;
		std::optional<double> cpu;
		if (k > 0)
			cpu = k == 59 ? 100.0 : 40.0;
		if (cpu && k < 60) {
			cpu_sum += *cpu;
			++cpu_minutes;
		}
		const int64_t in = k < 30 ? 1000 + 100 * k : 50 + 100 * (k - 30);
		program::insert_usage_sample(db, minute("aaaa", at, cpu, 1000 * k, in));
	}
	// A second sample of a minute is ignored; task b has one minute.
	program::insert_usage_sample(db, minute("aaaa", kNoon, 99.0, 1, 1));
	program::insert_usage_sample(db, minute("bbbb", kNoon + 60, 10.0, 7000, 900));
	TEST_EXPECT(db.query("SELECT 1 FROM program_samples;").size() == 64);
	const auto noon = db.query("SELECT cpu_percent, memory_used_bytes, cpu_limit FROM "
							   "program_samples WHERE task_id = 'aaaa' AND at = "
							   "to_timestamp($1::double precision);",
			{ BindValue(kNoon) });
	TEST_EXPECT(noon.size() == 1 && near(double_at(noon[0], 0), 40.0) &&
			noon[0].as_int(1) == 60000 && near(double_at(noon[0], 2), 0.5));

	const int64_t now = kNoon + 180;
	for (int pass = 0; pass < 2; ++pass) {
		program::rollup_usage_hours(db, now);
		const auto hours = db.query("SELECT task_id, CAST(extract(epoch FROM hour) AS bigint), "
									"cpu_avg, cpu_max, memory_peak_bytes, net_in_bytes, "
									"net_out_bytes FROM program_hourly ORDER BY task_id, hour;");
		TEST_EXPECT(hours.size() == 3);
		// 11:00: 29 minutes of 100 after the first (none before it), the start-over's
		// 50, 29 more of 100.
		TEST_EXPECT(hours[0].as_text(0) == "aaaa" && hours[0].as_int(1) == kNoon - 3600);
		TEST_EXPECT(near(double_at(hours[0], 2), cpu_sum / cpu_minutes) &&
				near(double_at(hours[0], 3), 100.0));
		TEST_EXPECT(hours[0].as_int(4) == 59000 && hours[0].as_int(5) == 5850 &&
				hours[0].as_int(6) == 0);
		// 12:00: three minutes of 100, the first against 11:59.
		TEST_EXPECT(hours[1].as_int(1) == kNoon && hours[1].as_int(5) == 300 &&
				hours[1].as_int(4) == 62000 && near(double_at(hours[1], 3), 40.0));
		TEST_EXPECT(hours[2].as_text(0) == "bbbb" && hours[2].as_int(5) == 0 &&
				hours[2].as_int(4) == 7000 && near(double_at(hours[2], 2), 10.0));
	}

	// 48 hours on: the minutes before 12:02 are gone; 12:00, no longer wholly kept, is
	// left as it was.
	const int64_t later = kNoon + 120 + program::kUsageSampleKeepSeconds;
	program::prune_usage(db, later);
	TEST_EXPECT(db.query("SELECT 1 FROM program_samples;").size() == 1);
	program::rollup_usage_hours(db, later);
	const auto kept = db.query("SELECT net_in_bytes FROM program_hourly WHERE task_id = 'aaaa' "
							   "AND hour = to_timestamp($1::double precision);",
			{ BindValue(kNoon) });
	TEST_EXPECT(kept.size() == 1 && kept[0].as_int(0) == 300);
	// 400 days on from 12:00 less a second: the 11:00 hour goes, 12:00 stays.
	program::prune_usage(db, kNoon - 1 + program::kUsageHourlyKeepSeconds);
	TEST_EXPECT(db.query("SELECT 1 FROM program_hourly;").size() == 2);
	TEST_EXPECT(db.query("SELECT 1 FROM program_hourly WHERE hour < to_timestamp($1::double "
						 "precision);",
						  { BindValue(kNoon) })
					.empty());
	std::printf("  the rows: a minute once, the hours from the minutes (a counter that "
				"started over), the prunes\n");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	test_db::init(argc, argv);
	struct Case {
		const char *name;
		int (*fn)();
	};
	const Case cases[] = {
		{ "cpu_limit", test_cpu_limit },
		{ "sampler", test_sampler },
		{ "find_task", test_find_task },
		{ "rows", test_rows },
	};
	for (const Case &c : cases) {
		std::printf("-- %s\n", c.name);
		if (c.fn() != 0) {
			std::fprintf(stderr, "FAIL: %s\n", c.name);
			return 1;
		}
	}
	std::printf("OK: the programs' own usage\n");
	return 0;
}
