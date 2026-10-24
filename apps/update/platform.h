#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace opennova::updater {
std::filesystem::path executable_path(const char *argv0 = nullptr);
std::vector<std::string> process_arguments(int argc = 0, char **argv = nullptr);
uint64_t process_id();
bool wait_for_parent(uint64_t pid, uintptr_t window, std::string &error,
		const std::atomic_bool *cancel = nullptr);
bool launch_detached(const std::filesystem::path &program, const std::filesystem::path &cwd,
		const std::vector<std::string> &args, std::string &error);
void write_stdout(const std::string &text);
#ifdef _WIN32
std::wstring wide(const std::string &text);
#endif
} // namespace opennova::updater
