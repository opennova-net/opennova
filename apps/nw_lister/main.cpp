// opennova-nw-lister: lists one server on a NovaWorld master without running the game. The NWU session,
// the host role, the lobby HTTP machines and the remote-admin codec are the engine's
// (net/novaworld, net/admin); this file is the command line, the stop signals and the loop.

#include "console_log.h"
#include "lister.h"
#include "listing.h"

#include "net_sockets.h"

#include <base/io/log.h>
#include <base/io/strutil.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

using namespace opennova::nw_lister;
using opennova::io::LogLevel;

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
HANDLE g_done = nullptr; // set once the lister deregistered

BOOL WINAPI on_console_ctrl(DWORD type) {
	g_stop = true;
	// Closing the window, a logoff or a shutdown ends the process when this returns; hold it
	// (Windows allows about 5 s) so the loop can deregister first.
	if ((type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) && g_done)
		WaitForSingleObject(g_done, 4500);
	return TRUE;
}
#else
void on_stop_signal(int) { g_stop = true; }
#endif

struct Args {
	ListerOptions lister;
	std::string credentials_path;
	std::string log_path;
	std::string stop_file;
	bool verbose = false;
};

void usage() {
	std::fprintf(stderr,
	             "opennova-nw-lister - list one server on a NovaWorld master without the game\n\n"
	             "usage: opennova-nw-lister --listing FILE.json [options]\n"
	             "  --credentials FILE      KEY=VALUE file: NOVAWORLD_USER / NOVAWORLD_PASS (the account\n"
	             "                          login and HOSTKEY), ADMIN_USER / ADMIN_PASS (--admin)\n"
	             "  --master-host HOST      the NovaWorld gate (default 127.0.0.1)\n"
	             "  --master-gate-port N    the gate's UDP port (default %u)\n"
	             "  --allow-public          allow destinations off 127.0.0.0/8 (a live master)\n"
	             "  --admin HOST[:PORT]     take the players, map and time left from the game server's\n"
	             "                          remote-admin port (default port %u)\n"
	             "  --log FILE              append the log to FILE as well as stderr\n"
	             "  --verbose               log the debug lines too\n"
	             "  --stop-file PATH        deregister and exit when PATH appears\n",
	             opennova::GATE_DEFAULT_PORT, kAdminDefaultPort);
}

bool parse_port(const std::string &text, uint16_t &out) {
	const auto value = opennova::strutil::parse_int(text);
	if (!value || *value <= 0 || *value > 65535) return false;
	out = static_cast<uint16_t>(*value);
	return true;
}

bool parse_args(int argc, char **argv, Args &args) {
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto next = [&](std::string &out) {
			if (i + 1 >= argc) return false;
			out = argv[++i];
			return true;
		};
		std::string value;
		if (a == "--listing") {
			if (!next(args.lister.listing_path)) return false;
		} else if (a == "--credentials") {
			if (!next(args.credentials_path)) return false;
		} else if (a == "--master-host") {
			if (!next(args.lister.master_host)) return false;
		} else if (a == "--master-gate-port") {
			if (!next(value) || !parse_port(value, args.lister.master_gate_port)) return false;
		} else if (a == "--allow-public") {
			args.lister.allow_public = true;
		} else if (a == "--admin") {
			if (!next(value)) return false;
			const size_t colon = value.rfind(':');
			args.lister.admin_host = value.substr(0, colon);
			if (colon != std::string::npos && !parse_port(value.substr(colon + 1), args.lister.admin_port))
				return false;
		} else if (a == "--log") {
			if (!next(args.log_path)) return false;
		} else if (a == "--verbose") {
			args.verbose = true;
		} else if (a == "--stop-file") {
			if (!next(args.stop_file)) return false;
		} else {
			return false;
		}
	}
	return !args.lister.listing_path.empty();
}

uint32_t now_ms() {
	return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

bool stop_requested(const std::string &stop_file) {
	if (g_stop) return true;
	std::error_code ec;
	return !stop_file.empty() && std::filesystem::exists(stop_file, ec);
}

int run(const Args &args) {
	Lister lister(args.lister);
	if (!lister.start()) return lister.exit_code();
	while (lister.tick(now_ms())) {
		if (stop_requested(args.stop_file)) {
			lister.stop();
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return lister.exit_code();
}

} // namespace

int main(int argc, char **argv) {
	Args args;
	if (!parse_args(argc, argv, args)) {
		usage();
		return kExitUsage;
	}
	install_console_log(args.log_path, args.verbose);
	if (!args.credentials_path.empty()) {
		std::string error;
		if (!load_credentials(args.credentials_path, args.lister.credentials, error)) {
			opennova::io::logf(LogLevel::kError, "[credentials] %s", error.c_str());
			return kExitBadInput;
		}
		const Credentials &c = args.lister.credentials;
		for (const std::string *secret : {&c.user, &c.pass, &c.admin_user, &c.admin_pass}) add_log_secret(*secret);
	}
	if (!args.lister.admin_host.empty() && !args.lister.credentials.admin_present()) {
		opennova::io::logf(LogLevel::kError, "[credentials] --admin needs ADMIN_USER and ADMIN_PASS in --credentials");
		return kExitBadInput;
	}
#ifdef _WIN32
	g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(on_console_ctrl, TRUE);
#else
	std::signal(SIGINT, on_stop_signal);
	std::signal(SIGTERM, on_stop_signal);
#endif
	if (opennova::net::startup() != 0) {
		opennova::io::logf(LogLevel::kError, "[net] socket startup failed");
		return kExitNetwork;
	}
	const int rc = run(args);
	opennova::net::shutdown();
	opennova::io::logf(LogLevel::kInfo, "[main] exit %d", rc);
#ifdef _WIN32
	SetEvent(g_done);
#endif
	return rc;
}
