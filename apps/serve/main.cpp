// opennova-serve: the headless game server (ADR 0051). The game's Serve Only
// host with no Godot and no window, configured by the retail host file over
// the working directory's game.cfg, listed on NovaWorld when the cfg's network
// type says so. The server's legs live in server.cpp; this file owns the
// console, the stop signals, the socket layer and the wall clock. The status
// page and its chat input are status_console.cpp's; what a supervisor reads of
// the run (the tick health, the exit's line) is run_reports.h's.

#include "run_reports.h"
#include "server.h"
#include "status_console.h"

#include "console_log.h"
#include "credentials.h"

#include <base/io/file_io.h>
#include <base/io/log.h>
#include <runtime/world/tick_accumulator.h>

#include "net_sockets.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

#ifdef _WIN32
HANDLE g_done = nullptr; // set once the server stopped (and deregistered)

// Closing the console window, a logoff or a shutdown ends the process when
// this returns; hold it (Windows allows about 5 s) so the loop can stop the
// session and deregister from NovaWorld first.
BOOL WINAPI on_console_ctrl(DWORD type) {
	g_stop.store(true);
	if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) && g_done)
		WaitForSingleObject(g_done, 4500);
	return TRUE;
}
#endif

// `ns` of the steady clock.
int64_t ns_of(std::chrono::steady_clock::time_point at) {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}

// Starts the server and runs it until a stop or the session's end. Returns
// the process exit code, and at a non-zero one the error it logged in
// `failure` (the --exit-file's line).
int serve_until_stopped(const opennova::serve::ServeOptions &options, std::string &failure) {
	using namespace opennova;
	serve::Server server(options);
	std::string error;
	if (!server.start(error, &g_stop)) {
		io::logf(io::LogLevel::kError, "opennova-serve: %s", error.c_str());
		// A set mpreset ends the process with code 0, as retail's load and its
		// session create do [orig: Game_LoadConfig @0x5514A1..0x5514AC;
		// CNapiGameSession_CreateSession @0x4C97E7..0x4C97F0; crt_exit(0)].
		if (server.reset_exit())
			return 0;
		failure = error;
		return 1;
	}
	io::logf(io::LogLevel::kInfo, "opennova-serve: serving on UDP %u (Ctrl+C stops)", server.bound_port());

	// The session banks the measured wall clock, as Game_MainLoop banks its
	// GetTickCount deltas; the loop wakes once per logic quantum.
	using clock = std::chrono::steady_clock;
	const auto period = std::chrono::nanoseconds(
			static_cast<int64_t>(1e9 * world::TickAccumulator::kTickDt));
	auto last = clock::now();
	const auto started = last;
	auto next = last + period;
	int code = 0;
	// The status page stands in for the scene a retail dedicated host draws;
	// stdin is its chat input line (status_console.h). A supervised server
	// (--status-page off) has neither.
	serve::StatusConsole status(stdout);
	serve::ConsoleInput input;
	if (options.status_page)
		input.start();
	serve::TickStats ticks;
	while (!g_stop.load()) {
		const auto now = clock::now();
		const double delta = std::chrono::duration<double>(now - last).count();
		last = now;
		if (!server.frame(delta)) {
			// The rotation's end and the remote admin's quit are the session's
			// own end: the router's teardown (PostMenu_RouteMissionExit
			// @0x56864F, the quit's @0x5684AB). Any other end of the session is
			// an exit by itself.
			if (server.rotation_ended() || server.quit()) {
				io::logf(io::LogLevel::kInfo, "opennova-serve: %s after %d mission(s)",
						server.end_message().c_str(), server.missions_played());
			} else {
				io::logf(io::LogLevel::kError, "opennova-serve: the session ended: %s",
						server.end_message().c_str());
				failure = "the session ended: " + server.end_message();
				code = 3;
			}
			break;
		}
		if (options.status_page) {
			std::string line;
			while (input.poll(line)) status.submit(server, line);
			status.update(server, static_cast<uint32_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()));
		}
		if (!options.stats_file.empty()) {
			// This frame's work, due by `next`; the window's summary once a minute.
			const int64_t done_ns = ns_of(clock::now());
			ticks.frame(ns_of(now), done_ns, ns_of(next));
			if (const auto minute = ticks.take_if_due(done_ns)) {
				std::string why;
				if (!io::write_file_atomic(options.stats_file,
							serve::tick_minute_json(*minute, static_cast<int64_t>(std::time(nullptr))), why))
					io::logf(io::LogLevel::kWarn, "opennova-serve: --stats-file: %s", why.c_str());
			}
		}
		std::this_thread::sleep_until(next);
		next += period;
	}
	server.stop();
	io::logf(io::LogLevel::kInfo, "opennova-serve: stopped");
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
#ifdef _WIN32
	// The defaults' display width (server.h ServeOptions::desktop_width); a call that reports
	// none (0) leaves the platform equivalent.
	if (const int width = GetSystemMetrics(SM_CXSCREEN); width > 0) options.desktop_width = width;
#endif
	// The log sink masks every registered secret, so an account name, a
	// password or a session tag never reaches the console (console_log.h).
	serve::install_console_log(std::string(), options.log_debug);
	// The error a non-zero exit ends on, for the supervisor (--exit-file).
	const auto exit_with = [&options](int code, const std::string &failure) {
		std::string why;
		if (code != 0 && !options.exit_file.empty() &&
				!io::write_file_atomic(options.exit_file, serve::exit_file_text(failure), why))
			io::logf(io::LogLevel::kWarn, "opennova-serve: --exit-file: %s", why.c_str());
		return code;
	};
	if (!options.credentials_path.empty()) {
		if (!serve::load_credentials(options.credentials_path, options.credentials, error)) {
			io::logf(io::LogLevel::kError, "opennova-serve: --credentials: %s", error.c_str());
			return exit_with(2, "--credentials: " + error);
		}
		const serve::Credentials &c = options.credentials;
		for (const std::string *secret : { &c.user, &c.pass })
			serve::add_log_secret(*secret);
	}
	// Installed before the boot, so a stop during a long load or the
	// NovaWorld hosting stops cleanly.
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
#ifdef _WIN32
	g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(on_console_ctrl, TRUE);
#endif

	// The process owns the socket layer, which is process-wide: the server's
	// stop() closes its own sockets and leaves the layer up, and the lister
	// never starts or stops it.
	if (net::startup() != 0) {
		io::logf(io::LogLevel::kError, "opennova-serve: the socket layer did not start");
		return exit_with(1, "the socket layer did not start");
	}
	std::string failure;
	const int code = serve_until_stopped(options, failure);
	net::shutdown();
#ifdef _WIN32
	SetEvent(g_done);
#endif
	return exit_with(code, failure);
}
