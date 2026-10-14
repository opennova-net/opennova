#include "local_process_provider.h"

#include "net_sockets.h"
#include "owner_file.h"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace fs = std::filesystem;

namespace {

std::string absolute_path(const std::string &path) {
	if (path.empty()) return path;
	std::error_code ec;
	const fs::path absolute = fs::absolute(fs::u8path(path), ec);
	return ec ? path : absolute.u8string();
}

} // namespace

const std::vector<std::string> LocalProcessProvider::kServeEnvironment = {
#ifdef _WIN32
	"PATH", "SystemRoot", "SystemDrive", "windir", "TEMP", "TMP",
#else
	"PATH", "HOME", "LANG", "LC_*", "TZ", "TMPDIR",
#endif
};

LocalProcessProvider::LocalProcessProvider(LocalProcessConfig config) : config_(std::move(config)) {
	config_.serve_binary = absolute_path(config_.serve_binary);
	for (DataSet &d : config_.data_sets) d.resource_dir = absolute_path(d.resource_dir);
	config_.work_dir = absolute_path(config_.work_dir);
	spawner_ = std::thread([this] { spawner_loop(); });
}

LocalProcessProvider::~LocalProcessProvider() {
	// Every child asked to stop and reaped first, then the spawner thread joined:
	// its exit is what the children's parent-death signal follows.
	std::vector<std::string> ids;
	{
		std::lock_guard<std::mutex> lock(mu_);
		for (const auto &entry : machines_) ids.push_back(entry.first);
	}
	for (const std::string &id : ids) terminate(id);
	{
		std::lock_guard<std::mutex> lock(spawner_mu_);
		spawner_stopping_ = true;
	}
	spawner_cv_.notify_all();
	if (spawner_.joinable()) spawner_.join();
}

void LocalProcessProvider::spawner_loop() {
	for (;;) {
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(spawner_mu_);
			spawner_cv_.wait(lock, [this] { return spawner_stopping_ || !spawner_tasks_.empty(); });
			if (spawner_tasks_.empty()) return;
			task = std::move(spawner_tasks_.front());
			spawner_tasks_.pop_front();
		}
		// on_spawner's wrapper catches whatever the task throws; nothing may end
		// this thread but the stop (an escaping throw would end the service).
		task();
	}
}

bool LocalProcessProvider::on_spawner(const std::function<void()> &task, std::string &error) {
	std::mutex done_mu;
	std::condition_variable done_cv;
	bool done = false;
	std::string thrown;
	{
		std::lock_guard<std::mutex> lock(spawner_mu_);
		// The destructor has asked the spawner to end: a task queued now could
		// find it gone and would wait for good.
		if (spawner_stopping_) {
			error = "the local provider is shutting down";
			return false;
		}
		spawner_tasks_.push_back([&] {
			try {
				task();
			} catch (const std::exception &e) {
				thrown = e.what();
				if (thrown.empty()) thrown = "an exception";
			} catch (...) {
				thrown = "an exception";
			}
			std::lock_guard<std::mutex> done_lock(done_mu);
			done = true;
			done_cv.notify_all();
		});
	}
	spawner_cv_.notify_all();
	std::unique_lock<std::mutex> lock(done_mu);
	done_cv.wait(lock, [&] { return done; });
	if (!thrown.empty()) {
		error = "the spawn failed: " + thrown;
		return false;
	}
	return true;
}

fs::path LocalProcessProvider::server_dir(int64_t server_id) const {
	return fs::u8path(config_.work_dir) / ("server-" + std::to_string(server_id));
}

MachinePlan LocalProcessProvider::plan(int64_t) {
	// Ports free a moment ago on this machine (another process may take one
	// first; the server's bind then fails, which reads as a crash and relaunches
	// with a new plan).
	net::startup();
	MachinePlan plan;
	{
		net::ScopedSocket udp(net::udp_bind(0, &plan.nw_port));
	}
	{
		net::ScopedSocket tcp(net::tcp_listen(0, 1, &plan.admin_port, /*loopback_only=*/true));
	}
	plan.admin_allowed_ip = "127.0.0.1";
	return plan;
}

std::optional<std::string> LocalProcessProvider::launch(const LaunchSpec &spec, std::string &error) {
	if (config_.serve_binary.empty() || config_.work_dir.empty()) {
		error = "the local provider needs ONNET_HOSTED_SERVE_BIN and ONNET_HOSTED_WORK_DIR";
		return std::nullopt;
	}
	const DataSet *data_set = find_data_set(config_.data_sets, spec.data_set);
	if (!data_set || data_set->resource_dir.empty()) {
		error = "the local provider has no game folder for data set '" + spec.data_set +
				"' (ONNET_HOSTED_DATA_SETS resource_dir)";
		return std::nullopt;
	}
	const fs::path dir = server_dir(spec.server_id);
	std::error_code ec;
	fs::remove_all(dir, ec);
	if (ec) {
		error = "cannot clear the working directory: " + ec.message();
		return std::nullopt;
	}
	fs::create_directories(dir, ec);
	if (ec) {
		error = "cannot create the working directory: " + ec.message();
		return std::nullopt;
	}
#ifndef _WIN32
	fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
	if (ec) {
		error = "cannot make the working directory the service's alone: " + ec.message();
		return std::nullopt;
	}
#endif
	Machine machine;
	machine.server_id = spec.server_id;
	for (const LaunchFile &file : spec.files) {
		if (file.name.empty() || file.name.find_first_of("/\\") != std::string::npos ||
				file.name == "." || file.name == "..") {
			error = "a launch file's name is no bare file name";
			return std::nullopt;
		}
		const fs::path path = dir / fs::u8path(file.name);
		if (!write_owner_file(path, file.content, file.secret, error)) return std::nullopt;
		if (file.secret) machine.secret_files.push_back(path);
	}

	proc::SpawnOptions options;
	options.argv = { config_.serve_binary, "--resource-dir", data_set->resource_dir };
	if (data_set->loose_root) options.argv.push_back("--loose-root");
	options.argv.insert(options.argv.end(), spec.serve_args.begin(), spec.serve_args.end());
	machine.argv = options.argv;
	options.working_dir = dir.u8string();
	options.output_path = (dir / kConsoleLogName).u8string();
	options.environment = proc::filter_environment(kServeEnvironment);
	options.end_with_parent = true;
	bool spawned = false;
	if (!on_spawner([&] { spawned = machine.process.spawn(options, error); }, error) || !spawned)
		return std::nullopt;

	std::lock_guard<std::mutex> lock(mu_);
	const std::string id =
			"local-" + std::to_string(spec.server_id) + "-" + std::to_string(next_machine_++);
	std::printf("[hosted] local: server %lld runs %s as pid %lld (%s) in %s\n",
			static_cast<long long>(spec.server_id), data_set->game.c_str(),
			static_cast<long long>(machine.process.pid()), id.c_str(), dir.u8string().c_str());
	machines_.emplace(id, std::move(machine));
	return id;
}

std::vector<std::string> LocalProcessProvider::command_of(const std::string &machine_id) {
	std::lock_guard<std::mutex> lock(mu_);
	const auto it = machines_.find(machine_id);
	return it == machines_.end() ? std::vector<std::string>() : it->second.argv;
}

MachineStatus LocalProcessProvider::describe(const std::string &machine_id) {
	std::lock_guard<std::mutex> lock(mu_);
	MachineStatus status;
	const auto it = machines_.find(machine_id);
	if (it == machines_.end()) return status; // Gone
	status.address = "127.0.0.1";
	if (const auto code = it->second.process.poll()) {
		status.state = MachineState::Exited;
		status.exit_code = *code;
	} else {
		status.state = MachineState::Running;
	}
	return status;
}

void LocalProcessProvider::terminate(const std::string &machine_id) {
	Machine machine;
	{
		std::lock_guard<std::mutex> lock(mu_);
		const auto it = machines_.find(machine_id);
		if (it == machines_.end()) return;
		machine = std::move(it->second);
		machines_.erase(it);
	}
	// A stop request first (opennova-serve then ends its session and
	// deregisters), the kill after the grace.
	machine.process.terminate();
	if (!machine.process.wait(kTerminateGraceMs)) {
		machine.process.kill();
		machine.process.wait(kTerminateGraceMs);
	}
	// Its credentials outlive it nowhere; console.log stays for the record.
	for (const fs::path &path : machine.secret_files) {
		std::error_code ec;
		fs::remove(path, ec);
	}
}

std::vector<TaggedMachine> LocalProcessProvider::list_tagged() {
	std::lock_guard<std::mutex> lock(mu_);
	std::vector<TaggedMachine> out;
	for (const auto &entry : machines_) out.push_back({ entry.first, entry.second.server_id });
	return out;
}

} // namespace opennova::novaworld_server::hosted
