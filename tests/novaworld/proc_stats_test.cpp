// What a Linux machine or container uses (apps/novaworld_common/core/proc_stats.h), read from
// fixture trees standing in for the kernel's files:
//
//   - /proc/stat's aggregate line: busy and total ticks, a percent between two readings
//     (one CPU of two busy is 50 %), the refusals (a counter gone back, no time passed);
//   - /proc/meminfo: used = MemTotal - MemAvailable, a file lacking either refused;
//   - /proc/net/dev: every interface's bytes but loopback's, summed;
//   - a cgroup v2 tree (memory.max "max" and a number, the inactive file cache taken off)
//     and a v1 tree (cpuacct.usage in nanoseconds, the no-limit value, both controller
//     spellings), v2 read first where both are there, and an empty root refused;
//   - statvfs of a real directory.
//
// Database-free.

#include "core/proc_stats.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <base/io/file_io.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace ps = opennova::novaworld_server::proc_stats;
namespace io = opennova::io;

namespace {

bool put(const test_temp::TempDir &dir, const std::string &relative, const std::string &text) {
	const std::string path = dir.root() + "/" + relative;
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	std::string error;
	return io::write_file_atomic(path, text, error);
}

bool near(double a, double b) {
	return std::fabs(a - b) < 1e-9;
}

int exercise_host_cpu() {
	test_temp::TempDir dir("proc_stats_cpu");
	TEST_EXPECT(put(dir, "proc/stat",
			"cpu  100 20 30 800 50 0 0 0 0 0\n"
			"cpu0 50 10 15 400 25 0 0 0 0 0\n"
			"cpu1 50 10 15 400 25 0 0 0 0 0\n"
			"intr 12345\nctxt 999\nbtime 1700000000\n"));
	ps::CpuTimes prev;
	TEST_EXPECT(ps::read_host_cpu(prev, dir.root()));
	TEST_EXPECT(prev.total == 1000 && prev.busy == 150);

	// A minute later on two CPUs: 12000 ticks passed, one CPU busy (6000 of them).
	TEST_EXPECT(put(dir, "proc/stat", "cpu  5100 20 1030 6800 50 0 0 0 0 0\n"));
	ps::CpuTimes now;
	TEST_EXPECT(ps::read_host_cpu(now, dir.root()));
	TEST_EXPECT(now.total == 13000 && now.busy == 6150);
	TEST_EXPECT(near(ps::cpu_percent(prev, now), 50.0));

	// Steal counts as busy (the machine could not have it); guest is already in user.
	TEST_EXPECT(put(dir, "proc/stat", "cpu  10 0 0 80 0 0 0 10 7 0\n"));
	TEST_EXPECT(ps::read_host_cpu(now, dir.root()));
	TEST_EXPECT(now.total == 100 && now.busy == 20);

	// An old kernel's four columns read; fewer, or a word that is no number, do not.
	TEST_EXPECT(put(dir, "proc/stat", "cpu  1 2 3 4\n"));
	TEST_EXPECT(ps::read_host_cpu(now, dir.root()) && now.total == 10 && now.busy == 6);
	TEST_EXPECT(put(dir, "proc/stat", "cpu  1 2 3\n"));
	TEST_EXPECT(!ps::read_host_cpu(now, dir.root()));
	TEST_EXPECT(put(dir, "proc/stat", "cpu  1 2 x 4 5\n"));
	TEST_EXPECT(!ps::read_host_cpu(now, dir.root()));
	TEST_EXPECT(put(dir, "proc/stat", "cpu0 1 2 3 4 5\n"));
	TEST_EXPECT(!ps::read_host_cpu(now, dir.root()));
	test_temp::TempDir empty("proc_stats_none");
	TEST_EXPECT(!ps::read_host_cpu(now, empty.root()));

	// The percent: a wall clock against a cgroup's usage, of a share of CPUs.
	TEST_EXPECT(near(ps::cpu_percent({ 0, 0 }, { 30'000'000, 60'000'000 }, 1.0), 50.0));
	TEST_EXPECT(near(ps::cpu_percent({ 0, 0 }, { 30'000'000, 60'000'000 }, 0.5), 100.0));
	TEST_EXPECT(near(ps::cpu_percent({ 0, 0 }, { 90'000'000, 60'000'000 }, 1.0), 150.0));
	// No time passed, a counter gone back (a reset), no CPUs: 0.
	TEST_EXPECT(ps::cpu_percent({ 5, 10 }, { 6, 10 }) == 0);
	TEST_EXPECT(ps::cpu_percent({ 5, 10 }, { 4, 20 }) == 0);
	TEST_EXPECT(ps::cpu_percent({ 5, 10 }, { 6, 5 }) == 0);
	TEST_EXPECT(ps::cpu_percent({ 0, 0 }, { 6, 20 }, 0) == 0);
	std::printf("  /proc/stat: busy of total, a percent between readings\n");
	return 0;
}

int exercise_host_memory() {
	test_temp::TempDir dir("proc_stats_mem");
	TEST_EXPECT(put(dir, "proc/meminfo",
			"MemTotal:        4015456 kB\n"
			"MemFree:          214332 kB\n"
			"MemAvailable:    3015456 kB\n"
			"Buffers:           81240 kB\n"
			"Cached:          2489760 kB\n"));
	ps::Memory memory;
	TEST_EXPECT(ps::read_host_memory(memory, dir.root()));
	TEST_EXPECT(memory.total_bytes == uint64_t(4015456) * 1024);
	TEST_EXPECT(memory.used_bytes == uint64_t(1000000) * 1024);

	TEST_EXPECT(put(dir, "proc/meminfo", "MemTotal: 4015456 kB\nMemFree: 214332 kB\n"));
	TEST_EXPECT(!ps::read_host_memory(memory, dir.root()));
	std::printf("  /proc/meminfo: used = MemTotal - MemAvailable\n");
	return 0;
}

int exercise_net_dev() {
	test_temp::TempDir dir("proc_stats_net");
	TEST_EXPECT(put(dir, "proc/net/dev",
			"Inter-|   Receive                                                |  Transmit\n"
			" face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets "
			"errs drop fifo colls carrier compressed\n"
			"    lo: 9000000   900    0    0    0     0          0         0  9000000   900    0 "
			"   0    0     0       0          0\n"
			"  ens5: 1200000  1500    0    0    0     0          0         0   340000   800    0 "
			"   0    0     0       0          0\n"
			"docker0:   5000    10    0    0    0     0          0         0     6000    12    0 "
			"   0    0     0       0          0\n"));
	ps::NetBytes net;
	TEST_EXPECT(ps::read_net_dev(net, dir.root()));
	TEST_EXPECT(net.in_bytes == 1205000 && net.out_bytes == 346000);

	// Loopback alone: read, and nothing counted.
	TEST_EXPECT(put(
			dir, "proc/net/dev", "Inter-|\n face |\n    lo: 10 1 0 0 0 0 0 0 20 2 0 0 0 0 0 0\n"));
	TEST_EXPECT(ps::read_net_dev(net, dir.root()) && net.in_bytes == 0 && net.out_bytes == 0);
	TEST_EXPECT(put(dir, "proc/net/dev", "Inter-|\n face |\n"));
	TEST_EXPECT(!ps::read_net_dev(net, dir.root()));
	std::printf("  /proc/net/dev: every interface but lo, summed\n");
	return 0;
}

int exercise_cgroup_v2() {
	test_temp::TempDir dir("proc_stats_v2");
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu.stat",
			"usage_usec 81234567\nuser_usec 61234567\nsystem_usec 20000000\nnr_periods 0\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.current", "734003200\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.max", "1073741824\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.stat",
			"anon 600000000\nfile 134003200\nactive_file 34003200\ninactive_file 100000000\n"));
	uint64_t usec = 0;
	TEST_EXPECT(ps::read_cgroup_cpu_usec(usec, dir.root()) && usec == 81234567);
	ps::CgroupMemory memory;
	TEST_EXPECT(ps::read_cgroup_memory(memory, dir.root()));
	TEST_EXPECT(memory.current_bytes == 634003200 && memory.limit_bytes == 1073741824);

	// No limit, no memory.stat: the usage as it stands.
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory.max", "max\n"));
	std::filesystem::remove(dir.root() + "/sys/fs/cgroup/memory.stat");
	TEST_EXPECT(ps::read_cgroup_memory(memory, dir.root()));
	TEST_EXPECT(memory.current_bytes == 734003200 && memory.limit_bytes == 0);

	// v1's files beside v2's (a hybrid mount): v2 is read.
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpuacct/cpuacct.usage", "5000\n"));
	TEST_EXPECT(ps::read_cgroup_cpu_usec(usec, dir.root()) && usec == 81234567);
	std::printf("  cgroup v2: usage_usec, memory.current less inactive_file, memory.max\n");
	return 0;
}

int exercise_cgroup_v1() {
	test_temp::TempDir dir("proc_stats_v1");
	TEST_EXPECT(put(dir, "sys/fs/cgroup/cpu,cpuacct/cpuacct.usage", "81234567890\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory/memory.usage_in_bytes", "524288000\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory/memory.limit_in_bytes", "9223372036854771712\n"));
	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory/memory.stat",
			"cache 104857600\nrss 419430400\ninactive_file 1\ntotal_inactive_file 24288000\n"));
	uint64_t usec = 0;
	TEST_EXPECT(ps::read_cgroup_cpu_usec(usec, dir.root()) && usec == 81234567);
	ps::CgroupMemory memory;
	TEST_EXPECT(ps::read_cgroup_memory(memory, dir.root()));
	TEST_EXPECT(memory.current_bytes == 500000000 && memory.limit_bytes == 0);

	TEST_EXPECT(put(dir, "sys/fs/cgroup/memory/memory.limit_in_bytes", "1073741824\n"));
	TEST_EXPECT(ps::read_cgroup_memory(memory, dir.root()) && memory.limit_bytes == 1073741824);

	// The controller under its own name.
	test_temp::TempDir split("proc_stats_v1_split");
	TEST_EXPECT(put(split, "sys/fs/cgroup/cpuacct/cpuacct.usage", "2000000\n"));
	TEST_EXPECT(ps::read_cgroup_cpu_usec(usec, split.root()) && usec == 2000);

	// Nothing there: refused.
	test_temp::TempDir empty("proc_stats_cg_none");
	TEST_EXPECT(!ps::read_cgroup_cpu_usec(usec, empty.root()));
	TEST_EXPECT(!ps::read_cgroup_memory(memory, empty.root()));
	std::printf("  cgroup v1: cpuacct.usage in ns, the no-limit value, total_inactive_file\n");
	return 0;
}

int exercise_disk() {
	test_temp::TempDir dir("proc_stats_disk");
	ps::DiskSpace disk;
#ifdef _WIN32
	TEST_EXPECT(!ps::read_disk(dir.root(), disk));
#else
	TEST_EXPECT(ps::read_disk(dir.root(), disk));
	TEST_EXPECT(disk.total_bytes > 0 && disk.free_bytes <= disk.total_bytes);
	TEST_EXPECT(!ps::read_disk(dir.root() + "/missing", disk));
#endif
	std::printf("  statvfs: a filesystem's free of total\n");
	return 0;
}

} // namespace

int main() {
	int rc = exercise_host_cpu();
	if (rc == 0)
		rc = exercise_host_memory();
	if (rc == 0)
		rc = exercise_net_dev();
	if (rc == 0)
		rc = exercise_cgroup_v2();
	if (rc == 0)
		rc = exercise_cgroup_v1();
	if (rc == 0)
		rc = exercise_disk();
	if (rc == 0)
		std::printf("OK: a machine's and a container's use\n");
	return rc;
}
