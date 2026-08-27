#include "util/nova_process.h"

#include <godot_cpp/core/class_db.hpp>

#ifdef _WIN32
#include <windows.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#endif

namespace godot {

void Process::_bind_methods() {
	ClassDB::bind_static_method("Process",
			D_METHOD("spawn_in_dir", "path", "args", "working_dir"), &Process::spawn_in_dir);
	ClassDB::bind_static_method("Process",
			D_METHOD("supports_working_directory"), &Process::supports_working_directory);
	ClassDB::bind_static_method("Process", D_METHOD("kill_pid", "pid"), &Process::kill_pid);
	ClassDB::bind_static_method("Process", D_METHOD("is_running", "pid"), &Process::is_running);
	ClassDB::bind_static_method("Process",
			D_METHOD("wait_for_exit", "pid", "timeout_msec"), &Process::wait_for_exit);
	ClassDB::bind_static_method("Process", D_METHOD("release", "pid"), &Process::release);
}

bool Process::supports_working_directory() {
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

#ifdef _WIN32
namespace {

// The children this binding spawned, by pid. Holding the process handle is what makes the pid
// safe to reason about: an open handle keeps the process object alive (and its id reserved) until
// release(), so no query here can land on a recycled pid.
std::mutex g_children_mutex;
std::unordered_map<DWORD, HANDLE> g_children;

HANDLE find_child(int64_t pid) {
	std::lock_guard<std::mutex> lock(g_children_mutex);
	const auto it = g_children.find(static_cast<DWORD>(pid));
	return it == g_children.end() ? nullptr : it->second;
}

void remember_child(DWORD pid, HANDLE handle) {
	std::lock_guard<std::mutex> lock(g_children_mutex);
	const auto stale = g_children.find(pid);
	if (stale != g_children.end()) {
		// A pid can only come back once its previous holder is gone AND we released our
		// handle -- but never leak the old one if a caller skipped release().
		CloseHandle(stale->second);
		g_children.erase(stale);
	}
	g_children.emplace(pid, handle);
}

HANDLE forget_child(int64_t pid) {
	std::lock_guard<std::mutex> lock(g_children_mutex);
	const auto it = g_children.find(static_cast<DWORD>(pid));
	if (it == g_children.end()) {
		return nullptr;
	}
	HANDLE handle = it->second;
	g_children.erase(it);
	return handle;
}

bool has_exited(HANDLE handle) {
	return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

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

std::wstring to_wide(const String &value) {
	const CharString utf8 = value.utf8();
	if (utf8.length() == 0) {
		return std::wstring();
	}
	const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.get_data(), utf8.length(), nullptr, 0);
	if (needed <= 0) {
		return std::wstring();
	}
	std::wstring out(static_cast<size_t>(needed), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.get_data(), utf8.length(), out.data(), needed);
	return out;
}

DWORD wait_timeout(int64_t timeout_msec) {
	if (timeout_msec < 0) {
		return INFINITE;
	}
	return static_cast<DWORD>(timeout_msec);
}

} // namespace
#endif

bool Process::kill_pid(int64_t pid) {
#ifdef _WIN32
	if (pid <= 0) {
		return false;
	}
	if (HANDLE child = find_child(pid)) {
		if (has_exited(child)) {
			return true;
		}
		return TerminateProcess(child, 0) != 0;
	}
	// Not one of ours (or already released): probe by id. A pid that names no process is
	// "already gone"; any other refusal means it exists and we could not touch it.
	HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
	if (handle == nullptr) {
		return GetLastError() == ERROR_INVALID_PARAMETER;
	}
	const BOOL ok = TerminateProcess(handle, 0);
	CloseHandle(handle);
	return ok != 0;
#else
	(void)pid;
	return false;
#endif
}

bool Process::is_running(int64_t pid) {
#ifdef _WIN32
	if (pid <= 0) {
		return false;
	}
	if (HANDLE child = find_child(pid)) {
		return !has_exited(child);
	}
	HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
	if (handle == nullptr) {
		return false;
	}
	DWORD code = 0;
	const BOOL ok = GetExitCodeProcess(handle, &code);
	CloseHandle(handle);
	return ok != 0 && code == STILL_ACTIVE;
#else
	(void)pid;
	return false;
#endif
}

bool Process::wait_for_exit(int64_t pid, int64_t timeout_msec) {
#ifdef _WIN32
	if (pid <= 0) {
		return true;
	}
	if (HANDLE child = find_child(pid)) {
		return WaitForSingleObject(child, wait_timeout(timeout_msec)) == WAIT_OBJECT_0;
	}
	HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
	if (handle == nullptr) {
		return GetLastError() == ERROR_INVALID_PARAMETER;
	}
	const DWORD result = WaitForSingleObject(handle, wait_timeout(timeout_msec));
	CloseHandle(handle);
	return result == WAIT_OBJECT_0;
#else
	(void)pid;
	(void)timeout_msec;
	return true;
#endif
}

void Process::release(int64_t pid) {
#ifdef _WIN32
	if (pid <= 0) {
		return;
	}
	if (HANDLE child = forget_child(pid)) {
		CloseHandle(child);
	}
#else
	(void)pid;
#endif
}

int64_t Process::spawn_in_dir(const String &path, const PackedStringArray &args,
		const String &working_dir) {
#ifdef _WIN32
	const std::wstring exe = to_wide(path.replace("/", "\\"));
	const std::wstring cwd = to_wide(working_dir.replace("/", "\\"));
	if (exe.empty()) {
		return -1;
	}

	std::wstring command = quote_arg(exe);
	for (int i = 0; i < args.size(); ++i) {
		command.push_back(L' ');
		command.append(quote_arg(to_wide(args[i])));
	}

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION info{};

	// A mutable buffer: CreateProcessW may write into lpCommandLine.
	std::vector<wchar_t> buffer(command.begin(), command.end());
	buffer.push_back(L'\0');

	const BOOL ok = CreateProcessW(
			exe.c_str(),
			buffer.data(),
			nullptr,
			nullptr,
			FALSE,
			0,
			nullptr,
			cwd.empty() ? nullptr : cwd.c_str(),
			&startup,
			&info);
	if (!ok) {
		return -1;
	}
	// The thread handle is never needed; the process handle is the child's identity until
	// release().
	CloseHandle(info.hThread);
	remember_child(info.dwProcessId, info.hProcess);
	return static_cast<int64_t>(info.dwProcessId);
#else
	(void)path;
	(void)args;
	(void)working_dir;
	return -1;
#endif
}

} // namespace godot
