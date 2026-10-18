#pragma once

// What a Linux machine or container uses, read from the kernel's own files: the
// host's CPU (/proc/stat), memory (/proc/meminfo), network (/proc/net/dev) and a
// filesystem's room (statvfs), and a cgroup's CPU time and memory (cgroup v2's
// unified files, else v1's controllers). hostd reads its machine's (the
// fleet_machines stats it reports); each program reads its own task's (its
// program_samples rows). Every read but read_disk takes the root the kernel's
// files are under: "" on a machine, a fixture tree in the tests. No service
// dependency (hostd compiles it in). Platform plumbing, not a port.

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::proc_stats {

// A CPU counter pair: the time spent busy and the time it is measured against,
// both growing, in one unit. A percent is between two readings (cpu_percent).
struct CpuTimes {
	uint64_t busy = 0;
	uint64_t total = 0;
};

// The host's CPU since boot, from /proc/stat's aggregate `cpu` line in clock
// ticks, every CPU's summed: total = user + nice + system + idle + iowait + irq
// + softirq + steal (guest time is already in user and nice), busy = total -
// idle - iowait. False when the file does not read.
bool read_host_cpu(CpuTimes &out, const std::string &root = "");

// The share of `ncpu` CPUs busy between two readings, 100 = all of them busy:
// 100 * busy's growth / (total's growth * ncpu). A host reading's total already
// spans every CPU, so its `ncpu` is 1; a cgroup's usage paired with a wall
// clock (both in microseconds) passes the CPUs its percent is of. 0 when total
// did not grow, either counter went back (a reset) or `ncpu` is not positive.
double cpu_percent(const CpuTimes &prev, const CpuTimes &now, double ncpu = 1.0);

// Memory in bytes: what is used of what there is.
struct Memory {
	uint64_t used_bytes = 0;
	uint64_t total_bytes = 0;
};

// The host's memory from /proc/meminfo: total = MemTotal, used = MemTotal -
// MemAvailable (what could not be had without swapping). False when the file
// does not read or lacks either line.
bool read_host_memory(Memory &out, const std::string &root = "");

// A network's bytes since its interfaces came up.
struct NetBytes {
	uint64_t in_bytes = 0;
	uint64_t out_bytes = 0;
};

// Every interface's received and sent bytes in /proc/net/dev summed, but
// loopback's (lo): the reader's network namespace, so a host-network process
// (hostd) reads the machine's and a task's container its task's. False when the
// file does not read or names no interface.
bool read_net_dev(NetBytes &out, const std::string &root = "");

// A filesystem's room in bytes.
struct DiskSpace {
	uint64_t free_bytes = 0; // what an unprivileged writer can still take (f_bavail)
	uint64_t total_bytes = 0;
};

// statvfs of the filesystem `path` is on. False when it does not answer.
bool read_disk(const std::string &path, DiskSpace &out);

// The reader's own cgroup's CPU time in microseconds: v2's cpu.stat usage_usec
// under <root>/sys/fs/cgroup, else v1's cpuacct.usage (nanoseconds) under
// <root>/sys/fs/cgroup/cpuacct or cpu,cpuacct. False when neither reads.
bool read_cgroup_cpu_usec(uint64_t &out, const std::string &root = "");

// A cgroup's memory in bytes, counted as `docker stats` counts it: the usage
// less the inactive file cache (memory.stat's inactive_file; v1's
// total_inactive_file).
struct CgroupMemory {
	uint64_t current_bytes = 0;
	uint64_t limit_bytes = 0; // 0: no limit
};

// The reader's own cgroup's memory: v2's memory.current and memory.max under
// <root>/sys/fs/cgroup, else v1's memory.usage_in_bytes and
// memory.limit_in_bytes under <root>/sys/fs/cgroup/memory (v1's "no limit", a
// page-rounded INT64_MAX, reads as 0). False when neither reads.
bool read_cgroup_memory(CgroupMemory &out, const std::string &root = "");

} // namespace opennova::novaworld_server::proc_stats
