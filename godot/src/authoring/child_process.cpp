#include "authoring/child_process.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>
#endif

namespace godot {

#ifdef _WIN32
namespace {

// CreateProcessW parses one command line, so each argument is quoted the way the CRT's
// parser expects: wrap in quotes, backslash-escape embedded quotes, and double the run of
// backslashes that immediately precedes a quote (or the closing quote).
std::wstring quote_arg(const std::wstring &arg) {
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
		return arg;
	}
	std::wstring out;
	out.push_back(L'"');
	for (size_t i = 0;; ++i) {
		size_t slashes = 0;
		while (i < arg.size() && arg[i] == L'\\') {
			++i;
			++slashes;
		}
		if (i == arg.size()) {
			out.append(slashes * 2, L'\\');
			break;
		}
		if (arg[i] == L'"') {
			out.append(slashes * 2 + 1, L'\\');
		} else {
			out.append(slashes, L'\\');
		}
		out.push_back(arg[i]);
	}
	out.push_back(L'"');
	return out;
}

std::wstring to_wide(const std::string &utf8) {
	if (utf8.empty()) {
		return std::wstring();
	}
	const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (needed <= 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
	return out;
}

std::wstring native_path(std::string path) {
	for (char &c : path) {
		if (c == '/') c = '\\';
	}
	return to_wide(path);
}

// A path CreateProcessW takes: one past `limit` characters (the system's current directory holds
// MAX_PATH less its trailing backslash and terminator; an image MAX_PATH less its terminator) given
// in its 8.3 form where the volume keeps one, asked through the extended form a long path needs;
// empty where even that is past the limit (the spawn is refused, never handed a path it would cut).
std::wstring within(const std::wstring &path, size_t limit) {
	if (path.size() <= limit) {
		return path;
	}
	std::wstring extended = path;
	if (path.rfind(L"\\\\?\\", 0) != 0) {
		extended = path.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
	}
	const DWORD needed = GetShortPathNameW(extended.c_str(), nullptr, 0);
	if (needed == 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(needed), L'\0');
	const DWORD written = GetShortPathNameW(extended.c_str(), out.data(), needed);
	if (written == 0 || written >= needed) {
		return std::wstring();
	}
	out.resize(written);
	if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) {
		out = L"\\\\" + out.substr(8);
	} else if (out.rfind(L"\\\\?\\", 0) == 0) {
		out.erase(0, 4);
	}
	return out.size() <= limit ? out : std::wstring();
}

bool has_exited(HANDLE handle) {
	return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

std::string from_wide(const std::wstring &wide) {
	if (wide.empty()) {
		return std::string();
	}
	const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
			nullptr, nullptr);
	if (needed <= 0) {
		return std::string();
	}
	std::string out(static_cast<size_t>(needed), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr,
			nullptr);
	return out;
}

// The process's creation time as the OS keeps it (a FILETIME's 100 ns count), in decimal: equal
// for one process, different for another that later takes its pid. "" when it cannot be read.
std::string creation_stamp(HANDLE handle) {
	FILETIME created{}, exited{}, kernel{}, user{};
	if (GetProcessTimes(handle, &created, &exited, &kernel, &user) == 0) {
		return std::string();
	}
	return std::to_string((static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}

// The child's top-level windows get WM_CLOSE: the game's orderly quit path.
BOOL CALLBACK close_window_of_process(HWND window, LPARAM param) {
	DWORD owner = 0;
	GetWindowThreadProcessId(window, &owner);
	if (owner == static_cast<DWORD>(param) && GetWindow(window, GW_OWNER) == nullptr) {
		PostMessageW(window, WM_CLOSE, 0, 0);
	}
	return TRUE;
}

// A child started behind: each shown window of it sent to the bottom without activation (posted to its
// thread: the editor never waits on the game) and its flashing stopped, unless it is the foreground window
// (the person's own choice).
BOOL CALLBACK keep_window_behind(HWND window, LPARAM param) {
	DWORD owner = 0;
	GetWindowThreadProcessId(window, &owner);
	if (owner != static_cast<DWORD>(param) || !IsWindowVisible(window) || window == GetForegroundWindow()) return TRUE;
	SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	FLASHWINFO flash{};
	flash.cbSize = sizeof(flash);
	flash.hwnd = window;
	flash.dwFlags = FLASHW_STOP;
	FlashWindowEx(&flash);
	return TRUE;
}

} // namespace
#endif

ChildProcessPlatform::~ChildProcessPlatform() {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &[pid, handle] : children_) {
		CloseHandle(static_cast<HANDLE>(handle));
	}
	children_.clear();
#endif
}

bool ChildProcessPlatform::can_spawn() const {
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

int64_t ChildProcessPlatform::spawn(const opennova::editor::LaunchPlan &plan) {
#ifdef _WIN32
	// A run directory or an image under a deep project, past what the call takes, in its short form.
	const std::wstring exe = within(native_path(plan.executable), MAX_PATH - 1);
	const std::wstring cwd = within(native_path(plan.working_dir), MAX_PATH - 2);
	if (exe.empty() || (!plan.working_dir.empty() && cwd.empty())) {
		return -1;
	}
	std::wstring command = quote_arg(exe);
	for (const std::string &arg : plan.args) {
		command.push_back(L' ');
		command.append(quote_arg(to_wide(arg)));
	}
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	// Behind (the MCP gaps lane): its first window shown without activation, and the foreground locked while
	// it starts (tend() lets it go).
	if (plan.behind) {
		startup.dwFlags |= STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_SHOWNOACTIVATE;
		std::lock_guard<std::mutex> lock(mutex_);
		if (!locked_) locked_ = LockSetForegroundWindow(LSFW_LOCK) != 0;
	}
	PROCESS_INFORMATION info{};
	// A mutable buffer: CreateProcessW may write into lpCommandLine.
	std::vector<wchar_t> buffer(command.begin(), command.end());
	buffer.push_back(L'\0');
	const BOOL ok = CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
			cwd.empty() ? nullptr : cwd.c_str(), &startup, &info);
	if (!ok) {
		return -1;
	}
	// The thread handle is never needed; the process handle is the child's identity
	// until release().
	CloseHandle(info.hThread);
	std::lock_guard<std::mutex> lock(mutex_);
	const int64_t pid = static_cast<int64_t>(info.dwProcessId);
	if (auto stale = children_.find(pid); stale != children_.end()) {
		CloseHandle(static_cast<HANDLE>(stale->second));
		children_.erase(stale);
	}
	children_.emplace(pid, info.hProcess);
	if (plan.behind) behind_[pid] = static_cast<int64_t>(Time::get_singleton()->get_ticks_msec()) + kBehindMs;
	return pid;
#else
	(void)plan;
	return -1;
#endif
}

bool ChildProcessPlatform::is_running(int64_t pid) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = children_.find(pid);
	return it != children_.end() && !has_exited(static_cast<HANDLE>(it->second));
#else
	(void)pid;
	return false;
#endif
}

bool ChildProcessPlatform::terminate(int64_t pid) {
#ifdef _WIN32
	if (!is_running(pid)) {
		return true;
	}
	EnumWindows(close_window_of_process, static_cast<LPARAM>(pid));
	return true;
#else
	(void)pid;
	return false;
#endif
}

bool ChildProcessPlatform::kill(int64_t pid) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = children_.find(pid);
	if (it == children_.end()) {
		return true;
	}
	HANDLE handle = static_cast<HANDLE>(it->second);
	if (has_exited(handle)) {
		return true;
	}
	if (TerminateProcess(handle, 0) == 0) {
		return false;
	}
	// TerminateProcess only requests the exit; the child can hold its files open for a
	// moment afterwards, and the next build repacks into that directory's neighbour.
	return WaitForSingleObject(handle, 2000) == WAIT_OBJECT_0;
#else
	(void)pid;
	return false;
#endif
}

bool ChildProcessPlatform::exit_code(int64_t pid, uint32_t &out) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = children_.find(pid);
	if (it == children_.end()) {
		return false;
	}
	HANDLE handle = static_cast<HANDLE>(it->second);
	DWORD code = 0;
	if (!has_exited(handle) || GetExitCodeProcess(handle, &code) == 0) {
		return false;
	}
	out = static_cast<uint32_t>(code);
	return true;
#else
	(void)pid;
	(void)out;
	return false;
#endif
}

void ChildProcessPlatform::release(int64_t pid) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = children_.find(pid);
	if (it == children_.end()) {
		return;
	}
	CloseHandle(static_cast<HANDLE>(it->second));
	children_.erase(it);
#else
	(void)pid;
#endif
}

bool ChildProcessPlatform::process_identity(int64_t pid, opennova::editor::ProcessIdentity &out) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = children_.find(pid);
	if (it == children_.end()) {
		return false;
	}
	HANDLE handle = static_cast<HANDLE>(it->second);
	std::wstring image(MAX_PATH * 4, L'\0');
	DWORD length = static_cast<DWORD>(image.size());
	if (QueryFullProcessImageNameW(handle, 0, image.data(), &length) != 0) {
		image.resize(length);
		std::string path = from_wide(image);
		std::replace(path.begin(), path.end(), '\\', '/');
		out.image = path;
	}
	out.created = creation_stamp(handle);
	return !out.created.empty();
#else
	(void)pid;
	(void)out;
	return false;
#endif
}

opennova::editor::ProcessLiveness ChildProcessPlatform::process_liveness(int64_t pid, const std::string &created) {
	using opennova::editor::ProcessLiveness;
#ifdef _WIN32
	if (pid <= 0) {
		return ProcessLiveness::Unknown;
	}
	HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (handle == nullptr) {
		// No process of that id is Dead; one that will not be opened may be the game: Unknown.
		return GetLastError() == ERROR_INVALID_PARAMETER ? ProcessLiveness::Dead : ProcessLiveness::Unknown;
	}
	ProcessLiveness state = ProcessLiveness::Unknown;
	DWORD code = 0;
	if (GetExitCodeProcess(handle, &code) != 0 && code != STILL_ACTIVE) {
		state = ProcessLiveness::Dead; // it has exited (a handle someone holds keeps it listed)
	} else if (!created.empty()) {
		const std::string now = creation_stamp(handle);
		if (!now.empty()) {
			state = now == created ? ProcessLiveness::Alive : ProcessLiveness::Dead;
		}
	}
	CloseHandle(handle);
	return state;
#else
	(void)pid;
	(void)created;
	return ProcessLiveness::Unknown;
#endif
}

int64_t ChildProcessPlatform::now_ms() {
	return static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
}

void ChildProcessPlatform::tend() {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(mutex_);
	if (behind_.empty() && !locked_) return;
	const int64_t now = static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
	for (auto it = behind_.begin(); it != behind_.end();) {
		const auto child = children_.find(it->first);
		if (now >= it->second || child == children_.end() || has_exited(static_cast<HANDLE>(child->second))) {
			it = behind_.erase(it);
			continue;
		}
		EnumWindows(keep_window_behind, static_cast<LPARAM>(it->first));
		++it;
	}
	// Held only while a child starts: the system lets it go as the person switches windows anyway.
	if (behind_.empty() && locked_) {
		LockSetForegroundWindow(LSFW_UNLOCK);
		locked_ = false;
	}
#else
	behind_.clear();
	locked_ = false;
#endif
}

void ChildProcessPlatform::sleep_ms(int64_t ms) {
	OS::get_singleton()->delay_msec(static_cast<int>(ms));
}

} // namespace godot
