// Revision hand-off: wait for UPDATE to release its image, replace update.new, relaunch.
// [orig: RevUpdate_WndProc @0x401290 (REVUPDAT.EXE), docs/update/updater-re.md section 8]
#include "options.h"
#include "platform.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <formats/update/update_wizard.h>

#include <chrono>
#include <algorithm>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
using namespace opennova;

int run(const std::vector<std::string> &args, const std::filesystem::path &executable, std::string &error) {
	std::vector<std::string> own, forwarded;
	bool explicit_forwarding = false;
	for (size_t i = 0; i < args.size(); ++i) {
		if (args[i] == "--forward-options") {
			explicit_forwarding = true;
		} else if (args[i] == "--update-arg") {
			explicit_forwarding = true;
			if (++i == args.size()) { error = "Missing --update-arg value."; return 2; }
			forwarded.push_back(args[i]);
		} else if (args[i] == "--runtime-dir" || args[i] == "--parent-pid") {
			own.push_back(args[i]);
			if (++i == args.size()) { error = "Missing helper option value."; return 2; }
			own.push_back(args[i]);
		} else {
			// Stock UPDATE launches REVUPDAT with /RUN and its original command line.
			forwarded.push_back(args[i]);
		}
	}
	updater::Options options;
	if (!updater::parse_options(own, executable, options, error)) return 2;
	if (!updater::wait_for_parent(options.parent_pid, 0, error)) return 1;
	if (!options.parent_pid) std::this_thread::sleep_for(std::chrono::seconds(2));
	const auto replacement = options.runtime_dir / "update.new";
	std::error_code ec;
	if (std::filesystem::exists(replacement, ec) &&
			!io::replace_file(io::utf8_path(replacement), io::utf8_path(options.runtime_dir / "update.exe"), error)) return 1;
	if (!io::write_file_atomic(io::utf8_path(options.runtime_dir / "update.inf"),
			std::string(update::kUpdateInfText), error)) return 1;
	if (!explicit_forwarding) {
		if (forwarded.empty()) forwarded.push_back("/RUN");
		// Retail prepends /RUN before UPDATE's original positional hwnd. Give the new
		// updater its own harmless positional handle and discard the original one.
		for (size_t i = 0; i < std::min<size_t>(2, forwarded.size()); ++i) {
			if (!forwarded[i].empty() && std::all_of(forwarded[i].begin(), forwarded[i].end(),
						[](char c) { return c >= '0' && c <= '9'; })) {
				forwarded.erase(forwarded.begin() + i); break;
			}
		}
		forwarded.insert(forwarded.begin(), "0");
	}
	const auto launch_dir = std::filesystem::current_path(ec);
	if (ec) { error = "Cannot determine the launch directory."; return 1; }
	return updater::launch_detached(options.runtime_dir / "update.exe", launch_dir, forwarded, error) ? 0 : 1;
}

int app_main(const std::vector<std::string> &args, const std::filesystem::path &executable) {
	std::string error;
	const int result = run(args, executable, error);
	if (result) {
		updater::write_stdout("ERROR " + error + "\n");
#ifdef _WIN32
		MessageBoxW(nullptr, updater::wide(error).c_str(), L"Update Revision Control", MB_OK | MB_ICONERROR);
#endif
	}
	return result;
}
}
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
	return app_main(opennova::updater::process_arguments(), opennova::updater::executable_path());
}
#else
int main(int argc, char **argv) {
	return app_main(opennova::updater::process_arguments(argc, argv), opennova::updater::executable_path(argv[0]));
}
#endif
