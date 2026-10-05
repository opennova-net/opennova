// opennova-serve: the headless game server (ADR 0051). The game's Serve Only
// host with no Godot and no window, configured by the retail host file. The
// server's legs live in server.cpp; this file owns the console, the signals
// and the wall clock.

#include "server.h"

#include <base/io/log.h>
#include <runtime/world/tick_accumulator.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

bool g_log_debug = false;
void console_log(opennova::io::LogLevel level, const char *message) {
	if (level == opennova::io::LogLevel::kDebug && !g_log_debug) return;
	std::fprintf(level >= opennova::io::LogLevel::kWarn ? stderr : stdout, "%s\n", message);
}

} // namespace

int main(int argc, char **argv) {
	using namespace opennova;
	std::vector<std::string> args(argv + 1, argv + argc);
	serve::ServeOptions options;
	std::string error;
	const int parsed = serve::parse_serve_options(args, options, error);
	if (parsed < 0) {
		std::fputs(serve::serve_usage(), stdout);
		return 0;
	}
	if (parsed > 0) {
		std::fprintf(stderr, "opennova-serve: %s\n%s", error.c_str(), serve::serve_usage());
		return 2;
	}
	g_log_debug = options.log_debug;
	io::set_log_sink(&console_log);

	serve::Server server(options);
	if (!server.start(error)) {
		std::fprintf(stderr, "opennova-serve: %s\n", error.c_str());
		return 1;
	}
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	std::fprintf(stdout, "opennova-serve: serving on UDP %u (Ctrl+C stops)\n", server.bound_port());
	std::fflush(stdout);

	// The session banks the measured wall clock, as Game_MainLoop banks its
	// GetTickCount deltas; the loop wakes once per logic quantum.
	using clock = std::chrono::steady_clock;
	const auto period = std::chrono::nanoseconds(
			static_cast<int64_t>(1e9 * world::TickAccumulator::kTickDt));
	auto last = clock::now();
	auto next = last + period;
	int code = 0;
	while (!g_stop.load()) {
		const auto now = clock::now();
		const double delta = std::chrono::duration<double>(now - last).count();
		last = now;
		if (!server.frame(delta)) {
			std::fprintf(stderr, "opennova-serve: the session ended: %s\n",
					server.end_message().c_str());
			code = 3;
			break;
		}
		std::this_thread::sleep_until(next);
		next += period;
	}
	server.stop();
	std::fprintf(stdout, "opennova-serve: stopped\n");
	return code;
}
