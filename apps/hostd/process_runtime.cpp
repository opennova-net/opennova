#include "process_runtime.h"

#include <filesystem>
#include <base/io/file_io.h>

#include <ctime>
#include <utility>

#include <unistd.h>

namespace opennova::hostd {

std::vector<std::string> read_env_file_lines(const std::string &text) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const size_t eq = line.find('=');
		if (eq != std::string::npos && eq > 0)
			out.push_back(std::move(line));
		start = end + 1;
	}
	return out;
}

ProcessRuntime::ProcessRuntime(std::string serve, std::vector<std::string> environment,
		std::string output_path, Clock now_s, std::string log_dir) :
		serve_(std::move(serve)),
		environment_(std::move(environment)),
		output_path_(std::move(output_path)),
		now_s_(now_s ? std::move(now_s) : [] { return static_cast<int64_t>(std::time(nullptr)); }),
		log_dir_(std::move(log_dir)) {}

ProcessRuntime::~ProcessRuntime() {
	for (auto &[name, server] : servers_) {
		if (server.exit_code)
			continue;
		server.process.kill();
		server.process.wait(5000);
	}
}

void ProcessRuntime::poll(Server &server) {
	if (server.exit_code)
		return;
	if (const auto code = server.process.poll())
		server.exit_code = static_cast<int64_t>(*code);
}

bool ProcessRuntime::list(std::vector<ContainerInfo> &out, int, std::string &) {
	out.clear();
	for (auto &[name, server] : servers_) {
		poll(server);
		ContainerInfo info;
		info.name = name;
		info.slot = server.slot;
		info.state =
				server.exit_code ? hosted::ContainerState::Exited : hosted::ContainerState::Running;
		info.exit_code = server.exit_code;
		info.started_at = server.started_at;
		out.push_back(std::move(info));
	}
	return true;
}

bool ProcessRuntime::stats(const std::vector<std::string> &names,
		std::map<std::string, ContainerStats> &out, int, std::string &) {
	out.clear();
	for (const std::string &name : names)
		if (servers_.count(name) != 0)
			out[name] = ContainerStats{};
	return true;
}

bool ProcessRuntime::has_image(const std::string &, bool &present, int, std::string &) {
	present = true;
	return true;
}

bool ProcessRuntime::start_pull(const std::string &, std::string &) {
	return true;
}

PullState ProcessRuntime::poll_pull(const std::string &, std::string &) {
	return PullState::Done;
}

void ProcessRuntime::cancel_pull(const std::string &) {}

std::vector<std::string> ProcessRuntime::run_argv(const ContainerSpec &spec) const {
	std::vector<std::string> argv = { serve_, "--resource-dir", spec.base_dir };
	argv.insert(argv.end(), spec.args.begin(), spec.args.end());
	return argv;
}

std::string ProcessRuntime::output_of(const ContainerSpec &spec) const {
	if (log_dir_.empty() || !hosted::is_log_stream_name(spec.log_stream))
		return output_path_;
	return log_dir_ + "/servers/" + spec.log_stream + ".log";
}

bool ProcessRuntime::run(const ContainerSpec &spec, int, int &exit_code, std::string &error) {
	exit_code = 125;
	if (servers_.count(spec.name) != 0) {
		error = "a container named " + spec.name + " is on the machine already";
		return false;
	}
	std::vector<std::string> environment = environment_;
	if (!spec.env_file.empty()) {
		std::string text;
		if (!io::read_file_text(spec.env_file, text, error)) {
			error = "its environment file does not read: " + error;
			return false;
		}
		for (std::string &entry : read_env_file_lines(text))
			environment.push_back(std::move(entry));
	}
	// An expansion tree sits in the base tree at expansion/<code>, where /exp finds it:
	// the agent made that directory (TreeStore::expansion_mount_point, the docker
	// runtime's mount point), which becomes a link to the expansion tree here.
	if (!spec.expansion_dir.empty()) {
		std::error_code ec;
		const std::filesystem::path at = std::filesystem::path(spec.base_dir) / "expansion" / spec.expansion_code;
		if (std::filesystem::is_directory(std::filesystem::symlink_status(at, ec)))
			std::filesystem::remove(at, ec);
		if (!std::filesystem::is_symlink(std::filesystem::symlink_status(at, ec)))
			std::filesystem::create_directory_symlink(spec.expansion_dir, at, ec);
		if (ec) {
			error = "the expansion tree could not be linked at " + at.string() + ": " + ec.message();
			exit_code = 125;
			return false;
		}
	}
	proc::SpawnOptions options;
	options.argv = run_argv(spec);
	options.working_dir = spec.work_dir;
	const std::string output = output_of(spec);
	options.output_path = output;
	// Into the log directory: through a pipe whose lines are stamped with their
	// time, its write end the child's alone once it holds it.
	int write_fd = -1;
	if (output != output_path_ && !stamped_.open(output, options.output_path, write_fd, error))
		return false;
	options.environment = std::move(environment);
	options.end_with_parent = true;
	Server server;
	server.slot = spec.slot;
	const bool spawned = server.process.spawn(options, error);
	if (write_fd >= 0)
		::close(write_fd);
	if (!spawned)
		return false;
	server.started_at = now_s_();
	servers_.emplace(spec.name, std::move(server));
	exit_code = 0;
	return true;
}

bool ProcessRuntime::stop(const std::string &name, int, std::string &error) {
	const auto it = servers_.find(name);
	if (it == servers_.end()) {
		error = "no container named " + name;
		return false;
	}
	poll(it->second);
	if (!it->second.exit_code)
		it->second.process.terminate();
	return true;
}

bool ProcessRuntime::kill(const std::string &name, int, std::string &error) {
	const auto it = servers_.find(name);
	if (it == servers_.end()) {
		error = "no container named " + name;
		return false;
	}
	poll(it->second);
	if (!it->second.exit_code)
		it->second.process.kill();
	return true;
}

bool ProcessRuntime::list_images(
		const std::string &, std::vector<std::string> &out, int, std::string &) {
	out.clear();
	return true;
}

bool ProcessRuntime::remove_image(const std::string &image, int, std::string &error) {
	error = image + " is the machine's own image";
	return false;
}

bool ProcessRuntime::remove(const std::string &name, int, std::string &error) {
	const auto it = servers_.find(name);
	if (it == servers_.end())
		return true;
	poll(it->second);
	if (!it->second.exit_code) {
		error = name + " still runs";
		return false;
	}
	servers_.erase(it);
	return true;
}

} // namespace opennova::hostd
