#include "child_process.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <string_view>
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

#include <filesystem>

#include <base/io/os_path.h>
#else
#include <cerrno>
#include <climits>
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

// The process environment. POSIX defines `environ` but no header has to
// declare it (glibc's unistd.h does only under _GNU_SOURCE, macOS's never), so
// it is declared here, at global scope where it lives.
extern char **environ;
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

wchar_t upper(wchar_t c) { return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c; }

// A UTF-8 path as Windows spells it: UTF-16, every '/' a '\'.
std::wstring native_path(const std::string &path) {
	std::wstring out = io::widen_utf8(path);
	std::replace(out.begin(), out.end(), L'/', L'\\');
	return out;
}

// `path` without the \\?\ (or \\?\UNC\) prefix a call was asked through.
std::wstring without_extended_prefix(std::wstring path) {
	if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
		return L"\\\\" + path.substr(8);
	if (path.rfind(L"\\\\?\\", 0) == 0)
		path.erase(0, 4);
	return path;
}

// `path` as GetShortPathNameW (`to_short`) or GetLongPathNameW spells it, asked
// through the \\?\ form a long path needs; empty when the call fails (the file
// is not there).
std::wstring respelled(const std::wstring &path, bool to_short) {
	const std::wstring extended = io::os_path(std::filesystem::path(path)).native();
	const auto call = to_short ? GetShortPathNameW : GetLongPathNameW;
	const DWORD needed = call(extended.c_str(), nullptr, 0);
	if (needed == 0)
		return std::wstring();
	std::wstring out(static_cast<size_t>(needed), L'\0');
	const DWORD written = call(extended.c_str(), out.data(), needed);
	if (written == 0 || written >= needed)
		return std::wstring();
	out.resize(written);
	return without_extended_prefix(out);
}

// A path CreateProcessW takes: one past `limit` characters (the system's current
// directory holds MAX_PATH less its trailing backslash and terminator, an image
// MAX_PATH less its terminator) given in its short (8.3) form where the volume
// keeps one; empty where even that is past the limit (the spawn is refused,
// never handed a path it would cut).
std::wstring within(const std::wstring &path, size_t limit) {
	if (path.size() <= limit)
		return path;
	const std::wstring out = respelled(path, true);
	return out.size() <= limit ? out : std::wstring();
}

// The refusal of a path `within` found no form of: `what` names it.
std::string too_long(const std::string &program, const std::string &what, size_t limit) {
	return "cannot start " + program + ": " + what + " is past the " + std::to_string(limit) +
			" characters CreateProcessW takes and has no short (8.3) form within them";
}

// The process's creation time as the OS keeps it (a FILETIME's 100 ns count), in
// decimal: equal for one process, different for another that later takes its
// pid. "" when it cannot be read.
std::string creation_stamp(HANDLE handle) {
	FILETIME created{};
	FILETIME exited{};
	FILETIME kernel{};
	FILETIME user{};
	if (GetProcessTimes(handle, &created, &exited, &kernel, &user) == 0)
		return std::string();
	return std::to_string(
			(static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}

// The longest path Windows names (a UNICODE_STRING's 32767 characters and the
// terminator): room for any image path.
constexpr DWORD kLongestPath = 32768;

// The process's identity read through `handle` now. The image query fails once
// the process has exited, so the spawn reads it while the child is there. It
// reports the path the child was started by, an 8.3 one included (within()'s),
// so the image is spelled out in full (GetLongPathNameW) and compares equal
// however the child was started.
ProcessIdentity read_identity(HANDLE handle) {
	ProcessIdentity out;
	std::wstring image(kLongestPath, L'\0');
	DWORD length = static_cast<DWORD>(image.size());
	if (QueryFullProcessImageNameW(handle, 0, image.data(), &length) != 0) {
		image.resize(length);
		const std::wstring full = respelled(image, false);
		out.image = io::narrow_utf8(full.empty() ? image : full);
		std::replace(out.image.begin(), out.image.end(), '\\', '/');
	}
	out.created = creation_stamp(handle);
	return out;
}

// terminate()'s request: WM_CLOSE to each top-level unowned window of the
// process, counted.
struct CloseRequest {
	DWORD pid = 0;
	int posted = 0;
};

BOOL CALLBACK close_top_level_window(HWND window, LPARAM param) {
	CloseRequest &request = *reinterpret_cast<CloseRequest *>(param);
	DWORD owner = 0;
	GetWindowThreadProcessId(window, &owner);
	if (owner == request.pid && GetWindow(window, GW_OWNER) == nullptr &&
			PostMessageW(window, WM_CLOSE, 0, 0) != 0)
		++request.posted;
	return TRUE;
}

// CreateProcessW's environment block: each NAME=value in UTF-16, sorted by name
// case aside (the order an environment block keeps), each ended by a NUL, the
// block by one more.
std::vector<wchar_t> environment_block(const std::vector<std::string> &entries) {
	std::vector<std::wstring> wide;
	wide.reserve(entries.size());
	for (const std::string &e : entries) wide.push_back(io::widen_utf8(e));
	std::sort(wide.begin(), wide.end(), [](const std::wstring &a, const std::wstring &b) {
		const size_t an = a.find(L'=');
		const size_t bn = b.find(L'=');
		const std::wstring_view na(a.data(), an == std::wstring::npos ? a.size() : an);
		const std::wstring_view nb(b.data(), bn == std::wstring::npos ? b.size() : bn);
		for (size_t i = 0; i < na.size() && i < nb.size(); ++i) {
			if (upper(na[i]) != upper(nb[i])) return upper(na[i]) < upper(nb[i]);
		}
		return na.size() < nb.size();
	});
	std::vector<wchar_t> block;
	for (const std::wstring &w : wide) {
		block.insert(block.end(), w.begin(), w.end());
		block.push_back(L'\0');
	}
	if (block.empty()) block.push_back(L'\0');
	block.push_back(L'\0');
	return block;
}

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

// A released child's reaper: waits for its exit so it leaves no zombie behind.
// Not hot-reload safe: the detached thread runs this module's code until the
// child exits, so a module unloaded meanwhile (a reloaded GDExtension) would
// leave it running code that is gone.
void *reap_released(void *arg) {
	const pid_t pid = static_cast<pid_t>(reinterpret_cast<intptr_t>(arg));
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	return nullptr;
}

#ifdef __linux__

// /proc/<pid>/stat's state (field 3) and starttime (field 22, clock ticks after
// boot), read after the last ')': the command name in field 2 may hold spaces
// and parentheses. False when the file does not open (`missing` when no such
// process) or does not parse.
bool read_proc_stat(int64_t pid, char &state, std::string &starttime, bool &missing) {
	missing = false;
	const std::string path = "/proc/" + std::to_string(pid) + "/stat";
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		missing = errno == ENOENT || errno == ESRCH;
		return false;
	}
	std::string text;
	char buffer[1024];
	for (;;) {
		const ssize_t got = ::read(fd, buffer, sizeof(buffer));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		text.append(buffer, static_cast<size_t>(got));
	}
	::close(fd);
	const size_t close_paren = text.rfind(')');
	if (close_paren == std::string::npos)
		return false;
	// The fields after the name, from field 3 on, one space apart.
	std::vector<std::string> fields;
	size_t at = close_paren + 1;
	while (at < text.size()) {
		while (at < text.size() && (text[at] == ' ' || text[at] == '\n'))
			++at;
		const size_t end = text.find_first_of(" \n", at);
		if (at < text.size())
			fields.push_back(
					text.substr(at, end == std::string::npos ? std::string::npos : end - at));
		at = end == std::string::npos ? text.size() : end;
	}
	if (fields.size() < 20 || fields[0].empty())
		return false;
	state = fields[0][0];
	starttime = fields[19];
	return true;
}

// This boot's id (/proc/sys/kernel/random/boot_id), read once; "" when it
// cannot be read.
const std::string &boot_id() {
	static const std::string id = [] {
		std::string text;
		const int fd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			char buffer[64];
			ssize_t got = 0;
			do {
				got = ::read(fd, buffer, sizeof(buffer));
			} while (got < 0 && errno == EINTR);
			if (got > 0)
				text.assign(buffer, static_cast<size_t>(got));
			::close(fd);
		}
		while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
			text.pop_back();
		return text;
	}();
	return id;
}

// A process's creation stamp from its starttime: "<boot_id>:<starttime>", so a
// process of another boot that reuses the pid and the starttime never matches
// (the starttime alone where the boot id cannot be read).
std::string creation_stamp(const std::string &starttime) {
	const std::string &boot = boot_id();
	return boot.empty() ? starttime : boot + ":" + starttime;
}

// Process `pid`'s identity read from /proc now: /proc/<pid>/exe's target (gone
// once the process is a zombie) and its creation stamp.
ProcessIdentity read_identity(int64_t pid) {
	ProcessIdentity out;
	const std::string exe = "/proc/" + std::to_string(pid) + "/exe";
	std::string target(PATH_MAX, '\0');
	for (;;) {
		const ssize_t got = ::readlink(exe.c_str(), target.data(), target.size());
		if (got < 0)
			break;
		if (static_cast<size_t>(got) < target.size()) {
			out.image.assign(target.data(), static_cast<size_t>(got));
			break;
		}
		target.resize(target.size() * 2);
	}
	char state = 0;
	bool missing = false;
	std::string starttime;
	if (read_proc_stat(pid, state, starttime, missing))
		out.created = creation_stamp(starttime);
	return out;
}

#endif

#endif

// Whether `entry`'s name (before its '=') is `name`, or begins with it when
// `name` ends in '*'; case aside on Windows.
bool name_matches(const std::string &entry, const std::string &name) {
	const size_t eq = entry.find('=');
	const std::string entry_name = entry.substr(0, eq);
	const bool prefix = !name.empty() && name.back() == '*';
	const std::string want = prefix ? name.substr(0, name.size() - 1) : name;
	if (prefix ? entry_name.size() < want.size() : entry_name.size() != want.size()) return false;
	for (size_t i = 0; i < want.size(); ++i) {
		char a = entry_name[i];
		char b = want[i];
#ifdef _WIN32
		if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
		if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
#endif
		if (a != b) return false;
	}
	return true;
}

} // namespace

std::vector<std::string> current_environment() {
	std::vector<std::string> out;
#ifdef _WIN32
	wchar_t *block = GetEnvironmentStringsW();
	if (block == nullptr) return out;
	for (const wchar_t *entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
		if (entry[0] == L'=') continue; // the per-drive current directories
		out.push_back(io::narrow_utf8(entry));
	}
	FreeEnvironmentStringsW(block);
#else
	for (char **entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) out.emplace_back(*entry);
#endif
	return out;
}

std::vector<std::string> filter_environment(const std::vector<std::string> &names) {
	std::vector<std::string> out;
	for (const std::string &entry : current_environment()) {
		for (const std::string &name : names) {
			if (name_matches(entry, name)) {
				out.push_back(entry);
				break;
			}
		}
	}
	return out;
}

Liveness process_liveness(int64_t pid, const std::string &created) {
	if (pid <= 0)
		return Liveness::Unknown;
#ifdef _WIN32
	// Whether it has exited is asked by waiting on it (SYNCHRONIZE): its exit
	// code cannot tell, since a process may exit with STILL_ACTIVE's 259. One
	// that will not grant that right is asked through the query right alone,
	// and its exit code then has to do.
	const DWORD id = static_cast<DWORD>(pid);
	bool can_wait = true;
	HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id);
	if (handle == nullptr && GetLastError() == ERROR_ACCESS_DENIED) {
		can_wait = false;
		handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
	}
	if (handle == nullptr) {
		// No process of that id is Dead; one that will not be opened may be it: Unknown.
		return GetLastError() == ERROR_INVALID_PARAMETER ? Liveness::Dead : Liveness::Unknown;
	}
	Liveness state = Liveness::Unknown;
	DWORD code = 0;
	const bool exited = can_wait ? WaitForSingleObject(handle, 0) == WAIT_OBJECT_0
								 : GetExitCodeProcess(handle, &code) != 0 && code != STILL_ACTIVE;
	if (exited) {
		state = Liveness::Dead; // it has exited (a handle someone holds keeps it listed)
	} else if (!created.empty()) {
		const std::string now = creation_stamp(handle);
		if (!now.empty())
			state = now == created ? Liveness::Alive : Liveness::Dead;
	}
	CloseHandle(handle);
	return state;
#elif defined(__linux__)
	char state = 0;
	std::string starttime;
	bool missing = false;
	if (!read_proc_stat(pid, state, starttime, missing))
		return missing ? Liveness::Dead : Liveness::Unknown;
	if (state == 'Z' || state == 'X')
		return Liveness::Dead; // exited, not reaped yet
	if (created.empty())
		return Liveness::Unknown;
	return creation_stamp(starttime) == created ? Liveness::Alive : Liveness::Dead;
#else
	(void)created;
	return Liveness::Unknown;
#endif
}

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
	ends_with_parent_ = other.ends_with_parent_;
	identity_ = std::move(other.identity_);
	other.pid_ = 0;
	other.exit_code_.reset();
	other.identity_ = ProcessIdentity{};
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
	// has killed and reaped it by then, and release() refuses one that runs.
	if (job_ != nullptr) CloseHandle(handle_of(job_));
	process_ = nullptr;
	job_ = nullptr;
#endif
	pid_ = 0;
	ends_with_parent_ = false;
	identity_ = ProcessIdentity{};
}

bool ChildProcess::started() const {
#ifdef _WIN32
	return process_ != nullptr && !exit_code_.has_value();
#else
	return pid_ > 0;
#endif
}

int64_t ChildProcess::pid() const { return pid_; }

bool ChildProcess::release() {
	// A child that ends with this process cannot be let go while it runs; one
	// that has exited is reaped here and forgotten like any other.
	if (started() && ends_with_parent_ && !poll())
		return false;
#ifndef _WIN32
	if (started()) {
		// Reaped once it exits, so it leaves no zombie; with no thread to spare
		// it stays one until this process ends. Signals stay blocked in that
		// thread (it inherits the mask), so none of this process's land there.
		sigset_t all;
		sigset_t saved;
		sigfillset(&all);
		pthread_sigmask(SIG_SETMASK, &all, &saved);
		pthread_attr_t attributes;
		if (pthread_attr_init(&attributes) == 0) {
			pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
			pthread_t reaper;
			pthread_create(&reaper, &attributes, reap_released,
					reinterpret_cast<void *>(static_cast<intptr_t>(pid_)));
			pthread_attr_destroy(&attributes);
		}
		pthread_sigmask(SIG_SETMASK, &saved, nullptr);
	}
#endif
	reset();
	exit_code_.reset();
	return true;
}

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
	// The image and the current directory within what the call takes (above).
	const std::wstring program = within(native_path(options.argv[0]), MAX_PATH - 1);
	if (program.empty()) {
		error = too_long(options.argv[0], "its path", MAX_PATH - 1);
		return false;
	}
	const std::wstring cwd = options.working_dir.empty()
			? std::wstring()
			: within(native_path(options.working_dir), MAX_PATH - 2);
	if (!options.working_dir.empty() && cwd.empty()) {
		error = too_long(
				options.argv[0], "the working directory " + options.working_dir, MAX_PATH - 2);
		return false;
	}
	// argv[0] reaches the child as the path it runs.
	std::wstring command_line = quote_argument(program);
	for (size_t i = 1; i < options.argv.size(); ++i) {
		command_line.push_back(L' ');
		command_line += quote_argument(io::widen_utf8(options.argv[i]));
	}

	// The handles the child gets (its input, its output and, when the options
	// name one, its error file), inheritable, and the attribute list that hands
	// it those alone (every other inheritable handle stays here).
	SECURITY_ATTRIBUTES inheritable{};
	inheritable.nLength = sizeof(inheritable);
	inheritable.bInheritHandle = TRUE;
	HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
			OPEN_EXISTING, 0, nullptr);
	HANDLE out = options.output_path.empty()
			? CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
					  OPEN_EXISTING, 0, nullptr)
			: CreateFileW(io::os_path(options.output_path).c_str(), FILE_APPEND_DATA,
					  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable, OPEN_ALWAYS,
					  FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE err = INVALID_HANDLE_VALUE;
	auto close_std = [&] {
		if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
		if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
		if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
	};
	if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) {
		error = windows_error(in == INVALID_HANDLE_VALUE ? "opening NUL" : "opening the output file",
				GetLastError());
		close_std();
		return false;
	}
	// Standard error: its own file when the options name one, else the output's handle.
	if (!options.error_path.empty()) {
		err = CreateFileW(io::os_path(options.error_path).c_str(), FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
		if (err == INVALID_HANDLE_VALUE) {
			error = windows_error("opening the error file", GetLastError());
			close_std();
			return false;
		}
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
	HANDLE inherited[3] = { in, out, err };
	const size_t inherited_count = err != INVALID_HANDLE_VALUE ? 3 : 2;
	if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
				inherited_count * sizeof(HANDLE), nullptr, nullptr)) {
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
	startup.StartupInfo.hStdError = err != INVALID_HANDLE_VALUE ? err : out;
	if (options.behind) {
		// Its first window shown without activation (a GUI child's first
		// ShowWindow takes this in place of the one it asks for).
		startup.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW;
		startup.StartupInfo.wShowWindow = SW_SHOWNOACTIVATE;
	}
	startup.lpAttributeList = attributes;
	// One that ends with this process starts suspended until it is in its job,
	// so it cannot start a grandchild outside it. One that does not breaks away
	// from any job this process is in, whose kill-on-close would otherwise end it
	// with that job; where the job allows no breakaway, it stays in it.
	// CREATE_NO_WINDOW keeps a console child from opening a console window; a
	// GUI child's own windows are untouched.
	DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
	flags |= options.end_with_parent ? CREATE_SUSPENDED : CREATE_BREAKAWAY_FROM_JOB;
	// The environment the options name, else this process's (null).
	std::vector<wchar_t> environment;
	if (options.environment) environment = environment_block(*options.environment);
	PROCESS_INFORMATION info{};
	DWORD create_error = 0;
	auto create = [&](DWORD with) {
		// A fresh buffer each time: CreateProcessW may write into the command line.
		std::vector<wchar_t> command(command_line.begin(), command_line.end());
		command.push_back(L'\0');
		const BOOL ok = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE,
				with, options.environment ? environment.data() : nullptr,
				cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
		create_error = GetLastError();
		return ok != 0;
	};
	bool created = create(flags);
	if (!created && create_error == ERROR_ACCESS_DENIED && (flags & CREATE_BREAKAWAY_FROM_JOB) != 0)
		created = create(flags & ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB));
	DeleteProcThreadAttributeList(attributes);
	close_std();
	if (!created) {
		error = "cannot start " + options.argv[0] + ": " +
				windows_error("CreateProcessW", create_error);
		return false;
	}
	if (options.end_with_parent) {
		// Best effort: a process already in a job that refuses nesting runs
		// without one. The job lets a process in it start one that breaks away
		// (a child of the child that is itself started with end_with_parent
		// false, above).
		HANDLE job = CreateJobObjectW(nullptr, nullptr);
		if (job != nullptr) {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags =
					JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
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
	ends_with_parent_ = job_ != nullptr;
	identity_ = read_identity(info.hProcess);
	return true;
}

std::optional<uint32_t> ChildProcess::poll() {
	return wait(0);
}

std::optional<uint32_t> ChildProcess::wait(int timeout_ms) {
	if (!started()) return exit_code_;
	const DWORD waited = WaitForSingleObject(
			handle_of(process_), timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
	if (waited != WAIT_OBJECT_0) return std::nullopt;
	DWORD code = 0;
	if (!GetExitCodeProcess(handle_of(process_), &code)) code = 1;
	exit_code_ = static_cast<uint32_t>(code);
	pid_ = 0;
	// The process handle stays (identity() reads through it until release());
	// the job goes now, and with it any grandchild still in it.
	if (job_ != nullptr)
		CloseHandle(handle_of(job_));
	job_ = nullptr;
	return exit_code_;
}

bool ChildProcess::identity(ProcessIdentity &out) const {
	if (process_ == nullptr)
		return false;
	out = identity_;
	// What the spawn could not read, read now.
	if (out.image.empty() || out.created.empty()) {
		const ProcessIdentity now = read_identity(handle_of(process_));
		if (out.image.empty())
			out.image = now.image;
		if (out.created.empty())
			out.created = now.created;
	}
	return !out.created.empty();
}

bool ChildProcess::terminate(bool polite_only) {
	if (!started())
		return false;
	CloseRequest request;
	request.pid = static_cast<DWORD>(pid_);
	EnumWindows(close_top_level_window, reinterpret_cast<LPARAM>(&request));
	if (request.posted > 0)
		return true;
	// No window to ask (a console or headless child, a GUI one still starting):
	// nothing would carry the request, so the child is ended, unless the
	// caller asked politely only.
	if (polite_only)
		return false;
	return TerminateProcess(handle_of(process_), 1) != 0;
}

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
	// The environment the options name (execve), else this process's (execv).
	std::vector<std::string> env_entries = options.environment.value_or(std::vector<std::string>{});
	std::vector<char *> envp;
	envp.reserve(env_entries.size() + 1);
	for (std::string &entry : env_entries) envp.push_back(entry.data());
	envp.push_back(nullptr);
	const bool own_environment = options.environment.has_value();
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
	// Standard error: its own file when the options name one, else the output's descriptor.
	const int err_fd = options.error_path.empty()
			? -1
			: above_standard(::open(
					  options.error_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600));
	if (!options.error_path.empty() && err_fd < 0) {
		error = std::string("opening the error file: ") + std::strerror(errno);
		::close(in_fd);
		::close(out_fd);
		return false;
	}
	const int stderr_fd = err_fd >= 0 ? err_fd : out_fd;
	// The child reports a failed exec through this pipe; a successful exec
	// closes it (close-on-exec), and the parent reads end of file.
	int report[2] = { -1, -1 };
	if (::pipe(report) != 0) {
		error = std::string("pipe: ") + std::strerror(errno);
		::close(in_fd);
		::close(out_fd);
		if (err_fd >= 0) ::close(err_fd);
		return false;
	}
	::fcntl(report[0], F_SETFD, FD_CLOEXEC);
	::fcntl(report[1], F_SETFD, FD_CLOEXEC);
	report[0] = above_standard(report[0]);
	report[1] = above_standard(report[1]);
	if (report[0] < 0 || report[1] < 0) {
		error = "pipe: no descriptor past the standard three";
		for (const int fd : { in_fd, out_fd, err_fd, report[0], report[1] }) {
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
		(void)parent;
#endif
		// One meant to outlive this process leads a session of its own, so a
		// hangup of this process's terminal, or a signal to its process group,
		// misses it.
		if (!end_with_parent)
			::setsid();
		// Handlers reset at exec on their own; an ignored signal stays ignored.
		const int defaults[] = { SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD };
		for (const int sig : defaults) ::signal(sig, SIG_DFL);
		sigset_t none;
		sigemptyset(&none);
		::sigprocmask(SIG_SETMASK, &none, nullptr);
		if (::dup2(in_fd, 0) < 0 || ::dup2(out_fd, 1) < 0 || ::dup2(stderr_fd, 2) < 0) fail(errno);
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
		if (own_environment)
			::execve(argv[0], argv.data(), envp.data());
		else
			::execv(argv[0], argv.data());
		fail(errno);
	}
	const int fork_errno = errno;
	pthread_sigmask(SIG_SETMASK, &saved, nullptr);
	::close(in_fd);
	::close(out_fd);
	if (err_fd >= 0) ::close(err_fd);
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
#ifdef __linux__
	ends_with_parent_ = options.end_with_parent; // the parent-death signal armed
	identity_ = read_identity(pid_);
#endif
	return true;
}

std::optional<uint32_t> ChildProcess::poll() {
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
		exit_code_ = static_cast<uint32_t>(WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		exit_code_ = 128u + static_cast<uint32_t>(WTERMSIG(status));
	} else {
		return std::nullopt; // stopped or continued, still there
	}
	pid_ = 0;
	return exit_code_;
}

std::optional<uint32_t> ChildProcess::wait(int timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	for (;;) {
		if (auto code = poll()) return code;
		if (!started()) return exit_code_;
		if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) return std::nullopt;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

bool ChildProcess::identity(ProcessIdentity &out) const {
#ifdef __linux__
	if (pid_ <= 0)
		return false;
	out = identity_;
	// What the spawn could not read, read now (the child is unreaped, so the
	// pid is still its own).
	if (out.image.empty() || out.created.empty()) {
		const ProcessIdentity now = read_identity(pid_);
		if (out.image.empty())
			out.image = now.image;
		if (out.created.empty())
			out.created = now.created;
	}
	return !out.created.empty();
#else
	(void)out;
	return false;
#endif
}

bool ChildProcess::terminate(bool polite_only) {
	(void)polite_only; // SIGTERM is the request itself
	return started() && ::kill(static_cast<pid_t>(pid_), SIGTERM) == 0;
}

void ChildProcess::kill() {
	if (started()) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

#endif

} // namespace opennova::proc
