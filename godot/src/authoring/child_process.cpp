#include "authoring/child_process.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>

#ifdef _WIN32
#include <windows.h>

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

bool has_exited(HANDLE handle) {
	return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

// ASCII lower case, for comparing Windows paths.
std::wstring lowered(std::wstring text) {
	for (wchar_t &c : text) {
		if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
	}
	return text;
}

// A path's file name, lower-cased, whichever separator it uses.
std::wstring file_name_of(const std::wstring &path) {
	const size_t slash = path.find_last_of(L"\\/");
	return lowered(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

// A path in one form, lower-cased: made absolute, then its short (8.3) components made long; a
// path that no longer resolves keeps its absolute form.
std::wstring long_path_of(const std::wstring &path) {
	std::wstring full(MAX_PATH, L'\0');
	DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
	if (length >= full.size()) {
		full.resize(length);
		length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
	}
	full.resize(length > 0 && length < full.size() ? length : 0);
	if (full.empty()) {
		full = path;
	}
	std::wstring expanded(MAX_PATH, L'\0');
	length = GetLongPathNameW(full.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
	if (length >= expanded.size()) {
		expanded.resize(length);
		length = GetLongPathNameW(full.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
	}
	expanded.resize(length > 0 && length < expanded.size() ? length : 0);
	return lowered(expanded.empty() ? full : expanded);
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
	const std::wstring exe = native_path(plan.executable);
	const std::wstring cwd = native_path(plan.working_dir);
	if (exe.empty()) {
		return -1;
	}
	std::wstring command = quote_arg(exe);
	for (const std::string &arg : plan.args) {
		command.push_back(L' ');
		command.append(quote_arg(to_wide(arg)));
	}
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
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

opennova::editor::ProcessLiveness ChildProcessPlatform::process_liveness(int64_t pid, const std::string &executable) {
	using opennova::editor::ProcessLiveness;
#ifdef _WIN32
	if (pid <= 0 || executable.empty()) {
		return ProcessLiveness::Unknown;
	}
	HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (handle == nullptr) {
		// No process of that id is Dead; one that will not be opened (an elevated game) may be the
		// game: Unknown.
		return GetLastError() == ERROR_INVALID_PARAMETER ? ProcessLiveness::Dead : ProcessLiveness::Unknown;
	}
	ProcessLiveness state = ProcessLiveness::Unknown;
	if (has_exited(handle)) {
		state = ProcessLiveness::Dead;
	} else {
		std::wstring image(MAX_PATH * 4, L'\0');
		DWORD length = static_cast<DWORD>(image.size());
		if (QueryFullProcessImageNameW(handle, 0, image.data(), &length) != 0) {
			image.resize(length);
			const std::wstring running = long_path_of(image);
			const std::wstring leased = long_path_of(native_path(executable));
			if (running == leased) {
				state = ProcessLiveness::Alive;
			} else if (file_name_of(running) != file_name_of(leased)) {
				state = ProcessLiveness::Dead; // the id was recycled: another program runs under it
			}
			// The same program's name by another path (a link, a junction): Unknown.
		}
	}
	CloseHandle(handle);
	return state;
#else
	(void)pid;
	(void)executable;
	return ProcessLiveness::Unknown;
#endif
}

int64_t ChildProcessPlatform::now_ms() {
	return static_cast<int64_t>(Time::get_singleton()->get_ticks_msec());
}

void ChildProcessPlatform::sleep_ms(int64_t ms) {
	OS::get_singleton()->delay_msec(static_cast<int>(ms));
}

} // namespace godot
