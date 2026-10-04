#include "log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace opennova::lister {
namespace {
std::mutex g_mu;
FILE *g_file = nullptr;
bool g_verbose = false;
std::vector<std::string> g_secrets;

void emit(const char *fmt, va_list ap) {
	char buf[16384];
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	const std::string line = redact(buf);
	const auto now = std::chrono::system_clock::now();
	const std::time_t t = std::chrono::system_clock::to_time_t(now);
	const int ms = static_cast<int>(
			std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	std::tm tmv{};
#if defined(_WIN32)
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char ts[32];
	std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
	std::lock_guard<std::mutex> lk(g_mu);
	std::fprintf(stderr, "%s %s\n", ts, line.c_str());
	if (g_file) {
		std::fprintf(g_file, "%s %s\n", ts, line.c_str());
		std::fflush(g_file);
	}
}
} // namespace

void log_open_file(const std::string &path) {
	std::lock_guard<std::mutex> lk(g_mu);
	if (g_file) std::fclose(g_file);
	g_file = std::fopen(path.c_str(), "a");
}

void log_add_secret(const std::string &secret) {
	if (secret.size() < 2) return; // a 1-char mask would shred the log
	std::lock_guard<std::mutex> lk(g_mu);
	g_secrets.push_back(secret);
}

void log_set_verbose(bool verbose) { g_verbose = verbose; }
bool log_verbose() { return g_verbose; }

std::string redact(std::string text) {
	std::vector<std::string> secrets;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		secrets = g_secrets;
	}
	for (const auto &s : secrets) {
		size_t pos = 0;
		while ((pos = text.find(s, pos)) != std::string::npos) {
			text.replace(pos, s.size(), "***");
			pos += 3;
		}
	}
	return text;
}

std::string mask_value(const std::string &value, size_t keep) {
	if (value.empty()) return "(empty)";
	if (value.size() <= keep) return std::string(value.size(), '*');
	return value.substr(0, keep) + "...(" + std::to_string(value.size()) + ")";
}

void logf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	emit(fmt, ap);
	va_end(ap);
}

void vlogf(const char *fmt, ...) {
	if (!g_verbose) return;
	va_list ap;
	va_start(ap, fmt);
	emit(fmt, ap);
	va_end(ap);
}

} // namespace opennova::lister
