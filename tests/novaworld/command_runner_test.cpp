// The real ProcessCommandRunner (apps/common/command_runner.h), the one the EC2
// machine driver runs the AWS CLI through and billing runs curl through,
// against this binary itself (`--child <mode>`): stdout and stderr captured
// apart, the exit code, the timeout, a program that does not start, nothing of
// a run left in its scratch directory, and the AWS CLI's environment (the AWS
// variables and the system's, never the service's own, and the standard retry
// mode unless the service names one). Database-free.

#include "command_runner.h"
#include "core/owner_file.h"

#include <base/proc/child_process.h>

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace nws = opennova::novaworld_server;
using opennova::proc::CommandResult;

namespace {

std::string g_self;

#ifdef _WIN32
void set_env(const char *name, const char *value) { _putenv_s(name, value); }
#else
void set_env(const char *name, const char *value) { setenv(name, value, 1); }
#endif

int test_process_runner(const std::string &scratch) {
	const fs::path dir = fs::u8path(scratch) / "runner";
	std::string error;
	TEST_EXPECT(nws::make_owner_directory(dir, error));
	opennova::proc::ProcessCommandRunner runner(opennova::proc::current_environment(), dir.u8string());

	CommandResult r = runner.run({ g_self, "--child", "streams" }, 20000);
	TEST_EXPECT(r.ok() && r.outcome == CommandResult::Outcome::Exited);
	TEST_EXPECT(r.out.find("{\"on\":\"stdout\"}") != std::string::npos && r.out.find("diag") == std::string::npos);
	TEST_EXPECT(r.err.find("diag on stderr") != std::string::npos && r.err.find("stdout") == std::string::npos);

	r = runner.run({ g_self, "--child", "exit", "3" }, 20000);
	TEST_EXPECT(r.outcome == CommandResult::Outcome::Exited && r.exit_code == 3u && !r.ok());

	const auto started = std::chrono::steady_clock::now();
	r = runner.run({ g_self, "--child", "sleep" }, 300);
	TEST_EXPECT(r.outcome == CommandResult::Outcome::TimedOut && !r.ok());
	TEST_EXPECT(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));

	r = runner.run({ (dir / "no-such-program").u8string() }, 20000);
	TEST_EXPECT(r.outcome == CommandResult::Outcome::NotStarted && !r.error.empty() && !r.ok());

	// Nothing of a run stays in the scratch directory.
	TEST_EXPECT(fs::is_empty(dir));

	// The CLI's environment: the AWS variables and the system's, never the service's own,
	// and the standard retry mode unless the service names one.
	set_env("AWS_OPENNOVA_TEST_VAR", "kept");
	set_env("ONNET_OPENNOVA_TEST_SECRET", "hunter2");
	std::vector<std::string> env = opennova::proc::ProcessCommandRunner::service_environment();
	const auto has = [&env](const std::string &entry) {
		for (const std::string &e : env)
			if (e == entry) return true;
		return false;
	};
	const auto names = [&env](const std::string &prefix) {
		for (const std::string &e : env)
			if (e.rfind(prefix, 0) == 0) return true;
		return false;
	};
	TEST_EXPECT(has("AWS_OPENNOVA_TEST_VAR=kept"));
	TEST_EXPECT(!names("ONNET_OPENNOVA_TEST_SECRET="));
	TEST_EXPECT(names("AWS_RETRY_MODE=") && names("AWS_MAX_ATTEMPTS="));
	set_env("AWS_RETRY_MODE", "adaptive");
	env = opennova::proc::ProcessCommandRunner::service_environment();
	TEST_EXPECT(has("AWS_RETRY_MODE=adaptive") && !has("AWS_RETRY_MODE=standard"));
	return 0;
}

int child_main(int argc, char **argv) {
	const std::string mode = argc > 2 ? argv[2] : "";
	if (mode == "streams") {
		std::fprintf(stdout, "{\"on\":\"stdout\"}\n");
		std::fflush(stdout);
		std::fprintf(stderr, "diag on stderr\n");
		return 0;
	}
	if (mode == "exit") return argc > 3 ? std::atoi(argv[3]) : 0;
	if (mode == "sleep") {
		std::this_thread::sleep_for(std::chrono::seconds(60));
		return 9;
	}
	return 99;
}

} // namespace

int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return child_main(argc, argv);
	if (argc < 2) {
		std::fprintf(stderr, "usage: command_runner_test <this binary's path>\n");
		return 2;
	}
	g_self = argv[1];
	test_temp::TempDir temp("command_runner");
	std::printf("-- process_runner\n");
	if (test_process_runner(temp.root()) != 0) {
		std::fprintf(stderr, "FAIL: process_runner\n");
		return 1;
	}
	std::printf("OK: the command runner\n");
	return 0;
}
