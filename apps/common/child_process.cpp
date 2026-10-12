#include "child_process.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

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
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#endif

namespace opennova::proc {

namespace {

// How long the destructor waits for a killed child before it lets the handle go.
constexpr int kReapOnDestroyMs = 5000;

#ifdef _WIN32

// One argument as CommandLineToArgvW (and the MSVC CRT's argv parser) reads it
// back: quoted when it is empty or holds white space or a quote, a quote
// escaped with a backslash, and the backslashes before a quote (or before the
// closing quote) doubled.
std::wstring quote_argument(const std::wstring &arg) {
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
		return arg;
	std::wstring out = L"\"";
	for (size_t i = 0;; ++i) {
		size_t backslashes = 0;
		while (i < arg.size() && arg[i] == L'\\') {
			++backslashes;
			++i;
		}
		if (i == arg.size()) {
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (arg[i] == L'"') {
			out.append(backslashes * 2 + 1, L'\\');
		} else {
			out.append(backslashes, L'\\');
		}
		out.push_back(arg[i]);
	}
	out.push_back(L'"');
	return out;
}

std::string windows_error(const char *what, DWORD code) {
	return std::string(what) + " failed (Windows error " + std::to_string(code) + ")";
}

HANDLE handle_of(void *h) { return static_cast<HANDLE>(h); }

#else

// `fd` moved past the standard three (close-on-exec), so the child's dup2 onto
// 0..2 never lands on it: a process started with a standard descriptor closed
// hands the next open one of those numbers.
int above_standard(int fd) {
	if (fd < 0 || fd > 2) return fd;
	const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
	::close(fd);
	return moved;
}

#endif

} // namespace

ChildProcess::ChildProcess() = default;

ChildProcess::~ChildProcess() {
	if (started()) {
		kill();
		wait(kReapOnDestroyMs);
	}
	reset();
}

ChildProcess::ChildProcess(ChildProcess &&other) noexcept { *this = std::move(other); }

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept {
	if (this == &other) return *this;
	if (started()) {
		kill();
		wait(kReapOnDestroyMs);
	}
	reset();
	pid_ = other.pid_;
	exit_code_ = other.exit_code_;
	other.pid_ = 0;
	other.exit_code_.reset();
#ifdef _WIN32
	process_ = other.process_;
	job_ = other.job_;
	other.process_ = nullptr;
	other.job_ = nullptr;
#endif
	return *this;
}

void ChildProcess::reset() {
#ifdef _WIN32
	if (process_ != nullptr) CloseHandle(handle_of(process_));
	// Closing the job ends a child still in it (kill-on-close); the destructor
	// has killed and reaped it by then.
	if (job_ != nullptr) CloseHandle(handle_of(job_));
	process_ = nullptr;
	job_ = nullptr;
#endif
	pid_ = 0;
}

bool ChildProcess::started() const {
#ifdef _WIN32
	return process_ != nullptr;
#else
	return pid_ > 0;
#endif
}

int64_t ChildProcess::pid() const { return pid_; }

#ifdef _WIN32

bool ChildProcess::spawn(const SpawnOptions &options, std::string &error) {
	if (started()) {
		error = "a child is already running";
		return false;
	}
	if (options.argv.empty() || options.argv[0].empty()) {
		error = "no program to run";
		return false;
	}
	reset();
	exit_code_.reset();
	const std::wstring program = io::widen_utf8(options.argv[0]);
	std::wstring command_line;
	for (size_t i = 0; i < options.argv.size(); ++i) {
		if (i != 0) command_line.push_back(L' ');
		command_line += quote_argument(io::widen_utf8(options.argv[i]));
	}
	const std::wstring cwd =
			options.working_dir.empty() ? std::wstring() : io::widen_utf8(options.working_dir);

	// The two handles the child gets, inheritable, and the attribute list that
	// hands it those alone (every other inheritable handle stays here).
	SECURITY_ATTRIBUTES inheritable{};
	inheritable.nLength = sizeof(inheritable);
	inheritable.bInheritHandle = TRUE;
	HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
			OPEN_EXISTING, 0, nullptr);
	HANDLE out = options.output_path.empty()
			? CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
					  OPEN_EXISTING, 0, nullptr)
			: CreateFileW(io::widen_utf8(options.output_path).c_str(), FILE_APPEND_DATA,
					  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable, OPEN_ALWAYS,
					  FILE_ATTRIBUTE_NORMAL, nullptr);
	auto close_std = [&] {
		if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
		if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
	};
	if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) {
		error = windows_error(in == INVALID_HANDLE_VALUE ? "opening NUL" : "opening the output file",
				GetLastError());
		close_std();
		return false;
	}
	SIZE_T attributes_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
	std::vector<unsigned char> attributes_buffer(attributes_size);
	auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_buffer.data());
	if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributes_size)) {
		error = windows_error("InitializeProcThreadAttributeList", GetLastError());
		close_std();
		return false;
	}
	HANDLE inherited[2] = { in, out };
	if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
				sizeof(inherited), nullptr, nullptr)) {
		error = windows_error("UpdateProcThreadAttribute", GetLastError());
		DeleteProcThreadAttributeList(attributes);
		close_std();
		return false;
	}

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = in;
	startup.StartupInfo.hStdOutput = out;
	startup.StartupInfo.hStdError = out;
	startup.lpAttributeList = attributes;
	// Suspended until it is in its job, so it cannot start a grandchild outside it.
	DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
	if (options.end_with_parent) flags |= CREATE_SUSPENDED;
	std::vector<wchar_t> command(command_line.begin(), command_line.end());
	command.push_back(L'\0');
	PROCESS_INFORMATION info{};
	const BOOL created = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
			flags, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
	const DWORD create_error = GetLastError();
	DeleteProcThreadAttributeList(attributes);
	close_std();
	if (!created) {
		error = windows_error("CreateProcessW", create_error);
		return false;
	}
	if (options.end_with_parent) {
		// Best effort: a process already in a job that refuses nesting runs
		// without one.
		HANDLE job = CreateJobObjectW(nullptr, nullptr);
		if (job != nullptr) {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
						sizeof(limits)) ||
					!AssignProcessToJobObject(job, info.hProcess)) {
				CloseHandle(job);
				job = nullptr;
			}
		}
		job_ = job;
		ResumeThread(info.hThread);
	}
	CloseHandle(info.hThread);
	process_ = info.hProcess;
	pid_ = static_cast<int64_t>(info.dwProcessId);
	return true;
}

std::optional<int> ChildProcess::poll() { return wait(0); }

std::optional<int> ChildProcess::wait(int timeout_ms) {
	if (!started()) return exit_code_;
	const DWORD waited = WaitForSingleObject(
			handle_of(process_), timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
	if (waited != WAIT_OBJECT_0) return std::nullopt;
	DWORD code = 0;
	if (!GetExitCodeProcess(handle_of(process_), &code)) code = 1;
	exit_code_ = static_cast<int>(code);
	reset();
	return exit_code_;
}

void ChildProcess::terminate() { kill(); }

void ChildProcess::kill() {
	if (started()) TerminateProcess(handle_of(process_), 1);
}

#else // POSIX

bool ChildProcess::spawn(const SpawnOptions &options, std::string &error) {
	if (started()) {
		error = "a child is already running";
		return false;
	}
	if (options.argv.empty() || options.argv[0].empty()) {
		error = "no program to run";
		return false;
	}
	reset();
	exit_code_.reset();
	// Everything the child touches is made here, before the fork: the child of a
	// threaded process may call async-signal-safe functions alone.
	std::vector<std::string> args = options.argv;
	std::vector<char *> argv;
	argv.reserve(args.size() + 1);
	for (std::string &arg : args) argv.push_back(arg.data());
	argv.push_back(nullptr);
	const char *cwd = options.working_dir.empty() ? nullptr : options.working_dir.c_str();
	const int in_fd = above_standard(::open("/dev/null", O_RDONLY | O_CLOEXEC));
	const int out_fd = above_standard(options.output_path.empty()
					? ::open("/dev/null", O_WRONLY | O_CLOEXEC)
					: ::open(options.output_path.c_str(),
							  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
	if (in_fd < 0 || out_fd < 0) {
		error = std::string(in_fd < 0 ? "opening /dev/null: " : "opening the output file: ") +
				std::strerror(errno);
		if (in_fd >= 0) ::close(in_fd);
		if (out_fd >= 0) ::close(out_fd);
		return false;
	}
	// The child reports a failed exec through this pipe; a successful exec
	// closes it (close-on-exec), and the parent reads end of file.
	int report[2] = { -1, -1 };
	if (::pipe(report) != 0) {
		error = std::string("pipe: ") + std::strerror(errno);
		::close(in_fd);
		::close(out_fd);
		return false;
	}
	::fcntl(report[0], F_SETFD, FD_CLOEXEC);
	::fcntl(report[1], F_SETFD, FD_CLOEXEC);
	report[0] = above_standard(report[0]);
	report[1] = above_standard(report[1]);
	if (report[0] < 0 || report[1] < 0) {
		error = "pipe: no descriptor past the standard three";
		for (const int fd : { in_fd, out_fd, report[0], report[1] }) {
			if (fd >= 0) ::close(fd);
		}
		return false;
	}
	long open_max = ::sysconf(_SC_OPEN_MAX);
	if (open_max < 0 || open_max > 65536) open_max = 65536;
	const int last_fd = static_cast<int>(open_max);
	const pid_t parent = ::getpid();
	const bool end_with_parent = options.end_with_parent;

	// Signals stay blocked across the fork, so the child runs none of this
	// process's handlers before it resets them.
	sigset_t all;
	sigset_t saved;
	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &saved);
	const pid_t pid = ::fork();
	if (pid == 0) {
		const int report_fd = report[1];
		auto fail = [report_fd](int code) {
			const ssize_t written = ::write(report_fd, &code, sizeof(code));
			(void)written;
			::_exit(127);
		};
#ifdef __linux__
		if (end_with_parent) {
			::prctl(PR_SET_PDEATHSIG, SIGKILL);
			if (::getppid() != parent) ::_exit(127);
		}
#else
		(void)end_with_parent;
		(void)parent;
#endif
		// Handlers reset at exec on their own; an ignored signal stays ignored.
		const int defaults[] = { SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD };
		for (const int sig : defaults) ::signal(sig, SIG_DFL);
		sigset_t none;
		sigemptyset(&none);
		::sigprocmask(SIG_SETMASK, &none, nullptr);
		if (::dup2(in_fd, 0) < 0 || ::dup2(out_fd, 1) < 0 || ::dup2(out_fd, 2) < 0) fail(errno);
		if (cwd != nullptr && ::chdir(cwd) != 0) fail(errno);
		// Every descriptor past the standard three but the report pipe goes.
		bool closed = false;
#if defined(__linux__) && defined(SYS_close_range)
		closed = (report_fd <= 3 || ::syscall(SYS_close_range, 3u,
												static_cast<unsigned>(report_fd - 1), 0u) == 0) &&
				::syscall(SYS_close_range, static_cast<unsigned>(report_fd + 1), ~0u, 0u) == 0;
#endif
		if (!closed) {
			for (int fd = 3; fd < last_fd; ++fd) {
				if (fd != report_fd) ::close(fd);
			}
		}
		::execv(argv[0], argv.data());
		fail(errno);
	}
	const int fork_errno = errno;
	pthread_sigmask(SIG_SETMASK, &saved, nullptr);
	::close(in_fd);
	::close(out_fd);
	::close(report[1]);
	if (pid < 0) {
		::close(report[0]);
		error = std::string("fork: ") + std::strerror(fork_errno);
		return false;
	}
	int child_errno = 0;
	ssize_t got = 0;
	do {
		got = ::read(report[0], &child_errno, sizeof(child_errno));
	} while (got < 0 && errno == EINTR);
	::close(report[0]);
	if (got == static_cast<ssize_t>(sizeof(child_errno))) {
		int status = 0;
		::waitpid(pid, &status, 0);
		error = std::string("cannot start ") + options.argv[0] + ": " + std::strerror(child_errno);
		return false;
	}
	pid_ = static_cast<int64_t>(pid);
	return true;
}

std::optional<int> ChildProcess::poll() {
	if (!started()) return exit_code_;
	int status = 0;
	pid_t reaped = 0;
	do {
		reaped = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
	} while (reaped < 0 && errno == EINTR);
	if (reaped == 0) return std::nullopt;
	if (reaped < 0) {
		// Reaped elsewhere (a SIGCHLD set to SIG_IGN): its code is gone.
		exit_code_ = 1;
	} else if (WIFEXITED(status)) {
		exit_code_ = WEXITSTATUS(status);
	} else if (WIFSIGNALED(status)) {
		exit_code_ = 128 + WTERMSIG(status);
	} else {
		return std::nullopt; // stopped or continued, still there
	}
	pid_ = 0;
	return exit_code_;
}

std::optional<int> ChildProcess::wait(int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (;;) {
		if (auto code = poll()) return code;
		if (!started()) return exit_code_;
		if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) return std::nullopt;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void ChildProcess::terminate() {
	if (started()) ::kill(static_cast<pid_t>(pid_), SIGTERM);
}

void ChildProcess::kill() {
	if (started()) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

#endif

} // namespace opennova::proc
