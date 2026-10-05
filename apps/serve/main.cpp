// opennova-serve: the headless game server (ADR 0051). The game's Serve Only
// host with no Godot and no window, configured by the retail host file over
// the working directory's game.cfg. The server's legs live in server.cpp;
// this file owns the console, the signals, the socket layer and the wall
// clock.

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

// Starts the server and runs it until a signal or the session's end. Returns
// the process exit code.
int serve_until_stopped(const opennova::serve::ServeOptions &options) {
	using namespace opennova;
	serve::Server server(options);
	std::string error;
	if (!server.start(error)) {
		std::fprintf(stderr, "opennova-serve: %s\n", error.c_str());
		// A game.cfg with mpreset set ends the process with code 0, as retail's
		// load does [orig: Game_LoadConfig @0x5514A1..0x5514AC, crt_exit(0)].
		return server.reset_exit() ? 0 : 1;
	}
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
			// The rotation's end is the session's own end: the router's miss
			// arm (PostMenu_RouteMissionExit @0x56864F). Any other end of the
			// session is an exit by itself.
			if (server.rotation_ended()) {
				std::fprintf(stdout, "opennova-serve: %s after %d mission(s)\n",
						server.end_message().c_str(), server.missions_played());
			} else {
				std::fprintf(stderr, "opennova-serve: the session ended: %s\n",
						server.end_message().c_str());
				code = 3;
			}
			break;
		}
		std::this_thread::sleep_until(next);
		next += period;
	}
	server.stop();
	std::fprintf(stdout, "opennova-serve: stopped\n");
	return code;
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
	// Installed before the boot, so a Ctrl+C during a long load stops cleanly.
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	// The process owns the socket layer, which is process-wide: the server's
	// stop() closes its own socket and leaves the layer up.
	if (net::startup() != 0) {
		std::fprintf(stderr, "opennova-serve: the socket layer did not start\n");
		return 1;
	}
	const int code = serve_until_stopped(options);
	net::shutdown();
	return code;
}
