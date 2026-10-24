#include "platform.h"

#include <base/io/os_path.h>
#include <base/proc/child_process.h>

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace opennova::updater {
#ifdef _WIN32
std::wstring wide(const std::string &text) {
	if (text.empty()) return {};
	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
	std::wstring out(size > 0 ? size : 0, L'\0');
	if (size > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), size);
	return out;
}
static std::string utf8(const wchar_t *text) {
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	std::string out(size > 0 ? size : 0, '\0');
	if (size > 0) { WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr); out.pop_back(); }
	return out;
}
#endif

std::filesystem::path executable_path(const char *argv0) {
#ifdef _WIN32
	(void)argv0;
	constexpr size_t kWindowsPathBuffer = 32768;
	std::vector<wchar_t> buffer(kWindowsPathBuffer);
	const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
	return size && size < buffer.size() ? std::filesystem::path(buffer.data()) : std::filesystem::path();
#else
	std::error_code ec;
	auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
	if (!ec) return path;
	return argv0 ? std::filesystem::absolute(io::os_path(argv0), ec) : std::filesystem::path();
#endif
}

std::vector<std::string> process_arguments(int argc, char **argv) {
	std::vector<std::string> out;
#ifdef _WIN32
	(void)argc; (void)argv;
	int count = 0;
	wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
	if (args) { for (int i = 1; i < count; ++i) out.push_back(utf8(args[i])); LocalFree(args); }
#else
	for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
#endif
	return out;
}

uint64_t process_id() {
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return uint64_t(getpid());
#endif
}

// The game closed: WM_CLOSE posted to its window, then its process awaited, where retail polls
// IsWindow (D-UPD-10). [orig: Cupdate2App_InitInstance @ 0x404820 (UPDATE.EXE), @0x4048ef..0x40495a]
bool wait_for_parent(uint64_t pid, uintptr_t window, std::string &error, const std::atomic_bool *cancel) {
#ifdef _WIN32
	if (window) {
		DWORD owner = 0;
		GetWindowThreadProcessId(reinterpret_cast<HWND>(window), &owner);
		if (!pid) pid = owner;
		PostMessageW(reinterpret_cast<HWND>(window), WM_CLOSE, 0, 0);
	}
#else
	(void)window;
#endif
	if (!pid) return true;
	if (pid == process_id()) { error = "Cannot wait for the updater itself."; return false; }
#ifdef _WIN32
	HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
	if (!process) {
		if (GetLastError() == ERROR_INVALID_PARAMETER) return true;
		error = "Cannot wait for the game process to exit."; return false;
	}
#endif
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (true) {
#ifdef _WIN32
		const DWORD waited = WaitForSingleObject(process, 100);
		if (waited == WAIT_OBJECT_0) { CloseHandle(process); return true; }
		if (waited == WAIT_FAILED) { CloseHandle(process); error = "Cannot wait for the game process."; return false; }
#else
		if (::kill(static_cast<pid_t>(pid), 0) < 0 && errno == ESRCH) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
		if ((cancel && cancel->load()) || std::chrono::steady_clock::now() >= deadline) {
#ifdef _WIN32
			CloseHandle(process);
#endif
			error = cancel && cancel->load() ? "Update aborted!" : "Please close the game before updating. Game files are in use.";
			return false;
		}
	}
}

bool launch_detached(const std::filesystem::path &program, const std::filesystem::path &cwd,
		const std::vector<std::string> &args, std::string &error) {
	proc::SpawnOptions spawn;
	spawn.argv.push_back(io::utf8_path(program));
	spawn.argv.insert(spawn.argv.end(), args.begin(), args.end());
	spawn.working_dir = io::utf8_path(cwd);
	spawn.end_with_parent = false;
	proc::ChildProcess process;
	return process.spawn(spawn, error) && process.release();
}

void write_stdout(const std::string &text) {
#ifdef _WIN32
	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
	if (handle && handle != INVALID_HANDLE_VALUE) {
		DWORD wrote = 0;
		WriteFile(handle, text.data(), DWORD(text.size()), &wrote, nullptr);
	}
#else
	std::fwrite(text.data(), 1, text.size(), stdout);
	std::fflush(stdout);
#endif
}
} // namespace opennova::updater
