// apps/common/child_process.h: a child started from an argv array with no shell, run
// against this test binary itself (ctest passes its path; `--child <mode>` makes
// it the child). Exit codes, every argument reaching the child as it was given
// (spaces, quotes, backslashes, an empty one), the working directory, stdin at
// end of file, stdout and stderr appended to the output file, poll while it
// runs, terminate and kill, the refusals (a missing program, a missing working
// directory, a second spawn while one runs) and, on POSIX, no descriptor of
// this process reaching the child and the destructor killing a running one.

#include "child_process.h"

#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using opennova::proc::ChildProcess;
using opennova::proc::SpawnOptions;

namespace {

std::string g_self;

// --- the child's side ---------------------------------------------------------

int child_main(int argc, char **argv) {
	const std::string mode = argc > 2 ? argv[2] : "";
	if (mode == "exit") return argc > 3 ? std::atoi(argv[3]) : 0;
	if (mode == "args") {
		for (int i = 3; i < argc; ++i) std::printf("[%s]\n", argv[i]);
		return 0;
	}
	if (mode == "cwd") {
		std::printf("%s\n", fs::current_path().u8string().c_str());
		return 0;
	}
	if (mode == "stdin") {
		const int c = std::getchar();
		std::printf(c == EOF ? "eof\n" : "data\n");
		return 0;
	}
	if (mode == "stderr") {
		std::fprintf(stdout, "to stdout\n");
		std::fflush(stdout);
		std::fprintf(stderr, "to stderr\n");
		return 0;
	}
	if (mode == "sleep") {
		std::this_thread::sleep_for(std::chrono::seconds(60));
		return 9;
	}
#ifndef _WIN32
	if (mode == "fds") {
		int open_fds = 0;
		for (int fd = 3; fd < 1024; ++fd) {
			if (::fcntl(fd, F_GETFD) != -1) ++open_fds;
		}
		std::printf("%d\n", open_fds);
		return 0;
	}
#endif
	return 99;
}

// --- the parent's side --------------------------------------------------------

SpawnOptions child(const std::vector<std::string> &args, const std::string &output = {}) {
	SpawnOptions options;
	options.argv = { g_self, "--child" };
	options.argv.insert(options.argv.end(), args.begin(), args.end());
	options.output_path = output;
	return options;
}

// The output file's lines, CR dropped (a Windows child's stdout is text mode).
std::vector<std::string> read_lines(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		lines.push_back(line);
	}
	return lines;
}

int test_exit_codes() {
	for (const int code : { 0, 7, 42 }) {
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "exit", std::to_string(code) }), error));
		TEST_EXPECT(p.started() && p.pid() > 0);
		const auto exited = p.wait(20000);
		TEST_EXPECT(exited && *exited == code);
		TEST_EXPECT(!p.started() && p.pid() == 0);
		TEST_EXPECT(p.exit_code() == code);
		TEST_EXPECT(p.poll() == code); // reaped: the kept code
	}
	return 0;
}

int test_arguments_arrive_whole(const fs::path &dir) {
	const std::vector<std::string> args = {
		"plain", "two words", "", "quo\"te", "back\\slash", "trailing\\", "both \\\" mixed",
		"C:\\dir with space\\", "tab\there", "semi;colon & pipe | <redirect> $HOME %PATH%",
	};
	const fs::path out = dir / "args.txt";
	std::vector<std::string> child_args = { "args" };
	child_args.insert(child_args.end(), args.begin(), args.end());
	ChildProcess p;
	std::string error;
	TEST_EXPECT(p.spawn(child(child_args, out.u8string()), error));
	TEST_EXPECT(p.wait(20000) == 0);
	const auto lines = read_lines(out);
	if (lines.size() != args.size()) std::fprintf(stderr, "  %zu lines, want %zu\n", lines.size(), args.size());
	TEST_EXPECT(lines.size() == args.size());
	for (size_t i = 0; i < args.size(); ++i) {
		if (lines[i] != "[" + args[i] + "]")
			std::fprintf(stderr, "  arg %zu: got %s, want [%s]\n", i, lines[i].c_str(), args[i].c_str());
		TEST_EXPECT(lines[i] == "[" + args[i] + "]");
	}
	return 0;
}

int test_working_dir_stdin_and_output(const fs::path &dir) {
	const fs::path work = dir / "work dir";
	fs::create_directories(work);
	{
		ChildProcess p;
		SpawnOptions options = child({ "cwd" }, (dir / "cwd.txt").u8string());
		options.working_dir = work.u8string();
		std::string error;
		TEST_EXPECT(p.spawn(options, error));
		TEST_EXPECT(p.wait(20000) == 0);
		const auto lines = read_lines(dir / "cwd.txt");
		TEST_EXPECT(lines.size() == 1);
		std::error_code ec;
		TEST_EXPECT(fs::equivalent(fs::u8path(lines[0]), work, ec));
	}
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "stdin" }, (dir / "stdin.txt").u8string()), error));
		TEST_EXPECT(p.wait(20000) == 0);
		TEST_EXPECT(read_lines(dir / "stdin.txt") == std::vector<std::string>{ "eof" });
	}
	{
		// stdout and stderr both land in the file, appended to what it held.
		{
			std::ofstream seed(dir / "both.txt", std::ios::binary);
			seed << "before\n";
		}
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "stderr" }, (dir / "both.txt").u8string()), error));
		TEST_EXPECT(p.wait(20000) == 0);
		const auto lines = read_lines(dir / "both.txt");
		TEST_EXPECT(lines.size() == 3);
		TEST_EXPECT(lines[0] == "before" && lines[1] == "to stdout" && lines[2] == "to stderr");
	}
	{
		// No output file: the output is discarded and the child still runs.
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "args", "discarded" }), error));
		TEST_EXPECT(p.wait(20000) == 0);
	}
	return 0;
}

int test_terminate_and_kill() {
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "sleep" }), error));
		TEST_EXPECT(!p.poll().has_value());
		TEST_EXPECT(!p.wait(200).has_value());
		// One child at a time.
		std::string second_error;
		TEST_EXPECT(!p.spawn(child({ "exit", "0" }), second_error) && !second_error.empty());
		p.terminate();
		const auto code = p.wait(20000);
		TEST_EXPECT(code.has_value());
#ifdef _WIN32
		TEST_EXPECT(*code == 1);
#else
		TEST_EXPECT(*code == 128 + SIGTERM);
#endif
	}
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "sleep" }), error));
		p.kill();
		const auto code = p.wait(20000);
		TEST_EXPECT(code.has_value());
#ifdef _WIN32
		TEST_EXPECT(*code == 1);
#else
		TEST_EXPECT(*code == 128 + SIGKILL);
#endif
		// Once exited, both are no-ops.
		p.terminate();
		p.kill();
		TEST_EXPECT(p.poll() == code);
	}
	return 0;
}

int test_refusals(const fs::path &dir) {
	{
		ChildProcess p;
		std::string error;
		SpawnOptions options;
		options.argv = { (dir / "no-such-program").u8string() };
		TEST_EXPECT(!p.spawn(options, error));
		TEST_EXPECT(!error.empty() && !p.started());
	}
	{
		ChildProcess p;
		std::string error;
		SpawnOptions options = child({ "exit", "0" });
		options.working_dir = (dir / "no-such-dir").u8string();
		TEST_EXPECT(!p.spawn(options, error));
		TEST_EXPECT(!error.empty() && !p.started());
	}
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(!p.spawn(SpawnOptions{}, error) && !error.empty());
	}
	return 0;
}

#ifndef _WIN32
int test_posix_descriptors_and_destructor(const fs::path &dir) {
	// A descriptor this process holds without close-on-exec stays here.
	const int held = ::open((dir / "held.txt").c_str(), O_WRONLY | O_CREAT, 0600);
	TEST_EXPECT(held >= 0);
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "fds" }, (dir / "fds.txt").u8string()), error));
		TEST_EXPECT(p.wait(20000) == 0);
	}
	::close(held);
	TEST_EXPECT(read_lines(dir / "fds.txt") == std::vector<std::string>{ "0" });

	// Destroying a ChildProcess whose child runs kills and reaps it.
	int64_t pid = 0;
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "sleep" }), error));
		pid = p.pid();
	}
	TEST_EXPECT(pid > 0);
	TEST_EXPECT(::kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH);
	return 0;
}
#endif

} // namespace

int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--child") == 0) return child_main(argc, argv);
	if (argc < 2) {
		std::fprintf(stderr, "usage: app_process_test <this binary's path>\n");
		return 2;
	}
	g_self = argv[1];
	test_temp::TempDir temp("app_process");
	TEST_EXPECT(test_exit_codes() == 0);
	TEST_EXPECT(test_arguments_arrive_whole(temp.path) == 0);
	TEST_EXPECT(test_working_dir_stdin_and_output(temp.path) == 0);
	TEST_EXPECT(test_terminate_and_kill() == 0);
	TEST_EXPECT(test_refusals(temp.path) == 0);
#ifndef _WIN32
	TEST_EXPECT(test_posix_descriptors_and_destructor(temp.path) == 0);
#endif
	std::printf("OK: child processes\n");
	return 0;
}
