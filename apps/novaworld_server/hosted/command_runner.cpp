#include "command_runner.h"

#include "child_process.h"
#include "hosted_settings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::hosted {

namespace fs = std::filesystem;

namespace {

// At most `cap` bytes of the file at `path`.
std::string read_capped(const fs::path &path, size_t cap) {
	std::ifstream in(path, std::ios::binary);
	std::string out;
	char buffer[64 * 1024];
	while (in && out.size() < cap) {
		in.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
		const size_t got = static_cast<size_t>(in.gcount());
		if (got == 0) break;
		out.append(buffer, std::min(got, cap - out.size()));
	}
	return out;
}

} // namespace

const std::vector<std::string> ProcessCommandRunner::kEnvironment = {
#ifdef _WIN32
	"PATH", "SystemRoot", "SystemDrive", "windir", "TEMP", "TMP", "USERPROFILE", "AWS_*",
#else
	"PATH", "HOME", "LANG", "LC_*", "TZ", "TMPDIR", "AWS_*",
#endif
};

std::vector<std::string> ProcessCommandRunner::service_environment() {
	std::vector<std::string> env = proc::filter_environment(kEnvironment);
	for (const char *fallback : { "AWS_RETRY_MODE=standard", "AWS_MAX_ATTEMPTS=4" }) {
		const std::string name = std::string(fallback).substr(0, std::string(fallback).find('=') + 1);
		bool named = false;
		for (const std::string &entry : env) named = named || entry.compare(0, name.size(), name) == 0;
		if (!named) env.emplace_back(fallback);
	}
	return env;
}

ProcessCommandRunner::ProcessCommandRunner(std::vector<std::string> environment, std::string scratch_dir) :
		environment_(std::move(environment)), scratch_dir_(std::move(scratch_dir)) {}

CommandResult ProcessCommandRunner::run(const std::vector<std::string> &argv, int timeout_ms) {
	CommandResult result;
	// A pair of files of this run's own: a random name in the owner-only
	// scratch directory, so two runs never share one.
	const std::string stem = "cmd-" + random_secret(16);
	const fs::path out_path = fs::u8path(scratch_dir_) / (stem + ".out");
	const fs::path err_path = fs::u8path(scratch_dir_) / (stem + ".err");
	proc::SpawnOptions options;
	options.argv = argv;
	options.output_path = out_path.u8string();
	options.error_path = err_path.u8string();
	options.environment = environment_;
	// The run is waited for here, so the starting thread outlives it.
	options.end_with_parent = true;

	proc::ChildProcess child;
	if (!child.spawn(options, result.error)) {
		result.outcome = CommandResult::Outcome::NotStarted;
	} else if (const auto code = child.wait(timeout_ms)) {
		result.outcome = CommandResult::Outcome::Exited;
		result.exit_code = *code;
	} else {
		child.kill();
		child.wait(5000);
		result.outcome = CommandResult::Outcome::TimedOut;
	}
	result.out = read_capped(out_path, kMaxOutputBytes);
	result.err = read_capped(err_path, kMaxOutputBytes);
	std::error_code ec;
	fs::remove(out_path, ec);
	fs::remove(err_path, ec);
	return result;
}

} // namespace opennova::novaworld_server::hosted
