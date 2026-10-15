// opennova-hostd: the machine agent. On each machine of the hosted servers'
// fleet it runs the opennova-serve containers the NovaWorld service places on
// the machine's slots and reports them, over the agent protocol
// (apps/novaworld_server/hosted/agent_protocol.h; agent.h). This file owns the
// command line, the log, the stop signals and the pass loop. Linux only: it
// runs as root under systemd, shipped inside the opennova-serve image and
// copied out at the machine's bootstrap (README.md).

#include "agent.h"
#include "docker_runtime.h"
#include "egress_counters.h"
#include "hostd_options.h"
#include "slot_files.h"

#include "command_runner.h"
#include "net_sockets.h"

#include <base/io/log.h>
#include <base/proc/child_process.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace opennova;

std::atomic<bool> g_stop{ false };
void on_signal(int) {
	g_stop.store(true);
}

bool g_verbose = false;

// One line per message on stderr (the journal keeps the time).
void log_sink(io::LogLevel level, const char *message) {
	if (level == io::LogLevel::kDebug && !g_verbose)
		return;
	const char *tag = level == io::LogLevel::kError ? "error: "
			: level == io::LogLevel::kWarn			? "warning: "
													: "";
	std::fprintf(stderr, "opennova-hostd: %s%s\n", tag, message);
}

bool make_private_dir(const std::string &path) {
	if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
		return false;
	return ::chmod(path.c_str(), 0700) == 0;
}

// Waits `ms`, or less when a stop signal comes.
void wait_ms(int ms) {
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!g_stop.load() && std::chrono::steady_clock::now() < until)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

} // namespace

int main(int argc, char **argv) {
	using namespace opennova::hostd;
	HostdOptions options;
	std::string error;
	switch (parse_hostd_options(std::vector<std::string>(argv + 1, argv + argc), options, error)) {
		case ParseOutcome::Help:
			std::fputs(hostd_usage(), stdout);
			return 0;
		case ParseOutcome::Version:
			std::printf("opennova-hostd %s\n", hostd_version());
			return 0;
		case ParseOutcome::Error:
			std::fprintf(stderr, "opennova-hostd: %s\n%s", error.c_str(), hostd_usage());
			return 2;
		case ParseOutcome::Run:
			break;
	}
	g_verbose = options.verbose;
	io::set_log_sink(log_sink);
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::signal(SIGPIPE, SIG_IGN);

	const bool root = ::geteuid() == 0;
	if (!root)
		io::logf(io::LogLevel::kWarn, "not running as root: the slots' files stay this user's");
	const std::string scratch = options.state_dir + "/scratch";
	if (!make_private_dir(options.state_dir) || !make_private_dir(scratch)) {
		io::logf(io::LogLevel::kError, "cannot make %s (0700)", scratch.c_str());
		return 1;
	}
	SlotFiles slots(options.slots_root, root);
	if (!slots.ensure_root(error)) {
		io::logf(io::LogLevel::kError, "%s", error.c_str());
		return 1;
	}
	if (net::startup() != 0) {
		io::logf(io::LogLevel::kError, "the socket layer did not start");
		return 1;
	}

	// The CLIs get the system's variables alone.
	const std::vector<std::string> environment = proc::filter_environment(
			{ "PATH", "HOME", "LANG", "LC_*", "TZ", "TMPDIR", "DOCKER_*" });
	proc::ProcessCommandRunner runner(environment, scratch);
	DockerRuntime docker(runner, options.docker, environment, scratch);
	std::vector<std::string> tools;
	if (options.iptables != "none")
		tools.push_back(options.iptables);
	if (options.ip6tables != "none" && ::access("/proc/net/if_inet6", F_OK) == 0)
		tools.push_back(options.ip6tables);
	Agent::EgressFactory egress;
	if (!tools.empty())
		egress = [&runner, tools](int slot_count) {
			return std::make_unique<EgressCounters>(runner, tools, slot_count);
		};
	HttpServiceLink link(options.service_url);
	AgentConfig config;
	config.machine_id = options.machine_id;
	config.agent_version = hostd_version();
	config.state_dir = options.state_dir;
	config.scratch_dir = scratch;
	config.token_file = options.token_file;
	Agent agent(config, link, docker, slots, egress);

	io::logf(io::LogLevel::kInfo, "%s for machine %lld, service %s", hostd_version(),
			static_cast<long long>(options.machine_id), options.service_url.c_str());
	int code = 0;
	while (!g_stop.load()) {
		const int delay = agent.step();
		if (options.once) {
			code = agent.last_sync_ok() ? 0 : 1;
			break;
		}
		wait_ms(delay);
	}
	if (g_stop.load())
		io::logf(io::LogLevel::kInfo, "stopping; the containers keep running");
	net::shutdown();
	return code;
}
