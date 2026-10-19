// opennova-hostd: the machine agent. On each machine of the hosted servers'
// fleet it runs the opennova-serve containers the NovaWorld service places on
// the machine's slots and reports them, over the agent protocol
// (apps/novaworld_common/hosted/protocol/agent_protocol.h; agent.h). This file owns the
// command line, the machine's identity checks, the lock on the state
// directory, the fences and their self-test, the log (and its copy under
// --log-dir), what the agent reads of the machine once at its start (its
// image, its own log stream), the stop signals and the pass loop (which keeps
// the alive file fresh for the ECS health check).
// Linux only: on a cloud machine it is the fleet cluster's ECS daemon, its own
// image (opennova-hostd, apps/serve/Dockerfile), root on the machine's network
// (README.md); in the local stack it is the machine container's own process,
// its servers its children (--runtime process).

#include "agent.h"
#include "docker_runtime.h"
#include "fence_probe.h"
#include "hostd_options.h"
#include "instance_metadata.h"
#include "machine_identity.h"
#include "nft_table.h"
#include "process_runtime.h"
#include "slot_files.h"
#include "stamped_output.h"
#include "tree_store.h"

#include "command_runner.h"
#include "core/ecs_metadata.h"
#include "net_sockets.h"

#include <base/io/file_io.h>

#include <base/io/log.h>
#include <base/proc/child_process.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace opennova;

std::atomic<bool> g_stop{ false };
void on_signal(int) {
	g_stop.store(true);
}

bool g_verbose = false;
// --log-dir's copy of the agent's lines (machines/<machine id>/hostd.log).
std::FILE *g_log_copy = nullptr;

// One line per message on stderr (the journal keeps the time), and with
// --log-dir the same line after its time in the copy.
void log_sink(io::LogLevel level, const char *message) {
	if (level == io::LogLevel::kDebug && !g_verbose)
		return;
	const char *tag = level == io::LogLevel::kError ? "error: "
			: level == io::LogLevel::kWarn			? "warning: "
													: "";
	std::fprintf(stderr, "opennova-hostd: %s%s\n", tag, message);
	if (g_log_copy) {
		const auto now = std::chrono::system_clock::now();
		const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
		using std::chrono::milliseconds;
		const int64_t ms =
				std::chrono::duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
		std::tm utc{};
		gmtime_r(&seconds, &utc);
		char at[32];
		std::strftime(at, sizeof(at), "%Y-%m-%dT%H:%M:%S", &utc);
		std::fprintf(g_log_copy, "%s.%03dZ opennova-hostd: %s%s\n", at, static_cast<int>(ms), tag,
				message);
		std::fflush(g_log_copy);
	}
}

bool make_private_dir(const std::string &path) {
	if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
		return false;
	return ::chmod(path.c_str(), 0700) == 0;
}

// The alive file the pass loop keeps fresh (--check-alive).
std::string g_alive;
// The alive file is fresh this long; the loop touches it at every pass and
// every kAliveTouchMs of a wait.
constexpr int64_t kAliveMaxAgeS = 120;
constexpr int kAliveTouchMs = 10 * 1000;

// Waits `ms`, or less when a stop signal comes, the alive file kept fresh.
void wait_ms(int ms) {
	const auto start = std::chrono::steady_clock::now();
	const auto until = start + std::chrono::milliseconds(ms);
	auto touched = start;
	while (!g_stop.load() && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (!g_alive.empty() &&
				std::chrono::steady_clock::now() - touched >=
						std::chrono::milliseconds(kAliveTouchMs)) {
			touched = std::chrono::steady_clock::now();
			opennova::hostd::touch_alive(g_alive);
		}
	}
}

// The state directory's lock, held for the agent's life (its fd left open):
// never two agents on one machine, as a daemon's replacement may start before
// the task it replaces has ended. Waits for it until a stop signal; false then,
// or when the lock file cannot be made.
bool lock_state_dir(const std::string &state_dir) {
	const std::string path = state_dir + "/lock";
	const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		io::logf(io::LogLevel::kError, "cannot open %s", path.c_str());
		return false;
	}
	bool waited = false;
	while (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
		if (errno != EWOULDBLOCK && errno != EINTR) {
			io::logf(io::LogLevel::kError, "cannot lock %s", path.c_str());
			return false;
		}
		if (!waited)
			io::logf(io::LogLevel::kWarn, "another opennova-hostd holds %s: waiting for it to end",
					path.c_str());
		waited = true;
		wait_ms(1000);
		if (g_stop.load())
			return false;
	}
	if (waited)
		io::logf(io::LogLevel::kInfo, "the state directory is this agent's");
	return true;
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
		case ParseOutcome::CheckAlive:
			// The ECS health check: the loop turned within kAliveMaxAgeS.
			return alive_within(alive_path(options.state_dir), kAliveMaxAgeS, std::time(nullptr))
					? 0
					: 1;
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

	// Which machine this is: the user-data's file on a cloud machine, which
	// the ECS agent registered (its instance attribute) must agree with.
	if (!options.machine_file.empty() &&
			!read_machine_file(options.machine_file, options.machine_id, error)) {
		io::logf(io::LogLevel::kError, "%s", error.c_str());
		return 1;
	}
	if (!options.ecs_config.empty()) {
		std::string text;
		std::string registered;
		if (!io::read_file_text(options.ecs_config, text, error) ||
				!ecs_config_machine_id(text, registered, error)) {
			io::logf(io::LogLevel::kError, "the ECS agent's config %s: %s",
					options.ecs_config.c_str(), error.c_str());
			return 1;
		}
		if (registered != std::to_string(options.machine_id)) {
			io::logf(io::LogLevel::kError,
					"machine %lld, but the ECS agent registered machine-id %s (%s): refusing to "
					"run",
					static_cast<long long>(options.machine_id), registered.c_str(),
					options.ecs_config.c_str());
			return 1;
		}
	}

	if (!options.log_dir.empty()) {
		// The local stack's log directory (the machine known by now): this
		// machine's own lines beside its servers' (the process runtime's),
		// readable by the site's log reader (open_log_file).
		const std::string path = (std::filesystem::path(options.log_dir) / "machines" /
				std::to_string(options.machine_id) / "hostd.log")
				.string();
		std::string why;
		const int fd = open_log_file(path, why);
		g_log_copy = fd < 0 ? nullptr : ::fdopen(fd, "a");
		if (!g_log_copy) {
			if (fd >= 0)
				::close(fd);
			std::fprintf(stderr, "opennova-hostd: warning: cannot write %s: %s\n", path.c_str(),
					fd < 0 ? why.c_str() : "fdopen failed");
		}
	}

	const bool root = ::geteuid() == 0;
	if (!root)
		io::logf(io::LogLevel::kWarn, "not running as root: the slots' files stay this user's");
	const std::string scratch = options.state_dir + "/scratch";
	if (!make_private_dir(options.state_dir) || !make_private_dir(scratch)) {
		io::logf(io::LogLevel::kError, "cannot make %s (0700)", scratch.c_str());
		return 1;
	}
	if (!lock_state_dir(options.state_dir))
		return g_stop.load() ? 0 : 1;
	g_alive = alive_path(options.state_dir);
	touch_alive(g_alive);
	SlotFiles slots(options.slots_root, root);
	if (!slots.ensure_root(error)) {
		io::logf(io::LogLevel::kError, "%s", error.c_str());
		return 1;
	}
	// The trees' root, 0755, where it is missing.
	if (::mkdir(options.trees_root.c_str(), 0755) != 0 && errno != EEXIST) {
		io::logf(io::LogLevel::kError, "cannot make %s", options.trees_root.c_str());
		return 1;
	}
	if (net::startup() != 0) {
		io::logf(io::LogLevel::kError, "the socket layer did not start");
		return 1;
	}

	// The CLIs get the system's variables alone (the AWS CLI its own too: a
	// developer's profile and region; the machine's credentials come from its
	// instance role).
	const std::vector<std::string> environment = proc::filter_environment(
			{ "PATH", "HOME", "LANG", "LC_*", "TZ", "TMPDIR", "DOCKER_*", "AWS_*" });
	proc::ProcessCommandRunner runner(environment, scratch);
	std::unique_ptr<ContainerRuntime> runtime;
	if (options.runtime == RuntimeKind::Process) {
		// Each server gets the system's variables (never DOCKER_*) and the env
		// file's; its lines go where the agent's own do.
		runtime = std::make_unique<ProcessRuntime>(options.serve,
				proc::filter_environment({ "PATH", "HOME", "LANG", "LC_*", "TZ", "TMPDIR" }),
				"/proc/self/fd/2", ProcessRuntime::Clock{}, options.log_dir);
	} else {
		runtime = std::make_unique<DockerRuntime>(runner, options.docker, environment, scratch,
				ContainerLogs{ options.log_group, options.log_region });
	}
	// The trees the servers' game data comes in (tree_store.h): fetched from the
	// store on demand, an s3:// one through the AWS CLI, a file:// one copied.
	ProcessTreeFetcher fetcher(options.aws, environment, scratch);
	TreeStore trees(options.trees_root, fetcher);

	// The slots' fences and counters, in place before any server can start,
	// and their self-test: as slot 0's user, nothing only the agent may reach
	// answers (fence_probe.h), or the agent does not run.
	std::string service_host;
	uint16_t service_port = 0;
	service_host_port(options.service_url, service_host, service_port);
	std::unique_ptr<NftTable> table;
	if (options.nft != "none") {
		table = std::make_unique<NftTable>(runner, options.nft, service_port, scratch);
		if (!table->ensure(error)) {
			io::logf(io::LogLevel::kError, "the slots' fences do not apply: %s", error.c_str());
			return 1;
		}
		std::string answered;
		const auto targets = fence_probe_targets(net::resolve_ipv4_all(service_host), service_port);
		if (!probe_as(hosted::slot_uid(0), targets, 2000, answered, error)) {
			io::logf(io::LogLevel::kError, "the fences' self-test did not run: %s", error.c_str());
			return 1;
		}
		if (!answered.empty()) {
			io::logf(io::LogLevel::kError,
					"UID %u still reaches %s: the fences do not hold, refusing to run",
					static_cast<unsigned>(hosted::slot_uid(0)), answered.c_str());
			return 1;
		}
		io::logf(io::LogLevel::kInfo,
				"the slots' users are fenced off the metadata services, the ECS agent and port %u",
				static_cast<unsigned>(service_port));
	}

	HttpServiceLink link(options.service_url);
	AgentConfig config;
	config.machine_id = options.machine_id;
	config.agent_version = hostd_version();
	config.state_dir = options.state_dir;
	config.scratch_dir = scratch;
	config.token_file = options.token_file;
	// On ECS, the image this task runs (its task definition's pinned
	// reference), the envelope's agent_image, and the agent's own log stream.
	if (const std::string uri = novaworld_server::ecs::metadata_uri_from_env(); !uri.empty()) {
		if (!novaworld_server::ecs::read_container_image(uri, config.agent_image, error))
			io::logf(io::LogLevel::kWarn, "its own image does not read: %s", error.c_str());
		std::string stream;
		if (novaworld_server::ecs::read_container_log_stream(uri, stream, error) &&
				hosted::is_log_stream_name(stream))
			config.log_stream = stream;
		else
			io::logf(io::LogLevel::kWarn, "the agent's log stream is not known: %s",
					stream.empty() ? error.c_str() : "its name is not one");
	}
	// A cloud machine's image (the instance metadata, read as root, which no
	// fence holds back), what every report says it runs.
	if (options.runtime == RuntimeKind::Docker) {
		if (read_image_id(kInstanceMetadataUrl, config.image_id, error))
			io::logf(io::LogLevel::kInfo, "the machine's image: %s", config.image_id.c_str());
		else
			io::logf(io::LogLevel::kInfo, "no machine image to report: %s", error.c_str());
	}
	Agent agent(config, link, *runtime, slots, trees, table.get());

	io::logf(io::LogLevel::kInfo, "%s for machine %lld, service %s, %s runtime%s%s",
			hostd_version(), static_cast<long long>(options.machine_id),
			options.service_url.c_str(),
			options.runtime == RuntimeKind::Process ? "process" : "docker",
			config.agent_image.empty() ? "" : ", image ", config.agent_image.c_str());
	if (!options.log_group.empty())
		io::logf(io::LogLevel::kInfo,
				"the containers' output: CloudWatch %s (%s), a stream per start",
				options.log_group.c_str(), options.log_region.c_str());
	if (!options.log_dir.empty())
		io::logf(io::LogLevel::kInfo, "the servers' output and the agent's lines: %s",
				options.log_dir.c_str());
	int code = 0;
	while (!g_stop.load()) {
		touch_alive(g_alive);
		const int delay = agent.step();
		if (options.once) {
			code = agent.last_sync_ok() ? 0 : 1;
			break;
		}
		wait_ms(delay);
	}
	if (g_stop.load())
		io::logf(io::LogLevel::kInfo,
				options.runtime == RuntimeKind::Process ? "stopping; the servers end with the agent"
														: "stopping; the containers keep running");
	net::shutdown();
	if (g_log_copy)
		std::fclose(g_log_copy);
	return code;
}
