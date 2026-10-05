// opennova-serve: the headless game server (ADR 0051). The game's Serve Only
// host with no Godot and no window, configured by the retail host file over
// the working directory's game.cfg, listed on NovaWorld when the cfg's network
// type says so. The server's legs live in server.cpp; this file owns the
// console, the stop signals, the socket layer and the wall clock. The status
// page and its chat input are status_console.cpp's.

#include "server.h"
#include "status_console.h"

#include "console_log.h"
#include "listing.h"

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
// session and deregister from NovaWorld first, as opennova-nw-lister does.
BOOL WINAPI on_console_ctrl(DWORD type) {
	g_stop.store(true);
	if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) && g_done)
		WaitForSingleObject(g_done, 4500);
	return TRUE;
}
#endif

// Starts the server and runs it until a stop or the session's end. Returns
// the process exit code.
int serve_until_stopped(const opennova::serve::ServeOptions &options) {
	using namespace opennova;
	serve::Server server(options);
	std::string error;
	if (!server.start(error, &g_stop)) {
		io::logf(io::LogLevel::kError, "opennova-serve: %s", error.c_str());
		// A game.cfg with mpreset set ends the process with code 0, as retail's
		// load does [orig: Game_LoadConfig @0x5514A1..0x5514AC, crt_exit(0)].
		return server.reset_exit() ? 0 : 1;
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
	// stdin is its chat input line (status_console.h).
	serve::StatusConsole status(stdout);
	serve::ConsoleInput input;
	input.start();
	while (!g_stop.load()) {
		const auto now = clock::now();
		const double delta = std::chrono::duration<double>(now - last).count();
		last = now;
		if (!server.frame(delta)) {
			// The rotation's end is the session's own end: the router's miss
			// arm (PostMenu_RouteMissionExit @0x56864F). Any other end of the
			// session is an exit by itself.
			if (server.rotation_ended()) {
				io::logf(io::LogLevel::kInfo, "opennova-serve: %s after %d mission(s)",
						server.end_message().c_str(), server.missions_played());
			} else {
				io::logf(io::LogLevel::kError, "opennova-serve: the session ended: %s",
						server.end_message().c_str());
				code = 3;
			}
			break;
		}
		std::string line;
		while (input.poll(line)) status.submit(server, line);
		status.update(server, static_cast<uint32_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count()));
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
	// The log sink masks every registered secret, so an account name, a
	// password or a session tag never reaches the console (opennova-nw-lister's).
	nw_lister::install_console_log(std::string(), options.log_debug);
	if (!options.credentials_path.empty()) {
		if (!nw_lister::load_credentials(options.credentials_path, options.credentials, error)) {
			io::logf(io::LogLevel::kError, "opennova-serve: --credentials: %s", error.c_str());
			return 2;
		}
		const nw_lister::Credentials &c = options.credentials;
		for (const std::string *secret : {&c.user, &c.pass, &c.admin_user, &c.admin_pass})
			nw_lister::add_log_secret(*secret);
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
		return 1;
	}
	const int code = serve_until_stopped(options);
	net::shutdown();
#ifdef _WIN32
	SetEvent(g_done);
#endif
	return code;
}
