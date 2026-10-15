#include "local_machine_driver.h"

#include "net_http.h"
#include "net_sockets.h"
#include "owner_file.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace fs = std::filesystem;

namespace {

uint64_t steady_now_ms() {
	using namespace std::chrono;
	return static_cast<uint64_t>(
			duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string absolute_path(const std::string &path) {
	if (path.empty())
		return path;
	std::error_code ec;
	const fs::path absolute = fs::absolute(fs::u8path(path), ec);
	return ec ? path : absolute.u8string();
}

std::string body_text(const net::HttpReply &reply) {
	return std::string(reply.body.begin(), reply.body.end());
}

std::string http_failure(const net::HttpReply &reply) {
	if (!reply.transport_ok)
		return "no answer (" + reply.error + ")";
	return "HTTP " + std::to_string(reply.code);
}

} // namespace

const std::vector<std::string> LocalMachineDriver::kServeEnvironment = {
#ifdef _WIN32
	"PATH",
	"SystemRoot",
	"SystemDrive",
	"windir",
	"TEMP",
	"TMP",
#else
	"PATH",
	"HOME",
	"LANG",
	"LC_*",
	"TZ",
	"TMPDIR",
#endif
};

// One stand-in machine: its agent's thread and what it runs. The thread alone
// starts, polls, stops and removes the containers (so their parent-death
// signal follows it); `mu` guards what view() copies and the stop flag.
struct LocalMachineDriver::Machine {
	struct Container {
		std::string name;
		int slot = 0;
		proc::ChildProcess process;
		bool spawned = false;
		std::optional<int64_t> exit_code; // set once it exited, or 125 for a failed start
		std::optional<int64_t> started_at;
		std::optional<uint64_t> stop_requested_ms;
		std::vector<std::string> argv;

		bool running() const { return spawned && !exit_code; }
	};

	int64_t id = 0;
	std::string cloud_id;
	LocalMachineConfig config;
	fs::path dir;

	std::mutex mu;
	std::condition_variable cv;
	bool stopping = false;
	bool bootstrapped = false;
	int slots = 0;
	uint64_t syncs = 0;
	bool draining = false;
	std::map<std::string, Container> containers;
	std::thread thread;

	// The agent's own, read by its thread alone.
	std::string token;
	std::string secret;
	std::string last_warning;
	std::set<std::string> refused_logged;
	int bootstrap_attempts = 0;

	void run();
	bool bootstrap();
	int pass();
	void start(const DesiredContainer &desired);
	void stop_all();
	void wipe_slot(int slot);
	net::HttpReply post(
			const std::string &path, const std::string &bearer, const std::string &body);
	void warn(const std::string &text);
	void recovered(const std::string &text);
};

net::HttpReply LocalMachineDriver::Machine::post(
		const std::string &path, const std::string &bearer, const std::string &body) {
	return net::http_exchange("POST", config.service_url + path,
			{ "Authorization: Bearer " + bearer, "Content-Type: application/json" }, body,
			config.http_timeout_ms, [](const std::string &host, net::Endpoint &out) {
				return net::resolve_ipv4(host, out, false);
			});
}

void LocalMachineDriver::Machine::warn(const std::string &text) {
	if (text == last_warning)
		return;
	last_warning = text;
	std::fprintf(stderr, "[fleet] WARN local machine %lld: %s\n", static_cast<long long>(id),
			text.c_str());
}

void LocalMachineDriver::Machine::recovered(const std::string &text) {
	if (last_warning.empty())
		return;
	last_warning.clear();
	std::printf("[fleet] local machine %lld: %s\n", static_cast<long long>(id), text.c_str());
}

void LocalMachineDriver::Machine::run() {
	for (;;) {
		int delay = 0;
		if (!bootstrapped)
			delay = bootstrap() ? 0 : config.bootstrap_retry_ms;
		else
			delay = pass();
		std::unique_lock<std::mutex> lock(mu);
		if (cv.wait_for(lock, std::chrono::milliseconds(delay), [this] { return stopping; }))
			break;
	}
	stop_all();
}

bool LocalMachineDriver::Machine::bootstrap() {
	AgentHello hello;
	hello.agent_version = config.agent_version;
	const net::HttpReply reply = post(machine_bootstrap_path(id), token, encode_agent_hello(hello));
	if (!reply.transport_ok || reply.code != 200) {
		// The first try may come before the launch's answer is written (the
		// routes admit no machine without its address): quiet.
		if (++bootstrap_attempts > 1)
			warn("the bootstrap failed: " + http_failure(reply));
		return false;
	}
	MachineBootstrap got;
	std::string error;
	if (!decode_machine_bootstrap(body_text(reply), got, error)) {
		warn("the bootstrap's reply does not read (" + error + ")");
		return false;
	}
	secret = got.agent_secret;
	token.clear();
	{
		std::lock_guard<std::mutex> lock(mu);
		bootstrapped = true;
		slots = got.slots;
	}
	last_warning.clear();
	std::printf("[fleet] local machine %lld bootstrapped: %d slots\n", static_cast<long long>(id),
			got.slots);
	return true;
}

int LocalMachineDriver::Machine::pass() {
	SyncRequest request;
	request.agent_version = config.agent_version;
	{
		std::lock_guard<std::mutex> lock(mu);
		for (auto &[name, c] : containers) {
			if (c.running())
				if (const auto code = c.process.poll())
					c.exit_code = static_cast<int64_t>(*code);
			ContainerReport report;
			report.name = name;
			report.slot = c.slot;
			report.state = c.running() ? ContainerState::Running : ContainerState::Exited;
			report.exit_code = c.exit_code;
			report.started_at = c.started_at;
			request.containers.push_back(std::move(report));
		}
	}
	const net::HttpReply reply = post(machine_sync_path(id), secret, encode_sync_request(request));
	if (!reply.transport_ok || reply.code != 200) {
		warn("the sync failed: " + http_failure(reply));
		return config.sync_interval_ms;
	}
	SyncReply answer;
	std::string error;
	if (!decode_sync_reply(body_text(reply), answer, error)) {
		warn("the sync's reply does not read (" + error + ")");
		return config.sync_interval_ms;
	}
	recovered("syncs again");

	std::lock_guard<std::mutex> lock(mu);
	++syncs;
	draining = answer.drain;
	const uint64_t now = steady_now_ms();
	bool busy = false;
	std::set<std::string> desired;
	for (const DesiredContainer &d : answer.desired)
		desired.insert(d.name);
	// The ones the reply leaves out: stopped, killed past the grace, removed.
	for (auto it = containers.begin(); it != containers.end();) {
		Container &c = it->second;
		if (desired.count(it->first) != 0) {
			++it;
			continue;
		}
		busy = true;
		if (c.running()) {
			if (!c.stop_requested_ms) {
				c.process.terminate();
				c.stop_requested_ms = now;
			} else if (now - *c.stop_requested_ms >= static_cast<uint64_t>(config.stop_grace_ms)) {
				c.process.kill();
			}
			++it;
			continue;
		}
		std::printf("[fleet] local machine %lld: container %s removed from slot %d\n",
				static_cast<long long>(id), it->first.c_str(), c.slot);
		const int slot = c.slot;
		it = containers.erase(it);
		wipe_slot(slot);
	}
	// The ones the machine lacks: started once their slot is free.
	for (const DesiredContainer &d : answer.desired) {
		if (containers.count(d.name) != 0)
			continue;
		std::string refusal;
		if (d.slot >= slots)
			refusal = "its slot is past the machine's";
		else if (!d.files)
			refusal = "its files were not sent";
		else if (!find_data_set(config.data_sets, d.data_set))
			refusal = "its data set is not on this machine";
		if (!refusal.empty()) {
			if (refused_logged.insert(d.name).second)
				std::fprintf(stderr,
						"[fleet] WARN local machine %lld: container %s cannot start: %s\n",
						static_cast<long long>(id), d.name.c_str(), refusal.c_str());
			continue;
		}
		busy = true;
		bool held = false;
		for (const auto &entry : containers)
			held = held || entry.second.slot == d.slot;
		if (!held)
			start(d);
	}
	return busy ? config.busy_sync_interval_ms : config.sync_interval_ms;
}

void LocalMachineDriver::Machine::start(const DesiredContainer &desired) {
	Container c;
	c.name = desired.name;
	c.slot = desired.slot;
	const DataSet *data_set = find_data_set(config.data_sets, desired.data_set);
	const fs::path slot_dir = dir / slot_dir_name(desired.slot);
	std::string error;
	wipe_slot(desired.slot);
	std::error_code ec;
	fs::create_directories(slot_dir, ec);
	if (ec)
		error = "cannot create the slot's directory: " + ec.message();
#ifndef _WIN32
	if (error.empty()) {
		fs::permissions(slot_dir, fs::perms::owner_all, fs::perm_options::replace, ec);
		if (ec)
			error = "cannot make the slot's directory the service's alone: " + ec.message();
	}
#endif
	if (error.empty())
		for (const auto &[file, bytes] : *desired.files)
			if (!write_owner_file(slot_dir / fs::u8path(file), bytes, true, error))
				break;
	if (error.empty()) {
		proc::SpawnOptions options;
		options.argv = { config.serve_binary, "--resource-dir", data_set->resource_dir };
		if (data_set->loose_root)
			options.argv.push_back("--loose-root");
		options.argv.insert(options.argv.end(), desired.args.begin(), desired.args.end());
		c.argv = options.argv;
		options.working_dir = slot_dir.u8string();
		options.output_path = (dir / (desired.name + ".log")).u8string();
		std::vector<std::string> environment = proc::filter_environment(kServeEnvironment);
		for (const auto &[name, value] : desired.env)
			environment.push_back(name + "=" + value);
		options.environment = std::move(environment);
		options.end_with_parent = true;
		if (c.process.spawn(options, error)) {
			c.spawned = true;
			c.started_at = static_cast<int64_t>(std::time(nullptr));
			std::printf("[fleet] local machine %lld: container %s runs in slot %d as pid %lld\n",
					static_cast<long long>(id), c.name.c_str(), c.slot,
					static_cast<long long>(c.process.pid()));
		}
	}
	if (!c.spawned) {
		// Reported exited (docker's 125) until the reply drops it; never retried.
		c.exit_code = 125;
		std::fprintf(stderr, "[fleet] WARN local machine %lld: container %s did not start: %s\n",
				static_cast<long long>(id), c.name.c_str(), error.c_str());
		wipe_slot(desired.slot);
	}
	containers.emplace(c.name, std::move(c));
}

void LocalMachineDriver::Machine::wipe_slot(int slot) {
	std::error_code ec;
	fs::remove_all(dir / slot_dir_name(slot), ec);
}

void LocalMachineDriver::Machine::stop_all() {
	std::lock_guard<std::mutex> lock(mu);
	for (auto &[name, c] : containers)
		if (c.running())
			c.process.terminate();
	for (auto &[name, c] : containers) {
		if (!c.running())
			continue;
		if (!c.process.wait(kTerminateGraceMs)) {
			c.process.kill();
			c.process.wait(kTerminateGraceMs);
		}
	}
	for (const auto &entry : containers)
		wipe_slot(entry.second.slot);
	containers.clear();
}

LocalMachineDriver::LocalMachineDriver(LocalMachineConfig config) : config_(std::move(config)) {
	config_.serve_binary = absolute_path(config_.serve_binary);
	for (DataSet &d : config_.data_sets)
		d.resource_dir = absolute_path(d.resource_dir);
	config_.work_dir = absolute_path(config_.work_dir);
}

LocalMachineDriver::~LocalMachineDriver() {
	std::vector<std::string> ids;
	{
		std::lock_guard<std::mutex> lock(mu_);
		for (const auto &entry : machines_)
			ids.push_back(entry.first);
	}
	for (const std::string &id : ids)
		terminate(id);
}

void LocalMachineDriver::set_service_url(const std::string &url) {
	std::lock_guard<std::mutex> lock(mu_);
	config_.service_url = url;
}

std::optional<LaunchedMachine> LocalMachineDriver::launch(
		const MachineLaunch &launch, std::string &error) {
	std::lock_guard<std::mutex> lock(mu_);
	if (launch.machine_id <= 0) {
		error = "no machine id";
		return std::nullopt;
	}
	if (config_.serve_binary.empty() || config_.work_dir.empty() || config_.service_url.empty()) {
		error = "the local driver needs ONNET_HOSTED_SERVE_BIN, ONNET_HOSTED_WORK_DIR and the "
				"internal listener's address";
		return std::nullopt;
	}
	auto machine = std::make_unique<Machine>();
	machine->id = launch.machine_id;
	machine->cloud_id =
			"local-" + std::to_string(launch.machine_id) + "-" + std::to_string(next_machine_++);
	machine->config = config_;
	machine->token = launch.bootstrap_token;
	machine->dir = fs::u8path(config_.work_dir) / ("machine-" + std::to_string(launch.machine_id));
	std::error_code ec;
	fs::remove_all(machine->dir, ec);
	fs::create_directories(machine->dir, ec);
	if (ec) {
		error = "cannot create the machine's directory: " + ec.message();
		return std::nullopt;
	}
	if (!make_owner_directory(machine->dir, error))
		return std::nullopt;
	Machine *raw = machine.get();
	machine->thread = std::thread([raw] { raw->run(); });
	LaunchedMachine out;
	out.cloud_id = machine->cloud_id;
	out.private_address = "127.0.0.1";
	out.public_address = "127.0.0.1";
	out.instance_type = "local";
	out.template_version = "local";
	std::printf("[fleet] local: machine %lld (%s, %d slots) runs as %s in %s\n",
			static_cast<long long>(launch.machine_id), machine_kind_text(launch.kind), launch.slots,
			out.cloud_id.c_str(), machine->dir.u8string().c_str());
	machines_.emplace(out.cloud_id, std::move(machine));
	return out;
}

CloudMachine LocalMachineDriver::describe(const std::string &cloud_id) {
	std::lock_guard<std::mutex> lock(mu_);
	CloudMachine status;
	if (machines_.count(cloud_id) == 0)
		return status; // Gone
	status.state = CloudState::Running;
	status.private_address = "127.0.0.1";
	status.public_address = "127.0.0.1";
	return status;
}

void LocalMachineDriver::terminate(const std::string &cloud_id) {
	std::unique_ptr<Machine> machine;
	{
		std::lock_guard<std::mutex> lock(mu_);
		const auto it = machines_.find(cloud_id);
		if (it == machines_.end())
			return;
		machine = std::move(it->second);
		machines_.erase(it);
	}
	{
		std::lock_guard<std::mutex> lock(machine->mu);
		machine->stopping = true;
	}
	machine->cv.notify_all();
	if (machine->thread.joinable())
		machine->thread.join();
	std::printf("[fleet] local: machine %lld (%s) ended\n", static_cast<long long>(machine->id),
			cloud_id.c_str());
}

std::vector<TaggedCloudMachine> LocalMachineDriver::list_tagged() {
	std::lock_guard<std::mutex> lock(mu_);
	std::vector<TaggedCloudMachine> out;
	for (const auto &entry : machines_)
		out.push_back({ entry.first, entry.second->id });
	return out;
}

SlotPlan LocalMachineDriver::slot_plan(const std::string &, int) {
	// Ports free a moment ago on this system (another process may take one
	// first; the server's bind then fails, which reads as a crash and restarts
	// with a new plan).
	net::startup();
	SlotPlan plan;
	{
		net::ScopedSocket udp(net::udp_bind(0, &plan.game_port));
	}
	{
		net::ScopedSocket tcp(net::tcp_listen(0, 1, &plan.admin_port, /*loopback_only=*/true));
	}
	plan.admin_allowed_ip = "127.0.0.1";
	return plan;
}

std::optional<LocalMachineView> LocalMachineDriver::view(const std::string &cloud_id) {
	std::lock_guard<std::mutex> lock(mu_);
	const auto it = machines_.find(cloud_id);
	if (it == machines_.end())
		return std::nullopt;
	Machine &m = *it->second;
	std::lock_guard<std::mutex> machine_lock(m.mu);
	LocalMachineView out;
	out.bootstrapped = m.bootstrapped;
	out.slots = m.slots;
	out.syncs = m.syncs;
	out.draining = m.draining;
	for (const auto &[name, c] : m.containers) {
		LocalMachineView::Container v;
		v.name = name;
		v.slot = c.slot;
		v.state = c.running() ? ContainerState::Running : ContainerState::Exited;
		v.exit_code = c.exit_code;
		v.argv = c.argv;
		out.containers.push_back(std::move(v));
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
