// base/proc/child_process.h: a child started from an argv array with no shell, run
// against this test binary itself (ctest passes its path; `--child <mode>` makes
// it the child). Exit codes (unsigned: a Windows code past 0x7FFFFFFF reads as it
// is), every argument reaching the child as it was given (spaces, quotes,
// backslashes, an empty one), the working directory, stdin at end of file, stdout
// and stderr appended to the output file (stderr to an error file of its own,
// owner-only on POSIX, when one is named), poll while it runs, terminate (a
// windowless child still ends) and kill, the refusals (a missing program, a
// missing working directory, a second spawn while one runs), the environment
// (inherited by default, else exactly what the options name: a planted variable
// stays here), the identity and process_liveness (Alive with the child's own
// stamp, Dead with another or once it exited, Unknown with none; on Windows Dead
// for a child that exited with STILL_ACTIVE's 259, and the image spelled in full
// for a child started by its 8.3 path; on Linux the boot id in the stamp),
// release (refused for a child that ends with this process; one that does not
// keeps running past its ChildProcess and past the kill-on-close job of the
// process that started it, and is watched until it ends on its own), on Windows
// the polite stop (WM_CLOSE to a window child's unowned top-level window alone,
// the child started behind; polite only, nothing delivered to a windowless one;
// where a window can be shown at all)
// and the paths past MAX_PATH (the short form, or the refusal that says why),
// and on POSIX the released child's own session, no descriptor of this process
// reaching the child and the destructor killing a running one.

#include <base/proc/child_process.h>

#include <algorithm>
#include <cctype>

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

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <base/io/os_path.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using opennova::proc::ChildProcess;
using opennova::proc::Liveness;
using opennova::proc::process_liveness;
using opennova::proc::ProcessIdentity;
using opennova::proc::SpawnOptions;

namespace {

std::string g_self;

// --- the child's side ---------------------------------------------------------

#ifdef _WIN32
// The window child's top-level unowned window: WM_CLOSE is its orderly quit (exit 0).
LRESULT CALLBACK main_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
	if (message == WM_CLOSE) {
		DestroyWindow(window);
		return 0;
	}
	if (message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

// Its owned window: terminate() asks unowned windows alone, so a WM_CLOSE here
// (which would arrive first: an owned window sits above its owner) exits 5.
LRESULT CALLBACK owned_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
	if (message == WM_CLOSE)
		ExitProcess(5);
	return DefWindowProcW(window, message, wparam, lparam);
}

// A GUI child: an unowned top-level window shown the way it was started
// (SW_SHOWDEFAULT: STARTUPINFO's, SW_SHOWNOACTIVATE when started behind) and a
// hidden window it owns, both off screen with no taskbar button. Reports
// "ready shown=<0|1> foreground=<0|1>" once they are up, then runs until WM_CLOSE.
int window_child() {
	const HINSTANCE instance = GetModuleHandleW(nullptr);
	WNDCLASSW main_class{};
	main_class.lpfnWndProc = main_window_proc;
	main_class.hInstance = instance;
	main_class.lpszClassName = L"OpenNovaChildProcessTestMain";
	WNDCLASSW owned_class{};
	owned_class.lpfnWndProc = owned_window_proc;
	owned_class.hInstance = instance;
	owned_class.lpszClassName = L"OpenNovaChildProcessTestOwned";
	if (RegisterClassW(&main_class) == 0 || RegisterClassW(&owned_class) == 0)
		return 90;
	const HWND top =
			CreateWindowExW(WS_EX_TOOLWINDOW, main_class.lpszClassName, L"child_process_test",
					WS_OVERLAPPEDWINDOW, -4000, -4000, 64, 64, nullptr, nullptr, instance, nullptr);
	const HWND owned = top == nullptr
			? nullptr
			: CreateWindowExW(WS_EX_TOOLWINDOW, owned_class.lpszClassName, L"owned",
					  WS_OVERLAPPEDWINDOW, -4000, -4000, 64, 64, top, nullptr, instance, nullptr);
	if (top == nullptr || owned == nullptr)
		return 91;
	ShowWindow(top, SW_SHOWDEFAULT);
	std::printf("ready shown=%d foreground=%d\n", IsWindowVisible(top) ? 1 : 0,
			GetForegroundWindow() == top ? 1 : 0);
	std::fflush(stdout);
	MSG message;
	while (GetMessageW(&message, nullptr, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return static_cast<int>(message.wParam);
}
#endif

int child_main(int argc, char **argv) {
	const std::string mode = argc > 2 ? argv[2] : "";
	// The code as an unsigned number: a Windows process exits with all 32 bits.
	if (mode == "exit")
		return argc > 3 ? static_cast<int>(std::strtoul(argv[3], nullptr, 10)) : 0;
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
	if (mode == "nap") {
		std::this_thread::sleep_for(std::chrono::milliseconds(argc > 3 ? std::atoi(argv[3]) : 0));
		return 0;
	}
	if (mode == "env") {
		for (const std::string &entry : opennova::proc::current_environment()) std::printf("%s\n", entry.c_str());
		return 0;
	}
	if (mode == "release") {
		// Starts this binary napping as a child meant to outlive it, lets it go,
		// reports "<pid> <created>" and exits: the parent's kill-on-close job (this
		// process's) closes with that exit.
		ChildProcess napper;
		SpawnOptions options;
		options.argv = { argv[0], "--child", "nap", argc > 3 ? argv[3] : "1500" };
		options.end_with_parent = false;
		std::string error;
		if (!napper.spawn(options, error)) {
			std::printf("error %s\n", error.c_str());
			return 80;
		}
		ProcessIdentity id;
		const int64_t pid = napper.pid();
		if (!napper.identity(id) || !napper.release())
			return 81;
		std::printf("%lld %s\n", static_cast<long long>(pid), id.created.c_str());
		return 0;
	}
#ifdef _WIN32
	if (mode == "window")
		return window_child();
#endif
#ifndef _WIN32
	if (mode == "session") {
		std::printf("%lld\n", static_cast<long long>(::getsid(0)));
		return 0;
	}
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
	// POSIX keeps a code's low 8 bits alone; Windows all 32, so a code such as
	// 0xC0000005 (an access violation's) must not read negative.
#ifdef _WIN32
	const uint32_t codes[] = { 0u, 7u, 42u, 0xC0000005u };
#else
	const uint32_t codes[] = { 0u, 7u, 42u, 255u };
#endif
	for (const uint32_t code : codes) {
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
	{
		// Behind: the first window shown without activation; a console child
		// has none and runs as ever (ignored on POSIX).
		ChildProcess p;
		std::string error;
		SpawnOptions options = child({ "exit", "3" });
		options.behind = true;
		TEST_EXPECT(p.spawn(options, error));
		TEST_EXPECT(p.wait(20000) == 3u);
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
	TEST_EXPECT(p.wait(20000) == 0u);
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
		TEST_EXPECT(p.wait(20000) == 0u);
		const auto lines = read_lines(dir / "cwd.txt");
		TEST_EXPECT(lines.size() == 1);
		std::error_code ec;
		TEST_EXPECT(fs::equivalent(fs::u8path(lines[0]), work, ec));
	}
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "stdin" }, (dir / "stdin.txt").u8string()), error));
		TEST_EXPECT(p.wait(20000) == 0u);
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
		TEST_EXPECT(p.wait(20000) == 0u);
		const auto lines = read_lines(dir / "both.txt");
		TEST_EXPECT(lines.size() == 3);
		TEST_EXPECT(lines[0] == "before" && lines[1] == "to stdout" && lines[2] == "to stderr");
	}
	{
		// An error file: stderr lands there, appended to what it held, apart
		// from stdout's file; on POSIX it is created owner-only.
		{
			std::ofstream seed(dir / "err.txt", std::ios::binary);
			seed << "before\n";
		}
		ChildProcess p;
		SpawnOptions options = child({ "stderr" }, (dir / "out.txt").u8string());
		options.error_path = (dir / "err.txt").u8string();
		std::string error;
		TEST_EXPECT(p.spawn(options, error));
		TEST_EXPECT(p.wait(20000) == 0u);
		TEST_EXPECT(read_lines(dir / "out.txt") == std::vector<std::string>{ "to stdout" });
		TEST_EXPECT(
				read_lines(dir / "err.txt") == (std::vector<std::string>{ "before", "to stderr" }));
#ifndef _WIN32
		ChildProcess fresh;
		SpawnOptions fresh_options = child({ "stderr" }, (dir / "out_fresh.txt").u8string());
		fresh_options.error_path = (dir / "err_fresh.txt").u8string();
		TEST_EXPECT(fresh.spawn(fresh_options, error));
		TEST_EXPECT(fresh.wait(20000) == 0u);
		const fs::perms others = fs::perms::group_all | fs::perms::others_all;
		TEST_EXPECT((fs::status(dir / "err_fresh.txt").permissions() & others) == fs::perms::none);
#endif
	}
	{
		// An error file that cannot open: nothing starts.
		ChildProcess p;
		SpawnOptions options = child({ "stderr" }, (dir / "out_refused.txt").u8string());
		options.error_path = (dir / "no such dir" / "err.txt").u8string();
		std::string error;
		TEST_EXPECT(!p.spawn(options, error) && !error.empty() && !p.started());
	}
	{
		// No output file: the output is discarded and the child still runs.
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "args", "discarded" }), error));
		TEST_EXPECT(p.wait(20000) == 0u);
	}
	return 0;
}

int test_terminate_and_kill() {
	{
		// The test binary is a console child with no window: on Windows
		// terminate() has no window to ask and ends it with TerminateProcess.
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "sleep" }), error));
		TEST_EXPECT(!p.poll().has_value());
		TEST_EXPECT(!p.wait(200).has_value());
		// One child at a time.
		std::string second_error;
		TEST_EXPECT(!p.spawn(child({ "exit", "0" }), second_error) && !second_error.empty());
		TEST_EXPECT(p.terminate());
		const auto code = p.wait(20000);
		TEST_EXPECT(code.has_value());
#ifdef _WIN32
		TEST_EXPECT(*code == 1u);
#else
		TEST_EXPECT(*code == 128u + SIGTERM);
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
		TEST_EXPECT(*code == 1u);
#else
		TEST_EXPECT(*code == 128u + SIGKILL);
#endif
		// Once exited, both are no-ops.
		TEST_EXPECT(!p.terminate());
		p.kill();
		TEST_EXPECT(p.poll() == code);
	}
	{
		// Polite only: a windowless Windows child has no request to take, so
		// nothing is delivered and nothing is forced; SIGTERM is the request on
		// POSIX.
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "sleep" }), error));
#ifdef _WIN32
		TEST_EXPECT(!p.terminate(true));
		TEST_EXPECT(!p.wait(300).has_value());
		p.kill();
		TEST_EXPECT(p.wait(20000) == 1u);
#else
		TEST_EXPECT(p.terminate(true));
		TEST_EXPECT(p.wait(20000) == 128u + SIGTERM);
#endif
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
		// The OS's own error rides the message.
#ifdef _WIN32
		TEST_EXPECT(error.find("CreateProcessW failed (Windows error 2)") != std::string::npos);
#else
		TEST_EXPECT(error.find("cannot start") != std::string::npos);
#endif
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

void plant(const char *name, const char *value) {
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

bool has_line(const std::vector<std::string> &lines, const std::string &want) {
	return std::find(lines.begin(), lines.end(), want) != lines.end();
}

bool has_name(const std::vector<std::string> &lines, const std::string &name) {
	return std::any_of(lines.begin(), lines.end(),
			[&name](const std::string &l) { return l.compare(0, name.size() + 1, name + "=") == 0; });
}

// The environment: inherited whole by default; with `environment` set, exactly
// what it names (a planted variable stays here), and filter_environment's exact
// names and '*' prefixes.
int test_environment(const fs::path &dir) {
	plant("OPENNOVA_PROCESS_PLANTED", "hunter2");
	plant("KEEP_PREFIX_A", "1");
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "env" }, (dir / "env_inherited.txt").u8string()), error));
		TEST_EXPECT(p.wait(20000) == 0u);
		TEST_EXPECT(has_line(read_lines(dir / "env_inherited.txt"), "OPENNOVA_PROCESS_PLANTED=hunter2"));
	}
	{
		ChildProcess p;
		std::string error;
		SpawnOptions options = child({ "env" }, (dir / "env_own.txt").u8string());
		options.environment = opennova::proc::filter_environment({ "PATH", "SystemRoot", "KEEP_*" });
		options.environment->push_back("KEEP_ME=kept");
		TEST_EXPECT(p.spawn(options, error));
		TEST_EXPECT(p.wait(20000) == 0u);
		const auto lines = read_lines(dir / "env_own.txt");
		TEST_EXPECT(has_line(lines, "KEEP_ME=kept") && has_line(lines, "KEEP_PREFIX_A=1"));
		TEST_EXPECT(!has_name(lines, "OPENNOVA_PROCESS_PLANTED"));
	}
	const auto prefixed = opennova::proc::filter_environment({ "KEEP_*" });
	TEST_EXPECT(has_line(prefixed, "KEEP_PREFIX_A=1") && !has_name(prefixed, "OPENNOVA_PROCESS_PLANTED"));
	TEST_EXPECT(!has_line(opennova::proc::filter_environment({ "KEEP_PREFIX" }), "KEEP_PREFIX_A=1"));
	TEST_EXPECT(has_line(opennova::proc::current_environment(), "OPENNOVA_PROCESS_PLANTED=hunter2"));
	return 0;
}

bool ends_with(const std::string &text, const std::string &tail) {
	return text.size() >= tail.size() &&
			text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

#ifdef _WIN32
// Two Windows paths naming the same file by their spelling, case aside.
bool same_path(const std::string &a, const std::string &b) {
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) ==
				std::tolower(static_cast<unsigned char>(y));
	});
}

// Whether a process this one starts may break away from the job this one is
// in (true outside any job).
bool can_break_away() {
	BOOL in_job = FALSE;
	if (IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) == 0 || in_job == FALSE)
		return true;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	if (QueryInformationJobObject(
				nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) == 0)
		return false;
	return (limits.BasicLimitInformation.LimitFlags &
				   (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) != 0;
}
#endif

#if defined(_WIN32) || defined(__linux__)

// Polls process_liveness until it reads Dead (true) or `timeout_ms` passes.
bool becomes_dead(int64_t pid, const std::string &created, int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (process_liveness(pid, created) == Liveness::Dead)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return process_liveness(pid, created) == Liveness::Dead;
}

// The identity (the image this binary is, a creation stamp read the same twice)
// and process_liveness against it, while the child runs and once it exited.
int test_identity_and_liveness() {
	ChildProcess p;
	std::string error;
	TEST_EXPECT(p.spawn(child({ "sleep" }), error));
	ProcessIdentity first;
	ProcessIdentity second;
	TEST_EXPECT(p.identity(first) && p.identity(second));
	TEST_EXPECT(!first.created.empty() && first.created == second.created &&
			first.image == second.image);
	const std::string name = fs::u8path(g_self).filename().u8string();
	if (!ends_with(first.image, "/" + name))
		std::fprintf(
				stderr, "  image %s, want one ending /%s\n", first.image.c_str(), name.c_str());
	TEST_EXPECT(ends_with(first.image, "/" + name));
	const int64_t pid = p.pid();
	TEST_EXPECT(process_liveness(pid, first.created) == Liveness::Alive);
	TEST_EXPECT(process_liveness(pid, first.created + "1") == Liveness::Dead); // a recycled pid's
	TEST_EXPECT(process_liveness(pid, "") == Liveness::Unknown);
	TEST_EXPECT(process_liveness(0, first.created) == Liveness::Unknown);
	p.kill();
	TEST_EXPECT(p.wait(20000).has_value());
	TEST_EXPECT(becomes_dead(pid, first.created, 20000));
	ProcessIdentity after;
#ifdef _WIN32
	// The handle stays held past the exit: the same identity reads through it.
	TEST_EXPECT(p.identity(after) && after.created == first.created && after.image == first.image);
	{
		// A process may exit with STILL_ACTIVE's 259: Dead all the same (asked by
		// waiting on it), while the handle held here keeps it listed.
		ChildProcess q;
		TEST_EXPECT(q.spawn(child({ "exit", "259" }), error));
		ProcessIdentity id;
		TEST_EXPECT(q.identity(id));
		const int64_t qpid = q.pid();
		TEST_EXPECT(q.wait(20000) == 259u);
		TEST_EXPECT(process_liveness(qpid, id.created) == Liveness::Dead);
	}
	{
		// Started by its 8.3 path, the image still reads in full, the same as
		// started by its long one.
		std::wstring self = opennova::io::widen_utf8(g_self);
		std::replace(self.begin(), self.end(), L'/', L'\\');
		std::wstring shorter(32768, L'\0');
		const DWORD written =
				GetShortPathNameW(self.c_str(), shorter.data(), static_cast<DWORD>(shorter.size()));
		shorter.resize(written < shorter.size() ? written : 0);
		if (shorter.empty() || shorter == self) {
			std::fprintf(stderr, "  no 8.3 form of %s: the long-image check is moot here\n",
					g_self.c_str());
		} else {
			ChildProcess q;
			SpawnOptions options;
			options.argv = { opennova::io::narrow_utf8(shorter), "--child", "sleep" };
			TEST_EXPECT(q.spawn(options, error));
			ProcessIdentity id;
			TEST_EXPECT(q.identity(id));
			if (!same_path(id.image, first.image))
				std::fprintf(
						stderr, "  image %s, want %s\n", id.image.c_str(), first.image.c_str());
			TEST_EXPECT(same_path(id.image, first.image));
			q.kill();
			TEST_EXPECT(q.wait(20000).has_value());
		}
	}
#else
	// Reaped: its pid may already be another's, so there is none to read.
	TEST_EXPECT(!p.identity(after));
	// The stamp carries this boot's id, so a reboot's reused pid and starttime
	// never match.
	std::ifstream boot("/proc/sys/kernel/random/boot_id");
	std::string boot_id;
	std::getline(boot, boot_id);
	TEST_EXPECT(
			!boot_id.empty() && first.created.compare(0, boot_id.size() + 1, boot_id + ":") == 0);
#endif
	return 0;
}

// release(): refused for a child that ends with this process while it runs;
// one started with end_with_parent false is let go and keeps running (past this
// object's destruction too, and past the exit of a process that started it from
// inside a kill-on-close job, whose close would end a child still in it), and is
// watched through process_liveness until it ends on its own, so the test leaves
// nothing behind.
int test_release(const fs::path &dir) {
	{
		ChildProcess p;
		TEST_EXPECT(p.release()); // nothing held
		std::string error;
		TEST_EXPECT(p.spawn(child({ "sleep" }), error));
		TEST_EXPECT(!p.release() && p.started());
		p.kill();
		TEST_EXPECT(p.wait(20000).has_value());
		TEST_EXPECT(p.release() && !p.started() && p.pid() == 0 && !p.exit_code());
	}
	int64_t pid = 0;
	ProcessIdentity id;
	{
		ChildProcess p;
		std::string error;
		SpawnOptions options = child({ "nap", "1500" });
		options.end_with_parent = false;
		TEST_EXPECT(p.spawn(options, error));
		TEST_EXPECT(p.identity(id));
		pid = p.pid();
		TEST_EXPECT(p.release());
		TEST_EXPECT(!p.started() && p.pid() == 0 && !p.exit_code() && !p.poll());
		ProcessIdentity none;
		TEST_EXPECT(!p.identity(none));
	}
	TEST_EXPECT(process_liveness(pid, id.created) == Liveness::Alive);
	TEST_EXPECT(becomes_dead(pid, id.created, 20000));
	{
		// The starter runs in this process's kill-on-close job (end_with_parent)
		// and releases a napping child of its own; the job closes once the
		// starter's exit is seen here.
		ChildProcess starter;
		std::string error;
		const fs::path out = dir / "released.txt";
		TEST_EXPECT(starter.spawn(child({ "release", "2000" }, out.u8string()), error));
		TEST_EXPECT(starter.wait(20000) == 0u);
		const auto lines = read_lines(out);
		TEST_EXPECT(lines.size() == 1);
		const size_t space = lines[0].find(' ');
		TEST_EXPECT(space != std::string::npos);
		const int64_t napper = std::strtoll(lines[0].c_str(), nullptr, 10);
		const std::string created = lines[0].substr(space + 1);
		TEST_EXPECT(napper > 0 && !created.empty());
#ifdef _WIN32
		if (!can_break_away()) {
			// This process's own job allows no breakaway, so the napper stayed in
			// the starter's job: nothing to show here but that it is gone.
			std::fprintf(
					stderr, "  this process's job allows no breakaway: survival not checked\n");
			TEST_EXPECT(becomes_dead(napper, created, 20000));
			return 0;
		}
#endif
		TEST_EXPECT(process_liveness(napper, created) == Liveness::Alive);
		TEST_EXPECT(becomes_dead(napper, created, 20000));
	}
	return 0;
}

#endif

#ifdef _WIN32
// The window child's "ready ..." line in `path`, once it is up; empty when it
// exited first or did not report within 20 s.
std::string wait_ready(const fs::path &path, ChildProcess &p) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (std::chrono::steady_clock::now() < deadline) {
		for (const std::string &line : read_lines(path)) {
			if (line.rfind("ready", 0) == 0)
				return line;
		}
		if (p.poll())
			return std::string();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return std::string();
}

// Whether this process's window station shows windows: an interactive one (a
// logged-on session's). A runner started as a service has none (RunsOn's
// Windows runners, unlike GitHub's hosted ones), and a window there is never
// shown.
bool interactive_window_station() {
	USEROBJECTFLAGS flags{};
	return GetUserObjectInformationW(
				   GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), nullptr) != 0 &&
			(flags.dwFlags & WSF_VISIBLE) != 0;
}

// terminate() posts WM_CLOSE to the window child's unowned top-level window
// alone (its owned window's WM_CLOSE would exit 5), and the child's orderly quit
// exits 0; polite only, the same. Started behind: its window shown, not the
// foreground. Skipped, saying so, where no window can be shown.
int test_window_stop(const fs::path &dir) {
	if (!interactive_window_station()) {
		std::printf("skip: the window stop: no interactive window station shows a window\n");
		return 0;
	}
	for (const bool polite_only : { false, true }) {
		ChildProcess p;
		std::string error;
		const fs::path out = dir / (polite_only ? "window_polite.txt" : "window.txt");
		SpawnOptions options = child({ "window" }, out.u8string());
		options.behind = true;
		TEST_EXPECT(p.spawn(options, error));
		const std::string ready = wait_ready(out, p);
		if (ready != "ready shown=1 foreground=0")
			std::fprintf(stderr, "  window child: '%s', exit %d\n", ready.c_str(),
					p.exit_code() ? static_cast<int>(*p.exit_code()) : -1);
		TEST_EXPECT(ready == "ready shown=1 foreground=0");
		TEST_EXPECT(p.terminate(polite_only));
		TEST_EXPECT(p.wait(20000) == 0u);
	}
	return 0;
}

// Paths past MAX_PATH: a program that has no short form (it does not exist) is
// refused with that reason; a working directory that exists runs through its
// short form where the volume keeps 8.3 names, else is refused saying why.
int test_long_paths(const fs::path &dir) {
	const std::string a(100, 'a');
	const std::string b(100, 'b');
	const std::string c(100, 'c');
	{
		ChildProcess p;
		std::string error;
		SpawnOptions options;
		options.argv = { (dir / a / b / c / "program.exe").generic_u8string() };
		TEST_EXPECT(!p.spawn(options, error) && !p.started());
		TEST_EXPECT(error.find("past the 259 characters") != std::string::npos &&
				error.find("no short (8.3) form") != std::string::npos);
	}
	const fs::path work = dir / a / b / c;
	std::error_code ec;
	fs::create_directories(opennova::io::os_path(work.u8string()), ec);
	TEST_EXPECT(!ec);
	ChildProcess p;
	std::string error;
	SpawnOptions options = child({ "cwd" }, (dir / "long_cwd.txt").u8string());
	options.working_dir = work.generic_u8string(); // '/' separators, spelled '\' at the call
	if (p.spawn(options, error)) {
		TEST_EXPECT(p.wait(20000) == 0u);
		const auto lines = read_lines(dir / "long_cwd.txt");
		TEST_EXPECT(lines.size() == 1);
		TEST_EXPECT(lines[0].size() <= 258);
		TEST_EXPECT(fs::equivalent(
				opennova::io::os_path(lines[0]), opennova::io::os_path(work.u8string()), ec));
	} else {
		std::fprintf(stderr, "  no 8.3 names on this volume: %s\n", error.c_str());
		TEST_EXPECT(error.find("the working directory") != std::string::npos &&
				error.find("no short (8.3) form") != std::string::npos);
	}
	return 0;
}
#endif

#ifndef _WIN32
// A child meant to outlive this process leads a session of its own (a hangup
// of this terminal misses it); one that ends with it stays in this one.
int test_posix_session(const fs::path &dir) {
	for (const bool end_with_parent : { false, true }) {
		ChildProcess p;
		std::string error;
		const fs::path out = dir / (end_with_parent ? "session_tied.txt" : "session_own.txt");
		SpawnOptions options = child({ "session" }, out.u8string());
		options.end_with_parent = end_with_parent;
		TEST_EXPECT(p.spawn(options, error));
		const int64_t pid = p.pid();
		TEST_EXPECT(p.wait(20000) == 0u);
		const auto lines = read_lines(out);
		TEST_EXPECT(lines.size() == 1);
		const long long sid = std::strtoll(lines[0].c_str(), nullptr, 10);
		TEST_EXPECT(end_with_parent ? sid == static_cast<long long>(::getsid(0)) : sid == pid);
	}
	return 0;
}

int test_posix_descriptors_and_destructor(const fs::path &dir) {
	// A descriptor this process holds without close-on-exec stays here.
	const int held = ::open((dir / "held.txt").c_str(), O_WRONLY | O_CREAT, 0600);
	TEST_EXPECT(held >= 0);
	{
		ChildProcess p;
		std::string error;
		TEST_EXPECT(p.spawn(child({ "fds" }, (dir / "fds.txt").u8string()), error));
		TEST_EXPECT(p.wait(20000) == 0u);
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
		std::fprintf(stderr, "usage: child_process_test <this binary's path>\n");
		return 2;
	}
	g_self = argv[1];
	test_temp::TempDir temp("child_process");
	TEST_EXPECT(test_exit_codes() == 0);
	TEST_EXPECT(test_arguments_arrive_whole(temp.path) == 0);
	TEST_EXPECT(test_working_dir_stdin_and_output(temp.path) == 0);
	TEST_EXPECT(test_terminate_and_kill() == 0);
	TEST_EXPECT(test_refusals(temp.path) == 0);
	TEST_EXPECT(test_environment(temp.path) == 0);
#if defined(_WIN32) || defined(__linux__)
	TEST_EXPECT(test_identity_and_liveness() == 0);
	TEST_EXPECT(test_release(temp.path) == 0);
#endif
#ifdef _WIN32
	TEST_EXPECT(test_window_stop(temp.path) == 0);
	TEST_EXPECT(test_long_paths(temp.path) == 0);
#endif
#ifndef _WIN32
	TEST_EXPECT(test_posix_session(temp.path) == 0);
	TEST_EXPECT(test_posix_descriptors_and_destructor(temp.path) == 0);
#endif
	std::printf("OK: child processes\n");
	return 0;
}
