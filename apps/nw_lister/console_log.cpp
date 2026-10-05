#include "console_log.h"

#include <base/io/log.h>
#include <base/io/os_path.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

namespace opennova::nw_lister {

namespace {

std::mutex g_mutex;
FILE *g_file = nullptr;
bool g_verbose = false;
std::vector<std::string> g_secrets;

void write_line(io::LogLevel level, const char *message) {
	if (level == io::LogLevel::kDebug && !g_verbose) return;
	std::string line(message);
	const auto now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	const int ms = static_cast<int>(
			std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	std::tm local{};
#if defined(_WIN32)
	localtime_s(&local, &seconds);
#else
	localtime_r(&seconds, &local);
#endif
	char stamp[32];
	std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec, ms);
	std::lock_guard<std::mutex> lock(g_mutex);
	for (const std::string &secret : g_secrets) {
		for (size_t at = line.find(secret); at != std::string::npos; at = line.find(secret, at + 3)) {
			line.replace(at, secret.size(), "***");
		}
	}
	std::fprintf(stderr, "%s %s\n", stamp, line.c_str());
	if (g_file != nullptr) {
		std::fprintf(g_file, "%s %s\n", stamp, line.c_str());
		std::fflush(g_file);
	}
}

} // namespace

void install_console_log(const std::string &file_path, bool verbose) {
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_verbose = verbose;
		if (!file_path.empty()) g_file = io::fopen_utf8(file_path.c_str(), "a");
	}
	io::set_log_sink(write_line);
	if (!file_path.empty() && g_file == nullptr) {
		io::logf(io::LogLevel::kWarn, "[log] cannot open %s; logging to stderr only", file_path.c_str());
	}
}

void add_log_secret(const std::string &secret) {
	if (secret.size() < 2) return; // a one-character mask would shred the log
	std::lock_guard<std::mutex> lock(g_mutex);
	g_secrets.push_back(secret);
}

std::string mask_value(const std::string &value, size_t keep) {
	if (value.empty()) return "(empty)";
	if (value.size() <= keep) return std::string(value.size(), '*');
	return value.substr(0, keep) + "...(" + std::to_string(value.size()) + ")";
}

} // namespace opennova::nw_lister
