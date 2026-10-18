#include "proc_stats.h"

#include <base/io/file_io.h>
#include <base/io/strutil.h>

#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/statvfs.h>
#endif

namespace opennova::novaworld_server::proc_stats {

namespace {

// v1's "no limit" is INT64_MAX rounded down to a page; anything this large is one.
constexpr uint64_t kNoLimitFloor = uint64_t(1) << 62;

bool read_text(const std::string &path, std::string &out) {
	std::string error;
	return io::read_file_text(path, out, error);
}

std::vector<std::string_view> words_of(std::string_view line) {
	std::vector<std::string_view> out;
	size_t at = 0;
	while (at < line.size()) {
		while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
			++at;
		const size_t start = at;
		while (at < line.size() && line[at] != ' ' && line[at] != '\t')
			++at;
		if (at > start)
			out.push_back(line.substr(start, at - start));
	}
	return out;
}

std::vector<std::string_view> lines_of(std::string_view text) {
	std::vector<std::string_view> out;
	size_t at = 0;
	while (at < text.size()) {
		size_t eol = text.find('\n', at);
		if (eol == std::string_view::npos)
			eol = text.size();
		out.push_back(text.substr(at, eol - at));
		at = eol + 1;
	}
	return out;
}

bool number(std::string_view word, uint64_t &out) {
	if (word.empty() || !strutil::all_digits(word))
		return false;
	const auto v = strutil::parse_ullong(std::string(word));
	if (!v)
		return false;
	out = *v;
	return true;
}

// The number after `key` on the line it opens ("usage_usec 123", "MemTotal: 16 kB").
bool keyed(std::string_view text, std::string_view key, uint64_t &out) {
	for (const std::string_view line : lines_of(text)) {
		const std::vector<std::string_view> words = words_of(line);
		if (words.size() >= 2 && words[0] == key)
			return number(words[1], out);
	}
	return false;
}

// A file holding one number ("123\n").
bool file_number(const std::string &path, uint64_t &out) {
	std::string text;
	return read_text(path, text) && number(strutil::trim_view(text), out);
}

} // namespace

bool read_host_cpu(CpuTimes &out, const std::string &root) {
	std::string text;
	if (!read_text(root + "/proc/stat", text))
		return false;
	for (const std::string_view line : lines_of(text)) {
		const std::vector<std::string_view> words = words_of(line);
		if (words.empty() || words[0] != "cpu")
			continue;
		// user nice system idle iowait irq softirq steal [guest guest_nice]
		uint64_t field[8] = {};
		if (words.size() < 5)
			return false;
		for (size_t i = 0; i < 8 && i + 1 < words.size(); ++i)
			if (!number(words[i + 1], field[i]))
				return false;
		uint64_t total = 0;
		for (const uint64_t v : field)
			total += v;
		const uint64_t idle = field[3] + field[4];
		out.total = total;
		out.busy = total - idle;
		return true;
	}
	return false;
}

double cpu_percent(const CpuTimes &prev, const CpuTimes &now, double ncpu) {
	if (ncpu <= 0 || now.total <= prev.total || now.busy < prev.busy)
		return 0;
	const double busy = static_cast<double>(now.busy - prev.busy);
	const double total = static_cast<double>(now.total - prev.total);
	return 100.0 * busy / (total * ncpu);
}

bool read_host_memory(Memory &out, const std::string &root) {
	std::string text;
	uint64_t total_kb = 0;
	uint64_t available_kb = 0;
	if (!read_text(root + "/proc/meminfo", text) || !keyed(text, "MemTotal:", total_kb) ||
			!keyed(text, "MemAvailable:", available_kb))
		return false;
	out.total_bytes = total_kb * 1024;
	out.used_bytes = available_kb < total_kb ? (total_kb - available_kb) * 1024 : 0;
	return true;
}

bool read_net_dev(NetBytes &out, const std::string &root) {
	std::string text;
	if (!read_text(root + "/proc/net/dev", text))
		return false;
	// "  eth0: <rx bytes> packets errs drop fifo frame compressed multicast <tx bytes> ..."
	NetBytes sum;
	bool any = false;
	for (const std::string_view line : lines_of(text)) {
		const size_t colon = line.find(':');
		if (colon == std::string_view::npos)
			continue;
		const std::string_view name = strutil::trim_view(line.substr(0, colon));
		const std::vector<std::string_view> words = words_of(line.substr(colon + 1));
		uint64_t in = 0;
		uint64_t sent = 0;
		if (name.empty() || words.size() < 9 || !number(words[0], in) || !number(words[8], sent))
			continue;
		any = true;
		if (name == "lo")
			continue;
		sum.in_bytes += in;
		sum.out_bytes += sent;
	}
	if (!any)
		return false;
	out = sum;
	return true;
}

bool read_disk(const std::string &path, DiskSpace &out) {
#ifdef _WIN32
	(void)path;
	(void)out;
	return false;
#else
	struct statvfs vfs{};
	if (::statvfs(path.c_str(), &vfs) != 0)
		return false;
	out.free_bytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
	out.total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
	return true;
#endif
}

bool read_cgroup_cpu_usec(uint64_t &out, const std::string &root) {
	const std::string cgroup = root + "/sys/fs/cgroup";
	std::string text;
	if (read_text(cgroup + "/cpu.stat", text) && keyed(text, "usage_usec", out))
		return true;
	for (const char *controller : { "/cpuacct", "/cpu,cpuacct" }) {
		uint64_t nsec = 0;
		if (file_number(cgroup + controller + "/cpuacct.usage", nsec)) {
			out = nsec / 1000;
			return true;
		}
	}
	return false;
}

bool read_cgroup_memory(CgroupMemory &out, const std::string &root) {
	const std::string cgroup = root + "/sys/fs/cgroup";
	std::string stat;
	uint64_t usage = 0;
	uint64_t inactive = 0;
	uint64_t limit = 0;
	if (file_number(cgroup + "/memory.current", usage)) {
		std::string max;
		if (!read_text(cgroup + "/memory.max", max))
			return false;
		if (strutil::trim_view(max) == "max")
			limit = 0;
		else if (!number(strutil::trim_view(max), limit))
			return false;
		if (read_text(cgroup + "/memory.stat", stat))
			keyed(stat, "inactive_file", inactive);
	} else {
		const std::string memory = cgroup + "/memory";
		if (!file_number(memory + "/memory.usage_in_bytes", usage) ||
				!file_number(memory + "/memory.limit_in_bytes", limit))
			return false;
		if (limit >= kNoLimitFloor)
			limit = 0;
		if (read_text(memory + "/memory.stat", stat))
			keyed(stat, "total_inactive_file", inactive);
	}
	out.current_bytes = inactive < usage ? usage - inactive : 0;
	out.limit_bytes = limit;
	return true;
}

} // namespace opennova::novaworld_server::proc_stats
