// Revision hand-off: wait for UPDATE to release its image, replace update.new, relaunch.
// [orig: RevUpdate_WndProc @0x401290 (REVUPDAT.EXE), docs/update/updater-re.md section 8]
#include "options.h"
#include "platform.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <formats/update/update_wizard.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
using namespace opennova;

int run(const std::vector<std::string> &args, const std::filesystem::path &executable, std::string &error) {
	updater::RevisionArguments revision;
	if (!updater::read_revision_arguments(args, revision, error)) return 2;
	updater::Options options;
	if (!updater::parse_options(revision.own, executable, options, error)) return 2;
	if (!updater::wait_for_parent(options.parent_pid, 0, error)) return 1;
	// update.inf first, then the swap; one that cannot be written is skipped [orig:
	// RevUpdate_WndProc @ 0x401290 (REVUPDAT.EXE): the write @0x4012fd..0x401356, passed over
	// when the file does not open @0x40130a, ahead of update.new's test @0x40136f].
	std::string unwritten;
	(void)io::write_file_atomic(io::utf8_path(options.runtime_dir / "update.inf"),
			std::string(update::kUpdateInfText), unwritten);
	if (!updater::install_staged_updater(options.runtime_dir, !options.parent_pid, error)) return 1;
	std::error_code ec;
	const auto launch_dir = std::filesystem::current_path(ec);
	if (ec) { error = "Cannot determine the launch directory."; return 1; }
	if (!updater::launch_detached(options.runtime_dir / "update.exe", launch_dir, revision.relaunch, error)) {
		error = updater::kRevupdatRelaunchFailed;
		return 1;
	}
	return 0;
}

int app_main(const std::vector<std::string> &args, const std::filesystem::path &executable) {
	std::string error;
	const int result = run(args, executable, error);
	if (result) {
		updater::write_stdout(error + "\n");
#ifdef _WIN32
		// The box retail's REVUPDAT shows: its caption, MB_TOPMOST (0x40000) and no icon.
		// [orig: RevUpdate_FatalError @ 0x401140 (REVUPDAT.EXE), its flags @0x401148]
		MessageBoxW(nullptr, updater::wide(error).c_str(), updater::wide(updater::kRevupdatTitle).c_str(),
				MB_OK | MB_TOPMOST);
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
