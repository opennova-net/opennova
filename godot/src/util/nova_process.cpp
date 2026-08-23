#include "util/nova_process.h"

#include <godot_cpp/core/class_db.hpp>

#ifdef _WIN32
#include <windows.h>

#include <string>
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

} // namespace
#endif

bool Process::kill_pid(int64_t pid) {
#ifdef _WIN32
	if (pid <= 0) {
		return false;
	}
	HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
	if (handle == nullptr) {
		// Already exited (or never ours) -- nothing left to stop either way.
		return true;
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
	// Detached: the caller tracks it by pid, so neither handle is kept.
	CloseHandle(info.hThread);
	CloseHandle(info.hProcess);
	return static_cast<int64_t>(info.dwProcessId);
#else
	(void)path;
	(void)args;
	(void)working_dir;
	return -1;
#endif
}

} // namespace godot
