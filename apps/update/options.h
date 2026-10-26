#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace opennova::updater {

struct Options {
	bool check = false;
	bool no_runtime = false;
	bool run = false;
	bool verbose = false;
	bool legacy = false;
	bool help = false;
	bool game_explicit = false;
	std::string game = "acidgambit";
	std::filesystem::path runtime_dir;
	std::filesystem::path resource_dir;
	std::string server;
	uint16_t port = 0;
	uint64_t parent_pid = 0;
	uintptr_t parent_window = 0;
	std::vector<std::string> run_args;
};

// Arguments exclude argv[0]. Defaults are based on the executable, never the caller's cwd.
bool parse_options(const std::vector<std::string> &args,
		const std::filesystem::path &executable, Options &out, std::string &error);
const char *usage();

// The self-update hand-off (D-UPD-4): once update.exe has staged update.new it starts revupdat,
// which replaces update.exe and relaunches it with the same install, as retail's REVUPDAT relaunches
// UPDATE with its own command line. [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE),
// US_RUN_REVUPDAT @ 0x408b18; RevUpdate_WndProc @ 0x401290 (REVUPDAT.EXE), the relaunch @ 0x40140d]
//
// revupdat's arguments from update.exe: its own (--runtime-dir, --parent-pid `updater_pid`, for
// it to wait for this process, and --forward-options), then each of update.exe's `original`
// arguments as an --update-arg, but --parent-pid and its value (the game's, which has exited),
// a legacy launch's window handle as "0" (stale by the relaunch, it must not close a window that
// reuses it) and the directories as `options` resolved them. A value-taking flag carries its
// value, so a --run-arg value that looks like a flag is never read as one.
std::vector<std::string> revision_handoff_arguments(const Options &options,
		const std::vector<std::string> &original, uint64_t updater_pid);

// revupdat's reading of its command line: its own options (--runtime-dir, --parent-pid), for
// parse_options, and the arguments it relaunches update.exe with. From update.exe those are the
// --update-arg values; from a stock UPDATE.EXE (no --forward-options), which starts REVUPDAT as
// `revupdat.exe /RUN <its command line>`, its own command line, a numeric window handle among its
// first two dropped and "0" put first, as the new updater's own stand-in handle. False with
// `error` when one of its own options lacks its value.
struct RevisionArguments {
	std::vector<std::string> own;
	std::vector<std::string> relaunch;
};
bool read_revision_arguments(const std::vector<std::string> &args, RevisionArguments &out,
		std::string &error);

// revupdat's step once the updater that started it has exited: a staged update.new put in
// update.exe's place as retail's REVUPDAT does, update.exe deleted and then update.new moved over
// it, each step's failure with its own text and the OS error code; nothing to do without an
// update.new. With `wait_first` (no updater process was awaited: a stock UPDATE.EXE started this)
// it first sleeps retail's 2 s, which retail sleeps only when there is an update.new. A failure
// stops here, keeping update.new for the next run, where retail's box does not stop it: it goes
// on and deletes update.new, then relaunches update.exe, the old image after a failed delete
// (which fetches and hands over again), none after a failed move (PERMANENT, ours stops).
// [orig: RevUpdate_WndProc @ 0x401290 (REVUPDAT.EXE): update.new's test @0x40136f, Sleep(2000)
//  @0x40137e, DeleteFileA @0x40138f and its text @0x4070d0, MoveFileA @0x4013c4 and its text
//  @0x407094; RevUpdate_FatalError @ 0x401140 (REVUPDAT.EXE) shows its box and returns]
bool install_staged_updater(const std::filesystem::path &runtime_dir, bool wait_first, std::string &error);

// The box revupdat shows when update.exe does not start again, and its caption, both retail's.
// [orig: RevUpdate_WndProc @ 0x401290 (REVUPDAT.EXE), the relaunch @0x401419 and its text @0x407054;
//  RevUpdate_FatalError @ 0x401140 (REVUPDAT.EXE): MessageBoxA(.., kRevUpdateTitle, 0x40000 = MB_TOPMOST)
//  @0x401148..0x401159]
inline constexpr const char *kRevupdatRelaunchFailed = "ERROR: Could not re-launch 'Update.exe'.";
inline constexpr const char *kRevupdatTitle = "Update Revision Control";

} // namespace opennova::updater
