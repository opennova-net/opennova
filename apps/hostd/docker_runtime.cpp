#include "docker_runtime.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace opennova::hostd {

namespace {

using io::JsonValue;
namespace fs = std::filesystem;

// The first line of a CLI's standard error, at most 200 bytes.
std::string first_line(const std::string &text) {
	std::string err = strutil::trim(text);
	if (const size_t eol = err.find('\n'); eol != std::string::npos)
		err.resize(eol);
	if (err.size() > 200)
		err.resize(200);
	return err;
}

// "docker <what> exited 1: <the start of its standard error>", for the log.
std::string failure(const char *what, const proc::CommandResult &result) {
	std::string out = std::string("docker ") + what;
	switch (result.outcome) {
		case proc::CommandResult::Outcome::NotStarted:
			return out + " did not start: " + result.error;
		case proc::CommandResult::Outcome::TimedOut:
			return out + " timed out";
		case proc::CommandResult::Outcome::Exited:
			break;
	}
	out += " exited " + std::to_string(result.exit_code);
	const std::string err = first_line(result.err);
	if (!err.empty())
		out += ": " + err;
	return out;
}

uint64_t steady_now_ms() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
					.count());
}

// docker's word for an image it does not hold ("Error: No such image: x",
// "Error response from daemon: No such image: x"), in any case.
bool says_no_such_image(const std::string &err) {
	std::string lower = err;
	std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lower.find("no such image") != std::string::npos;
}

std::vector<std::string> lines_of(std::string_view text) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string_view::npos)
			end = text.size();
		std::string line = strutil::trim(text.substr(start, end - start));
		if (!line.empty())
			out.push_back(std::move(line));
		start = end + 1;
	}
	return out;
}

std::string format_cpus(double cpus) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%g", cpus);
	return buffer;
}

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's days_from_civil).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

std::optional<unsigned> digits(std::string_view text, size_t at, size_t count) {
	if (at + count > text.size())
		return std::nullopt;
	unsigned value = 0;
	for (size_t i = at; i < at + count; ++i) {
		if (text[i] < '0' || text[i] > '9')
			return std::nullopt;
		value = value * 10 + static_cast<unsigned>(text[i] - '0');
	}
	return value;
}

} // namespace

DockerRuntime::DockerRuntime(proc::CommandRunner &runner, std::string docker,
		std::vector<std::string> environment, std::string scratch_dir) :
		runner_(runner),
		docker_(std::move(docker)),
		environment_(std::move(environment)),
		scratch_dir_(std::move(scratch_dir)) {}

DockerRuntime::~DockerRuntime() {
	for (auto &pull : pulls_) {
		pull.second.child.kill();
		pull.second.child.wait(5000);
		take_error(pull.second.error_path);
	}
}

bool DockerRuntime::list(std::vector<ContainerInfo> &out, int timeout_ms, std::string &error) {
	// The two calls share the one timeout.
	const uint64_t started = steady_now_ms();
	const proc::CommandResult ps = runner_.run(
			{ docker_, "ps", "-a", "--no-trunc", "--filter",
					std::string("label=") + kManagedLabel + "=1", "--format", "{{.Names}}" },
			timeout_ms);
	if (!ps.ok()) {
		error = failure("ps", ps);
		return false;
	}
	const std::vector<std::string> names = lines_of(ps.out);
	out.clear();
	if (names.empty())
		return true;
	const uint64_t spent = steady_now_ms() - started;
	if (spent >= static_cast<uint64_t>(timeout_ms)) {
		error = "docker ps took the listing's whole time";
		return false;
	}
	std::vector<std::string> argv = { docker_, "inspect", "--type", "container" };
	argv.insert(argv.end(), names.begin(), names.end());
	const proc::CommandResult inspect = runner_.run(argv, timeout_ms - static_cast<int>(spent));
	// A container removed between the two calls fails the inspect (exit 1) and
	// leaves the others' entries; anything else unread is an error.
	std::string why;
	if (inspect.outcome == proc::CommandResult::Outcome::Exited &&
			parse_docker_inspect(inspect.out, out, why))
		return true;
	error = inspect.ok() ? "docker inspect: " + why : failure("inspect", inspect);
	return false;
}

bool DockerRuntime::stats(const std::vector<std::string> &names,
		std::map<std::string, ContainerStats> &out, int timeout_ms, std::string &error) {
	out.clear();
	if (names.empty())
		return true;
	std::vector<std::string> argv = { docker_, "stats", "--no-stream", "--no-trunc", "--format",
		"{{json .}}" };
	argv.insert(argv.end(), names.begin(), names.end());
	const proc::CommandResult result = runner_.run(argv, timeout_ms);
	if (result.outcome != proc::CommandResult::Outcome::Exited) {
		error = failure("stats", result);
		return false;
	}
	// A container gone in between fails the call and leaves the others' lines.
	parse_docker_stats(result.out, out);
	if (!result.ok() && out.empty()) {
		error = failure("stats", result);
		return false;
	}
	return true;
}

bool DockerRuntime::has_image(
		const std::string &image, bool &present, int timeout_ms, std::string &error) {
	const proc::CommandResult result = runner_.run(
			{ docker_, "image", "inspect", "--format", "{{.Id}}", image }, timeout_ms);
	if (result.ok()) {
		present = true;
		return true;
	}
	if (result.outcome == proc::CommandResult::Outcome::Exited && result.exit_code == 1 &&
			says_no_such_image(result.err)) {
		present = false;
		return true;
	}
	error = failure("image inspect", result);
	return false;
}

bool DockerRuntime::start_pull(const std::string &image, std::string &error) {
	if (pulls_.count(image))
		return true;
	Pull pull;
	pull.error_path =
			(fs::u8path(scratch_dir_) / ("pull-" + std::to_string(++pulls_started_) + ".err"))
					.u8string();
	std::error_code ec;
	fs::remove(fs::u8path(pull.error_path), ec);
	proc::SpawnOptions options;
	options.argv = { docker_, "pull", "-q", image };
	options.error_path = pull.error_path;
	options.environment = environment_;
	// The agent's one thread starts it, and lives as long as it should.
	options.end_with_parent = true;
	std::string why;
	if (!pull.child.spawn(options, why)) {
		error = "docker pull did not start: " + why;
		take_error(pull.error_path);
		return false;
	}
	pulls_.emplace(image, std::move(pull));
	return true;
}

PullState DockerRuntime::poll_pull(const std::string &image, std::string &error) {
	const auto it = pulls_.find(image);
	if (it == pulls_.end()) {
		error = "no pull of it is under way";
		return PullState::Failed;
	}
	const std::optional<uint32_t> code = it->second.child.poll();
	if (!code)
		return PullState::Running;
	const std::string err = take_error(it->second.error_path);
	pulls_.erase(it);
	if (*code == 0)
		return PullState::Done;
	error = "docker pull exited " + std::to_string(*code) + (err.empty() ? "" : ": " + err);
	return PullState::Failed;
}

void DockerRuntime::cancel_pull(const std::string &image) {
	const auto it = pulls_.find(image);
	if (it == pulls_.end())
		return;
	it->second.child.kill();
	it->second.child.wait(5000);
	take_error(it->second.error_path);
	pulls_.erase(it);
}

std::string DockerRuntime::take_error(const std::string &path) {
	if (path.empty())
		return {};
	const fs::path file = fs::u8path(path);
	std::string text;
	{
		std::ifstream in(file, std::ios::binary);
		std::ostringstream read;
		read << in.rdbuf();
		text = read.str();
	}
	std::error_code ec;
	fs::remove(file, ec);
	return first_line(text);
}

std::vector<std::string> DockerRuntime::run_argv(const ContainerSpec &spec) const {
	const std::string user = std::to_string(spec.uid) + ":" + std::to_string(spec.uid);
	const std::string memory = std::to_string(spec.memory_mb) + "m";
	std::vector<std::string> argv = { docker_, "run", "--pull", "never", "-d", "--name", spec.name,
		"--label", std::string(kManagedLabel) + "=1", "--label",
		std::string(kSlotLabel) + "=" + std::to_string(spec.slot), "--network", "host", "--user",
		user, "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--read-only", "--tmpfs",
		"/tmp", "--cpus", format_cpus(spec.cpus), "--memory", memory, "--memory-swap", memory,
		"--pids-limit", std::to_string(spec.pids), "--stop-timeout",
		std::to_string(kStopTimeoutSeconds), "--log-driver", "journald", "--log-opt",
		"tag=" + spec.name, "--mount",
		"type=bind,source=" + spec.data_dir + ",target=/opt/game,readonly", "--mount",
		"type=bind,source=" + spec.work_dir + ",target=/srv/serve" };
	if (!spec.env_file.empty()) {
		argv.push_back("--env-file");
		argv.push_back(spec.env_file);
	}
	argv.push_back(spec.image);
	argv.insert(argv.end(), spec.args.begin(), spec.args.end());
	return argv;
}

bool DockerRuntime::run(
		const ContainerSpec &spec, int timeout_ms, int &exit_code, std::string &error) {
	const proc::CommandResult result = runner_.run(run_argv(spec), timeout_ms);
	if (result.ok())
		return true;
	// docker run's own codes: 125 the daemon refused, 126 and 127 the entry
	// point; a run that never ran or was cut off is the daemon's failure too.
	exit_code = result.outcome == proc::CommandResult::Outcome::Exited
			? static_cast<int>(result.exit_code)
			: 125;
	error = failure("run", result);
	return false;
}

bool DockerRuntime::simple(
		const char *what, std::vector<std::string> argv, int timeout_ms, std::string &error) {
	const proc::CommandResult result = runner_.run(argv, timeout_ms);
	if (result.ok())
		return true;
	error = failure(what, result);
	return false;
}

bool DockerRuntime::stop(const std::string &name, int timeout_ms, std::string &error) {
	return simple("kill", { docker_, "kill", "--signal", "SIGTERM", name }, timeout_ms, error);
}

bool DockerRuntime::kill(const std::string &name, int timeout_ms, std::string &error) {
	return simple("kill", { docker_, "kill", "--signal", "SIGKILL", name }, timeout_ms, error);
}

bool DockerRuntime::remove(const std::string &name, int timeout_ms, std::string &error) {
	return simple("rm", { docker_, "rm", name }, timeout_ms, error);
}

bool parse_docker_inspect(
		std::string_view json, std::vector<ContainerInfo> &out, std::string &error) {
	JsonValue v;
	if (!io::json_parse(json, v, error))
		return false;
	if (!v.is_array()) {
		error = "not a JSON array";
		return false;
	}
	out.clear();
	for (const JsonValue &entry : v.array) {
		const JsonValue *config = entry.get("Config");
		const JsonValue *labels = config ? config->get("Labels") : nullptr;
		const JsonValue *state = entry.get("State");
		if (!labels || !state || labels->get_string(DockerRuntime::kManagedLabel, "") != "1")
			continue;
		ContainerInfo info;
		info.name = entry.get_string("Name", "");
		if (!info.name.empty() && info.name[0] == '/')
			info.name.erase(0, 1);
		if (!hosted::is_container_name(info.name))
			continue;
		const std::string slot = labels->get_string(DockerRuntime::kSlotLabel, "");
		if (strutil::all_digits(slot) && slot.size() <= 2) {
			const int k = strutil::parse_int(slot).value_or(-1);
			if (hosted::slot_valid(k))
				info.slot = k;
		}
		const std::string status = state->get_string("Status", "");
		if (status == "running" || status == "paused") {
			info.state = hosted::ContainerState::Running;
		} else if (status == "exited" || status == "dead" || status == "removing") {
			info.state = hosted::ContainerState::Exited;
			int64_t code = 0;
			const JsonValue *exit = state->get("ExitCode");
			info.exit_code =
					exit && io::json_exact_whole_in(*exit, -2147483648.0, 2147483647.0, code) ? code
																							  : -1;
			info.oom = state->get_bool("OOMKilled", false);
		} else {
			info.state = hosted::ContainerState::Starting;
		}
		info.started_at = parse_docker_time(state->get_string("StartedAt", ""));
		out.push_back(std::move(info));
	}
	return true;
}

void parse_docker_stats(std::string_view lines, std::map<std::string, ContainerStats> &out) {
	for (const std::string &line : lines_of(lines)) {
		JsonValue v;
		std::string error;
		if (!io::json_parse(line, v, error) || !v.is_object())
			continue;
		const std::string name = v.get_string("Name", "");
		if (name.empty())
			continue;
		ContainerStats stats;
		std::string cpu = strutil::trim(v.get_string("CPUPerc", ""));
		if (!cpu.empty() && cpu.back() == '%')
			cpu.pop_back();
		if (const auto percent = strutil::parse_double(cpu);
				percent && std::isfinite(*percent) && *percent >= 0.0)
			stats.cpu_percent = *percent;
		const std::string usage = v.get_string("MemUsage", "");
		if (const auto bytes = parse_docker_size(strutil::trim(usage.substr(0, usage.find('/')))))
			stats.memory_bytes = *bytes;
		out[name] = stats;
	}
}

std::optional<int64_t> parse_docker_time(std::string_view text) {
	// YYYY-MM-DDTHH:MM:SS, then an optional fraction and the zone (docker's is Z).
	if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
			text[13] != ':' || text[16] != ':')
		return std::nullopt;
	const auto year = digits(text, 0, 4), month = digits(text, 5, 2), day = digits(text, 8, 2);
	const auto hour = digits(text, 11, 2), minute = digits(text, 14, 2),
			   second = digits(text, 17, 2);
	if (!year || !month || !day || !hour || !minute || !second)
		return std::nullopt;
	if (*year < 1970 || *month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 ||
			*minute > 59 || *second > 60)
		return std::nullopt;
	size_t at = 19;
	if (text[at] == '.') {
		++at;
		while (at < text.size() && text[at] >= '0' && text[at] <= '9')
			++at;
	}
	if (text.substr(at) != "Z" && text.substr(at) != "+00:00")
		return std::nullopt;
	return days_from_civil(*year, *month, *day) * 86400 + int64_t(*hour) * 3600 +
			int64_t(*minute) * 60 + int64_t(*second);
}

std::optional<uint64_t> parse_docker_size(std::string_view text) {
	size_t unit_at = 0;
	while (unit_at < text.size() &&
			((text[unit_at] >= '0' && text[unit_at] <= '9') || text[unit_at] == '.'))
		++unit_at;
	if (unit_at == 0)
		return std::nullopt;
	const auto number = strutil::parse_double(std::string(text.substr(0, unit_at)));
	if (!number || !std::isfinite(*number) || *number < 0.0)
		return std::nullopt;
	struct Unit {
		const char *name;
		double bytes;
	};
	static constexpr Unit kUnits[] = {
		{ "B", 1.0 },
		{ "kB", 1e3 },
		{ "KB", 1e3 },
		{ "MB", 1e6 },
		{ "GB", 1e9 },
		{ "TB", 1e12 },
		{ "KiB", 1024.0 },
		{ "MiB", 1048576.0 },
		{ "GiB", 1073741824.0 },
		{ "TiB", 1099511627776.0 },
	};
	const std::string_view unit = text.substr(unit_at);
	for (const Unit &u : kUnits) {
		if (unit == u.name) {
			const double bytes = std::round(*number * u.bytes);
			if (bytes > 1.8e19)
				return std::nullopt;
			return static_cast<uint64_t>(bytes);
		}
	}
	return std::nullopt;
}

} // namespace opennova::hostd
