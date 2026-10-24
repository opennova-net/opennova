#include "options.h"

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <limits>

namespace opennova::updater {
namespace {
bool number(const std::string &s, uint64_t &out) {
	if (s.empty()) return false;
	uint64_t n = 0;
	for (char c : s) {
		if (c < '0' || c > '9' || n > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
			return false;
		n = n * 10 + (c - '0');
	}
	out = n;
	return true;
}
}

const char *usage() {
	return "update.exe [--check] /game <opennova|jo|dfx2> [--runtime-dir <dir>]\n"
			"  [--resource-dir <dir>] [--no-runtime] [--server <host>] [--port <n>]\n"
			"  [--parent-pid <pid>] [/RUN] [--run-arg <argument>]... [/VERBOSE]\n"
			"Legacy: update.exe <window-handle> /DIR <install> /RUN /VERBOSE\n"
			"Check exits: 0 current, 10 available, 1 failure, 2 usage.\n";
}

bool parse_options(const std::vector<std::string> &args,
		const std::filesystem::path &executable, Options &out, std::string &error) {
	Options o;
	o.runtime_dir = executable.parent_path();
	bool explicit_game = false;
	for (size_t i = 0; i < args.size(); ++i) {
		const std::string a = strutil::to_lower(args[i]);
		uint64_t n = 0;
		if (i == 0 && number(args[i], n)) {
			// The game's window handle: a nonzero one also means /RUN.
			// [orig: Cupdate2App_InitInstance @ 0x404820 (UPDATE.EXE), the atol @0x4048d6,
			//  Cupdate2Dlg_SetRunWhenDone @0x404966]
			o.parent_window = static_cast<uintptr_t>(n);
			o.legacy = true;
			if (n) o.run = true;
		} else if (a == "--check") o.check = true;
		else if (a == "--no-runtime") o.no_runtime = true;
		else if (a == "/run") o.run = true;
		else if (a == "/verbose") o.verbose = true;
		else if (a == "/force" || a == "/log") { /* retail ignores these */ }
		else if (a == "--help" || a == "/?") o.help = true;
		else {
			if (a != "/game" && a != "--runtime-dir" && a != "--resource-dir" &&
					a != "/dir" && a != "--server" && a != "--port" &&
					a != "--parent-pid" && a != "--run-arg") {
				error = "unknown option " + args[i]; return false;
			}
			if (++i == args.size()) { error = "missing value after " + a; return false; }
			const std::string &v = args[i];
			if (a == "--run-arg") o.run_args.push_back(v);
			else if (v.empty()) { error = "empty value after " + a; return false; }
			else if (a == "/game") { o.game = strutil::to_lower(v); explicit_game = true; }
			else if (a == "--runtime-dir") o.runtime_dir = io::os_path(v);
			else if (a == "--resource-dir" || a == "/dir") {
				o.resource_dir = io::os_path(v);
				if (a == "/dir") o.legacy = true;
			} else if (a == "--server") o.server = v;
			else if (a == "--port") {
				if (!number(v, n) || n == 0 || n > 65535) { error = "invalid port"; return false; }
				o.port = static_cast<uint16_t>(n);
			} else if (a == "--parent-pid") {
				if (!number(v, o.parent_pid) || o.parent_pid == 0 || o.parent_pid > 0xffffffffu) {
					error = "invalid parent process id"; return false;
				}
			}
		}
	}
	if (o.game != "opennova" && o.game != "jo" && o.game != "dfx2") {
		error = "/game must be opennova, jo or dfx2"; return false;
	}
	if (explicit_game) o.legacy = false;
	o.game_explicit = explicit_game;
	if (o.legacy) o.no_runtime = true;
	std::error_code ec;
	o.runtime_dir = std::filesystem::absolute(o.runtime_dir, ec).lexically_normal();
	if (ec) { error = "invalid runtime directory"; return false; }
	if (o.resource_dir.empty()) o.resource_dir = o.runtime_dir;
	o.resource_dir = std::filesystem::absolute(o.resource_dir, ec).lexically_normal();
	if (ec) { error = "invalid resource directory"; return false; }
	out = std::move(o);
	return true;
}
} // namespace opennova::updater
