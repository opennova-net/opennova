#include "options.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <thread>

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
	return "update.exe [--check] /game <acidgambit|jo|dfx2> [--runtime-dir <dir>]\n"
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
	if (o.game != "acidgambit" && o.game != "jo" && o.game != "dfx2") {
		error = "/game must be acidgambit, jo or dfx2"; return false;
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

std::vector<std::string> revision_handoff_arguments(const Options &options,
		const std::vector<std::string> &original, uint64_t updater_pid) {
	std::vector<std::string> args{"--runtime-dir", io::utf8_path(options.runtime_dir),
			"--parent-pid", std::to_string(updater_pid), "--forward-options"};
	const auto forward = [&args](const std::string &value) {
		args.push_back("--update-arg");
		args.push_back(value);
	};
	for (size_t i = 0; i < original.size(); ++i) {
		if (strutil::iequals(original[i], "--parent-pid")) { ++i; continue; }
		if (i == 0 && options.legacy && !original[i].empty() && original[i][0] >= '0' && original[i][0] <= '9') {
			forward("0");
			continue;
		}
		forward(original[i]);
		const std::string flag = strutil::to_lower(original[i]);
		const bool directory = flag == "--runtime-dir" || flag == "--resource-dir" || flag == "/dir";
		const bool valued = flag == "--run-arg" || flag == "/game" || flag == "--server" || flag == "--port";
		if ((directory || valued) && i + 1 < original.size()) {
			++i;
			forward(!directory ? original[i]
			                   : io::utf8_path(flag == "--runtime-dir" ? options.runtime_dir : options.resource_dir));
		}
	}
	return args;
}

bool read_revision_arguments(const std::vector<std::string> &args, RevisionArguments &out,
		std::string &error) {
	RevisionArguments read;
	bool forwarded = false;
	for (size_t i = 0; i < args.size(); ++i) {
		if (args[i] == "--forward-options") {
			forwarded = true;
		} else if (args[i] == "--update-arg") {
			if (++i == args.size()) { error = "Missing --update-arg value."; return false; }
			read.relaunch.push_back(args[i]);
		} else if (args[i] == "--runtime-dir" || args[i] == "--parent-pid") {
			read.own.push_back(args[i]);
			if (++i == args.size()) { error = "Missing helper option value."; return false; }
			read.own.push_back(args[i]);
		} else {
			error = "unknown option " + args[i];
			return false;
		}
	}
	if (!forwarded) { error = "revupdat is started by update.exe (--forward-options)."; return false; }
	out = std::move(read);
	return true;
}

bool install_staged_updater(const std::filesystem::path &runtime_dir, bool wait_first, std::string &error) {
	const std::filesystem::path staged = io::os_path(runtime_dir / "update.new");
	std::error_code ec;
	if (!std::filesystem::exists(staged, ec)) return true;
	if (wait_first) std::this_thread::sleep_for(std::chrono::seconds(2));
	const std::filesystem::path image = io::os_path(runtime_dir / "update.exe");
	if (!std::filesystem::remove(image, ec)) {
		// DeleteFileA fails on a missing file too (ERROR_FILE_NOT_FOUND).
		const int code = ec ? ec.value() : int(std::errc::no_such_file_or_directory);
		error = "ERROR: Could not delete 'Update.exe'. (" + std::to_string(code) + ")";
		return false;
	}
	if (!io::rename_with_retry(staged, image, ec)) {
		error = "ERROR: Could not move 'Update.new' to 'Update.exe'. (" + std::to_string(ec.value()) + ")";
		return false;
	}
	return true;
}
} // namespace opennova::updater
