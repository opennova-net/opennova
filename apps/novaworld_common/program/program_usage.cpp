#include "program_usage.h"

#include "core/ecs_metadata.h"
#include "core/proc_stats.h"
#include "core/text_util.h"

#include <base/io/file_io.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <thread>
#include <utility>
#include <variant>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace opennova::novaworld_server::program {

namespace {

using db::BindValue;

BindValue double_or_null(const std::optional<double> &value) {
	return value ? BindValue(*value) : BindValue(std::monostate{});
}

BindValue int_or_null(const std::optional<int64_t> &value) {
	return value ? BindValue(*value) : BindValue(std::monostate{});
}

bool read_text(const std::string &path, std::string &out) {
	std::string error;
	return io::read_file_text(path, out, error);
}

// A quota over a period ("50000 100000") as CPUs; 0 for none ("max ...") or
// text that does not read.
double quota_over_period(std::string_view quota, std::string_view period) {
	const auto q = strutil::parse_llong(std::string(strutil::trim_view(quota)));
	const auto p = strutil::parse_llong(std::string(strutil::trim_view(period)));
	if (!q || !p || *q <= 0 || *p <= 0)
		return 0;
	return static_cast<double>(*q) / static_cast<double>(*p);
}

// `text` as a task id: its letters, digits, '.', '_' and '-', at most 64 of
// them.
std::string id_chars(std::string_view text) {
	std::string out;
	for (const char c : text)
		if (out.size() < 64 && (is_alnum(c) || c == '.' || c == '_' || c == '-'))
			out.push_back(c);
	return out;
}

} // namespace

std::string local_task_id() {
	std::string name;
#ifndef _WIN32
	char buf[256] = {};
	if (::gethostname(buf, sizeof(buf) - 1) == 0)
		name = buf;
#endif
	const std::string id = id_chars(name);
	return id.empty() ? std::string("local") : id;
}

double read_cgroup_cpu_limit(const std::string &root) {
	const std::string cgroup = root + "/sys/fs/cgroup";
	std::string text;
	if (read_text(cgroup + "/cpu.max", text)) {
		const std::string_view line = strutil::trim_view(text);
		const size_t space = line.find(' ');
		return space == std::string_view::npos
				? 0
				: quota_over_period(line.substr(0, space), line.substr(space + 1));
	}
	for (const char *controller : { "/cpu", "/cpu,cpuacct" }) {
		std::string quota;
		std::string period;
		if (read_text(cgroup + controller + "/cpu.cfs_quota_us", quota) &&
				read_text(cgroup + controller + "/cpu.cfs_period_us", period))
			return quota_over_period(quota, period);
	}
	return 0;
}

UsageTask find_usage_task(const std::string &root) {
	UsageTask task;
	ecs::TaskFacts facts;
	if (const std::string uri = ecs::metadata_uri_from_env(); !uri.empty()) {
		std::string error;
		if (!ecs::read_task_facts(uri, facts, error)) {
			std::fprintf(stderr, "[usage] WARN the task's metadata: %s; sampling as %s\n",
					error.c_str(), local_task_id().c_str());
			facts = ecs::TaskFacts();
		}
	}
	task.id = id_chars(facts.id);
	if (task.id.empty())
		task.id = local_task_id();
	const double quota = read_cgroup_cpu_limit(root);
	if (quota > 0)
		task.cpu_limit = quota;
	else if (facts.cpu > 0)
		task.cpu_limit = facts.cpu;
	else
		task.cpu_limit = static_cast<double>(std::max(1u, std::thread::hardware_concurrency()));
	task.memory_limit_bytes = static_cast<uint64_t>(facts.memory_mib) * 1024 * 1024;
	return task;
}

UsageSampler::UsageSampler(std::string program, UsageTask task, std::string root) :
		program_(std::move(program)), task_(std::move(task)), root_(std::move(root)) {}

UsageSample UsageSampler::take(int64_t now_s, uint64_t steady_us) {
	UsageSample s;
	s.program = program_;
	s.task_id = task_.id;
	s.at = now_s - now_s % 60;
	if (task_.cpu_limit > 0)
		s.cpu_limit = task_.cpu_limit;

	uint64_t cpu_usec = 0;
	if (proc_stats::read_cgroup_cpu_usec(cpu_usec, root_)) {
		if (have_prev_ && task_.cpu_limit > 0 && steady_us > prev_steady_us_)
			s.cpu_percent = proc_stats::cpu_percent(
					{ prev_cpu_usec_, prev_steady_us_ }, { cpu_usec, steady_us }, task_.cpu_limit);
		have_prev_ = true;
		prev_cpu_usec_ = cpu_usec;
		prev_steady_us_ = steady_us;
	} else {
		have_prev_ = false;
	}

	proc_stats::CgroupMemory memory;
	if (proc_stats::read_cgroup_memory(memory, root_)) {
		s.memory_used_bytes = static_cast<int64_t>(memory.current_bytes);
		uint64_t limit = memory.limit_bytes != 0 ? memory.limit_bytes : task_.memory_limit_bytes;
		proc_stats::Memory host;
		if (limit == 0 && proc_stats::read_host_memory(host, root_))
			limit = host.total_bytes;
		if (limit != 0)
			s.memory_limit_bytes = static_cast<int64_t>(limit);
	}

	proc_stats::NetBytes net;
	if (proc_stats::read_net_dev(net, root_)) {
		s.net_in_bytes = static_cast<int64_t>(net.in_bytes);
		s.net_out_bytes = static_cast<int64_t>(net.out_bytes);
	}
	return s;
}

void insert_usage_sample(db::Database &db, const UsageSample &s) {
	db.exec("INSERT INTO program_samples (program, task_id, at, cpu_percent, cpu_limit, "
			"memory_used_bytes, memory_limit_bytes, net_in_bytes, net_out_bytes) "
			"VALUES ($1, $2, to_timestamp($3::double precision), $4, $5, $6, $7, $8, $9) "
			"ON CONFLICT (program, task_id, at) DO NOTHING;",
			{ BindValue(s.program), BindValue(s.task_id), BindValue(s.at),
					double_or_null(s.cpu_percent), double_or_null(s.cpu_limit),
					int_or_null(s.memory_used_bytes), int_or_null(s.memory_limit_bytes),
					int_or_null(s.net_in_bytes), int_or_null(s.net_out_bytes) });
}

void rollup_usage_hours(db::Database &db, int64_t now) {
	// The hours from the first one whose minutes are all still kept. A minute's
	// bytes are its counter's growth since the task's minute before (the one
	// just before the first hour among them); a counter that went back started
	// over, so its count is the minute's.
	db.exec("WITH first AS (SELECT date_trunc('hour', to_timestamp($1::double precision)) + "
			"interval '1 hour' AS hour), "
			"counted AS (SELECT program, task_id, at, cpu_percent, memory_used_bytes, "
			"net_in_bytes, LAG(net_in_bytes) OVER w AS prev_in, "
			"net_out_bytes, LAG(net_out_bytes) OVER w AS prev_out "
			"FROM program_samples WHERE at >= (SELECT hour FROM first) - interval '10 minutes' "
			"WINDOW w AS (PARTITION BY program, task_id ORDER BY at)) "
			"INSERT INTO program_hourly (program, task_id, hour, cpu_avg, cpu_max, "
			"memory_peak_bytes, net_in_bytes, net_out_bytes) "
			"SELECT program, task_id, date_trunc('hour', at), AVG(cpu_percent), MAX(cpu_percent), "
			"MAX(memory_used_bytes), "
			"COALESCE(SUM(CASE WHEN prev_in IS NULL OR net_in_bytes IS NULL THEN 0 "
			"WHEN net_in_bytes >= prev_in THEN net_in_bytes - prev_in ELSE net_in_bytes END), 0), "
			"COALESCE(SUM(CASE WHEN prev_out IS NULL OR net_out_bytes IS NULL THEN 0 "
			"WHEN net_out_bytes >= prev_out THEN net_out_bytes - prev_out ELSE net_out_bytes END), "
			"0) "
			"FROM counted WHERE at >= (SELECT hour FROM first) "
			"GROUP BY program, task_id, date_trunc('hour', at) "
			"ON CONFLICT (program, task_id, hour) DO UPDATE SET cpu_avg = EXCLUDED.cpu_avg, "
			"cpu_max = EXCLUDED.cpu_max, memory_peak_bytes = EXCLUDED.memory_peak_bytes, "
			"net_in_bytes = EXCLUDED.net_in_bytes, net_out_bytes = EXCLUDED.net_out_bytes;",
			{ BindValue(now - kUsageSampleKeepSeconds) });
}

void prune_usage(db::Database &db, int64_t now) {
	db.exec("DELETE FROM program_samples WHERE at < to_timestamp($1::double precision);",
			{ BindValue(now - kUsageSampleKeepSeconds) });
	db.exec("DELETE FROM program_hourly WHERE hour < to_timestamp($1::double precision);",
			{ BindValue(now - kUsageHourlyKeepSeconds) });
}

} // namespace opennova::novaworld_server::program
