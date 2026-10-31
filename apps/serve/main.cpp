// opennova-serve: the headless game server (ADR 0051). The game's Serve Only
// host with no Godot and no window, configured by the retail host file over
// the working directory's game.cfg, listed on NovaWorld when the cfg's network
// type says so. The server's legs live in server.cpp, its frames timed by the
// engine's main-loop clock (inmatch/main_loop_clock.h); this file owns the
// console, the stop signals and the socket layer. The status
// page and its chat input are status_console.cpp's; what a supervisor reads of
// the run (the tick health, the exit's line, the second listing's state) is
// run_reports.h's.

#include "run_reports.h"
#include "server.h"
#include "status_console.h"

#include "console_log.h"
#include "credentials.h"
#include "server_completion.h"

#include <base/io/file_io.h>
#include <base/io/log.h>
#include <base/proc/child_process.h>
#include <base/version/one_build.h>
#include <net/novaworld/lobby_identity.h> // read_primary_screen_size
#include <runtime/world/tick_accumulator.h>

#include "net_sockets.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <games/games.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
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

// The console's output code page as the process found it (0: untouched). The log and the status
// page are UTF-8 (the version line's middle-dot separators among them), which a console left in
// the system's OEM code page (437 on a US machine) shows as other glyphs; the server sets the
// console to UTF-8 while it runs and gives it back its own at the exit, as the shell that
// started it expects. A platform primitive.
UINT g_console_output_cp = 0;
void restore_console_output_cp() {
	if (g_console_output_cp != 0)
		SetConsoleOutputCP(g_console_output_cp);
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
	io::logf(io::LogLevel::kInfo, "opennova-serve: %s", server.version_line().c_str());
	// The build it runs, for its supervisor (--build-file): written once, before anything that
	// could end the start.
	if (!options.build_file.empty()) {
		std::string why;
		if (!io::write_file_atomic(options.build_file, serve::build_file_json(one_build()), why))
			io::logf(io::LogLevel::kWarn, "opennova-serve: --build-file: %s", why.c_str());
	}
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

	// The main loop: one session frame per pass, timed by the server's main-loop clock as the
	// game's frame is (engine inmatch/main_loop_clock.h). game.cfg's lock_framerate (1 by
	// default) locks each frame to 16 ms of banked time inside the session's bank, which
	// sleeps 1 ms at a time on GetTickCount's steps, as retail's loop paces a Serve Only host;
	// with the word clear the loop runs unpaced, as retail's does
	// [orig: Game_MainLoop @0x52B630 - the lock word @0x52B86C, Sleep(1) @0x52B8B2, no
	//  dedicated-host test].
	using clock = std::chrono::steady_clock;
	// A frame is late when it starts more than one logic quantum past its predecessor's own
	// (the run's tick health, --stats-file; instrumentation only).
	const auto quantum = std::chrono::nanoseconds(
			static_cast<int64_t>(1e9 * world::TickAccumulator::kTickDt));
	const auto started = clock::now();
	auto previous_start = started;
	int code = 0;
	// The status page stands in for the scene a retail dedicated host draws, each print headed
	// by the server's version line; stdin is its chat input line (status_console.h). A
	// supervised server (--status-page off) has neither.
	serve::StatusConsole status(stdout, server.version_line());
	serve::ConsoleInput input;
	if (options.status_page)
		input.start();
	serve::TickStats ticks;
	// The second listing's state as last written (--second-listing-file): rewritten when it
	// changes.
	std::optional<serve::SecondListingReport> listing_written;
	while (!g_stop.load()) {
		const auto now = clock::now();
		if (!server.frame()) {
			// The rotation's end and the remote admin's quit are the session's
			// own end: the router's teardown (PostMenu_RouteMissionExit
			// @0x56864F, the quit's @0x5684AB). Any other end of the session is
			// an exit by itself.
			if (server.rotation_ended() || server.quit()) {
				failure = server.rotation_ended() ? std::string(kRotationCompleted) : std::string(kAdminSessionEnded);
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
		// The render callback's end: the stamp a mission-start frame re-bases to, so the
		// frame after a map change banks none of its load.
		server.rendered();
		if (!options.stats_file.empty()) {
			// This frame's work, its frame lock's sleeps left out; the window's summary once a
			// minute.
			const int64_t done_ns = ns_of(clock::now());
			const int64_t lock_ns = server.last_frame_perf().bank_us * 1000;
			ticks.frame(ns_of(now), done_ns - lock_ns, ns_of(previous_start + 2 * quantum));
			if (const auto minute = ticks.take_if_due(done_ns)) {
				std::string why;
				if (!io::write_file_atomic(options.stats_file,
							serve::tick_minute_json(*minute, static_cast<int64_t>(std::time(nullptr))), why))
					io::logf(io::LogLevel::kWarn, "opennova-serve: --stats-file: %s", why.c_str());
			}
		}
		previous_start = now;
		if (!options.second_listing_file.empty()) {
			// After the frame's measure, so a write never counts in its time. A write that fails
			// is logged once and tried again at the state's next change.
			const serve::SecondListingReport listing = server.second_listing_report();
			if (!listing_written || *listing_written != listing) {
				listing_written = listing;
				std::string why;
				if (!io::write_file_atomic(
							options.second_listing_file, serve::second_listing_json(listing), why))
					io::logf(io::LogLevel::kWarn, "opennova-serve: --second-listing-file: %s",
							why.c_str());
			}
		}
	}
	server.stop();
	io::logf(io::LogLevel::kInfo, "opennova-serve: stopped");
	return code;
}

} // namespace

int main(int argc, char **argv) {
	opennova::games::install(); // every game's titles and protocols (games/games.h)
	opennova::games::install_rules(); // and the rules each runs: this program runs a match
#ifdef _WIN32
	if (const UINT cp = GetConsoleOutputCP();
			cp != 0 && cp != CP_UTF8 && SetConsoleOutputCP(CP_UTF8)) {
		g_console_output_cp = cp;
		std::atexit(restore_console_output_cp);
	}
#endif
	using namespace opennova;
	std::vector<std::string> args(argv + 1, argv + argc);
	serve::ServeOptions options;
	std::string error;
	const int parsed = serve::parse_serve_options(args, options, error, proc::own_command_line());
	if (parsed < 0) {
		std::fputs(serve::serve_usage(), stdout);
		return 0;
	}
	if (parsed > 0) {
		std::fprintf(stderr, "opennova-serve: %s\n%s", error.c_str(), serve::serve_usage());
		return 2;
	}
	// --version: the line the server would log as it starts, then the exit (opennova-hostd's
	// form: "opennova-serve ONE 0.0.22.123 (a1b2c3d, staging)" with no game named).
	if (options.print_version) {
		std::printf("opennova-serve %s\n", serve::serve_version_line(options).c_str());
		return 0;
	}
	// The primary desktop (server.h ServeOptions::desktop_width / desktop_height); a platform that
	// reports none leaves the headless pair.
	read_primary_screen_size(options.desktop_width, options.desktop_height);
	// The log sink masks every registered secret, so an account name, a
	// password or a session tag never reaches the console (console_log.h).
	serve::install_console_log(std::string(), options.log_debug);
	// The error a non-zero exit ends on, for the supervisor (--exit-file).
	const auto exit_with = [&options](int code, const std::string &failure) {
		std::string why;
		if ((code != 0 || server_completed(code, failure)) && !options.exit_file.empty() &&
				!io::write_file_atomic(options.exit_file, serve::exit_file_text(failure), why))
			io::logf(io::LogLevel::kWarn, "opennova-serve: --exit-file: %s", why.c_str());
		return code;
	};
	// The first listing's account (--credentials) and the second's (--also-credentials).
	const auto load_account = [&error](const char *flag, const std::string &path,
			serve::Credentials &out) {
		if (path.empty()) return true;
		if (!serve::load_credentials(path, out, error)) {
			error = std::string(flag) + ": " + error;
			return false;
		}
		for (const std::string *secret : { &out.user, &out.pass })
			serve::add_log_secret(*secret);
		return true;
	};
	if (!load_account("--credentials", options.credentials_path, options.credentials) ||
			!load_account("--also-credentials", options.also_credentials_path,
					options.also_credentials)) {
		io::logf(io::LogLevel::kError, "opennova-serve: %s", error.c_str());
		return exit_with(2, error);
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
