#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

// The argument vector Play spawns (ADR 0046 d8): the game runtime on a build directory,
// packed data only (no `/d`, no `--loose-root`), its working directory and its log in the run
// directory Play took for it (run/run_directory.h, S13 A8) so the editor can tail the log before
// the runtime MCP answers and nothing is written into the build, and the MCP endpoint on the port
// the caller allocated. A source run drives the Godot binary at the project instead of a
// packaged executable; the game flags ride after `--` in both forms because the runtime
// reads Godot's own arguments and its user arguments in one pass.
struct LaunchPlan {
	std::string executable;
	std::vector<std::string> args; // without argv[0]
	std::string working_dir;       // the run directory
	std::string log_file;          // <run>/session.log (the game install's own: <run>/_filelog.txt)
	int mcp_port = 0;              // 0 = no endpoint
	std::string build_dir;         // the build it runs, which stays as the build wrote it
	// The game data the game mounts: the build directory for the standalone game; for an expansion
	// (ADR 0046 S16) the run directory, where prepare_expansion_run put the install's base game and
	// the build's expansion folder, mounted with `/exp <expansion>`.
	std::string resource_dir;
	std::string expansion; // "" for the standalone game
	// The game's window starts behind every other and never takes the foreground (a play {behind} over the
	// editor MCP, the MCP gaps lane: a person works at the machine), as the platform starts a process so
	// (the Shell's: shown without activation, kept at the bottom while it starts). Not on the command line.
	bool behind = false;
	// The files the staging put in the run directory, '/'-separated paths under it (the build's files, the
	// install's program and Bink DLL, an expansion's base game and folder; never a file the run kept, nor
	// the install's configuration and saves seeded where it had none of its own): its staging record
	// (run/run_directory.h, RunStaging), which the next Play removes before it stages again. Filled as each
	// file is staged, so a staging that failed partway names what it put there too.
	std::vector<std::string> staged;
};

// The packaged runtime (opennova.exe) on `build_dir`, in `run_dir`. `engine_args` are Godot's own
// options for the child (`--headless`, `--windowed`, `--quit-after N`), placed before
// the `--` that starts the game flags. A build of the expansion `expansion` (ADR 0046 S16) runs on
// the run directory prepare_expansion_run staged, with `/exp <expansion>`; so does a build
// prepare_runtime_run staged there (`on_run_dir`: Play from here's, whose mission the run directory's
// archive carries with its start, run/play_start.h).
LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &run_dir, const std::string &game_code, int mcp_port,
                                 const std::string &mission = std::string(),
                                 const std::vector<std::string> &engine_args = {},
                                 const std::string &expansion = std::string(), bool on_run_dir = false);

// The same run from source: `godot --path <project> res://game/game_runtime_root.tscn -- ...`, with
// `--working-dir <run>` among the game flags, since Godot's `--path` moves the process's working
// directory to the project and the game keeps its saves in the directory it was started in.
LaunchPlan make_source_launch_plan(const std::string &godot_executable, const std::string &godot_project_dir,
                                   const std::string &build_dir, const std::string &run_dir,
                                   const std::string &game_code, int mcp_port,
                                   const std::string &mission = std::string(),
                                   const std::vector<std::string> &engine_args = {},
                                   const std::string &expansion = std::string(), bool on_run_dir = false);

// The run directory of a build of the expansion `expansion` (ADR 0046 S16), laid out as an install
// the game runs `/exp <expansion>` in: at its root the install's boot archives, the loose files it
// ships beside them (list_install_loose_files) but a file the expansion's archives pack (under /d the
// root copy would stand over the packed edit [orig: FileSystem_OpenFile @ 0x75b1c0]), and the files
// the game reads from its folder by name (score.ini, earlyerr.txt, admin.cfg) where the run directory
// holds none of its own (seeded: a copy the game rewrote in a run before stays, and is no staged file);
// and `expansion/<expansion>/` a directory of its own holding the build's expansion folder's files, never
// a link to the folder: the game writes its expansion's weapon.sav there [orig:
// PlayerProfile_LoadAllFromDisk @ 0x54f4d0, @ 0x54f6c7]. A file the game only reads (its archives,
// videos, music and dialog banks, NovaWorld screens) is linked; every other, which the game may write
// (its configuration, saves, the NovaWorld cookie jar nw_cdata.coo, its logs), is copied fresh every
// run, never linked and never cached, so no write reaches the install, the build or the cache. A staged
// file replaces the name the run directory held (its name removed, never written through); each is
// added to `staged` ('/'-separated under the run directory, LaunchPlan::staged) as it is staged. Where
// the file system will not link an install's file to the run directory (another volume), it is copied
// into `copy_cache` (the project's `.opennova/install_copy/`, a folder per install) and linked from
// there, kept while the install's file keeps its size and a last write settled when it was copied (git's
// racy rule, io::file_stamp_settled); the cache keeps the current install's files the run used, nothing
// else. The install and the build are only read. False with `error`: no install
// (play.install_missing), an install with none of the game's archives, a build without the expansion's
// folder, or a file not staged (play.install_copy). `link` gives a file a second name (link_file, but
// in a test: one refusing what another volume's link would).
using FileLink = std::function<bool(const std::string &from, const std::string &to, std::string &error)>;
bool prepare_expansion_run(const std::string &install, const std::string &build_dir, const std::string &expansion,
                           const std::string &run_dir, const std::string &copy_cache, Diagnostic &error,
                           const FileLink &link = link_file, std::vector<std::string> *staged = nullptr);

// A standalone build staged in `run_dir` for the runtime to mount there (make_play_launch_plan's
// `on_run_dir`): every file of the build but its record, one the game only reads linked (copied where
// the file system will not link it), every other copied, each in place of what the run kept under its
// name and added to `staged` (LaunchPlan::staged), as the game install's staging stages them. The
// build is only read. False with `error` (play.install_copy).
bool prepare_runtime_run(const std::string &build_dir, const std::string &run_dir, Diagnostic &error,
                         std::vector<std::string> *staged, const FileLink &link = link_file);

// The game install's own program, which Play in the game install starts (and the install check looks for).
inline constexpr const char *kInstallExecutable = "Jointops.exe";

// The game install's game runs one at a time, machine-wide: it takes the first of its numbered
// semaphores ("semaphore:NOVALOGIC JOINT OPERATIONS:<n>", n from 1) no other process holds, and one
// that took any but the first brings the running game's window forward and quits at once with code 0,
// before it loads its data, unless it was started /HOST, /MANY or /SERVEONLY [orig:
// Game_ParseCommandLineAndInit @ 0x4a7325 the name, @ 0x4a7336 -> CSessionId_AcquireUnique @ 0x4a67c0
// (CreateSemaphoreA, a wait of 0, the next number on WAIT_TIMEOUT); the flags @ 0x4a7743, @ 0x4a7779,
// @ 0x4a79b8; the gate @ 0x4a7c03 (n > 1, none of them), FindWindowA("NLGAMECLASS") @ 0x4a7c17,
// crt_exit(0) @ 0x4a7c3f]. The first one held is a game running; Play passes none of those flags.
inline constexpr const char *kInstallInstanceSemaphore = "semaphore:NOVALOGIC JOINT OPERATIONS:1";

// The game install's own file log, `_filelog.txt` in its working directory, which `/FRISK` turns on
// [orig: Game_ParseCommandLineAndInit @ 0x4a768c -> File_SetLoggingEnabled @ 0x75a470]: a line for
// every file the game's file layer OPENED, appended as it opens it [orig: File_LogFileAccess @
// 0x75a480]: "PFF LOADED FILE: <name>" for an entry an archive served [orig: PFF_OpenFile @
// 0x7688da], "LOADED FILE: <path>" for a file opened from disk, an archive itself among them [orig:
// File_OpenRead @ 0x75a639 (the archives through PFF_Open @ 0x768330, the saves through
// PlayerProfile_LoadAllFromDisk @ 0x54f5a0); FileSystem_OpenFile @ 0x75b2d6; FileSystem_GetFileSize @
// 0x75b436; FileSystem_ReadFileEx @ 0x75b91b], so one file may be logged more than once. Only an open
// that succeeded is logged (each call follows a handle that is not -1, an entry PFF_FindEntry found):
// a file the game looked for and did not find never is, so the log cannot say what the game lacked. A
// file the game opens outside that layer is not logged either: its configuration, read through the C
// runtime's fopen [orig: File_ParseASCIIFileWithCallback @ 0x53d980, from Game_LoadConfig @ 0x551499].
//
// The game opens the log exclusively for each line (`_lopen(OF_WRITE | OF_SHARE_EXCLUSIVE)`, then
// `_llseek` to its end) and, when that open fails, makes the file anew with `_lcreat`, which
// truncates it; its first line deletes the log a run before left [orig: File_LogFileAccess @
// 0x75a4c2 DeleteFileA, @ 0x75a4d2 _lopen(0x11), @ 0x75a4e8 _lcreat]. A reader holding the file
// open while the game appends (a tail) therefore cuts the log to its last line: the editor reads it
// once the game has exited, never while it runs.
inline constexpr const char *kInstallFileLogName = "_filelog.txt";

// What a file log says the game loaded, each name once (the first spelling, compared without case
// as the game's file calls compare them), in the order the game first opened it.
struct FileAccessLog {
	size_t lines = 0;                       // the log's lines
	std::vector<std::string> archives;      // the archives the game opened from disk (a .pff)
	std::vector<std::string> from_archives; // the files the archives served
	std::vector<std::string> from_disk;     // the files opened from disk but the archives themselves
	bool operator==(const FileAccessLog &o) const {
		return lines == o.lines && archives == o.archives && from_archives == o.from_archives &&
		       from_disk == o.from_disk;
	}
};

// One line of a file log added to `log` (its line end already cut, a '\r' too); a line neither form
// starts is counted and nothing else.
void add_file_access_line(FileAccessLog &log, const std::string &line);
// A whole file log's text read into a FileAccessLog (lines ended by "\n", the game's, or "\r\n").
FileAccessLog parse_file_access_log(const std::string &text);

// The historical Jointops.exe /w /d /FRISK launch in `run_dir`: the game install's game opens its
// archives and, under /d, its loose files from its working directory [orig: PFF_OpenAllArchives @
// 0x4a4310, CWD-relative _lopen; docs/vfs/vfs-pff-mount-re.md] and writes there (game.cfg, its
// saves, its _filelog.txt), so the run directory gets the build's files (one the game only reads
// linked, copied where the file system cannot link it; every other, which the game may write,
// copied), the install's executable and Bink DLL, a game.cfg, the build's own when the project has
// one, and the install's game.cfg, player.sav and weapon.sav and the files it reads from its folder by
// name (score.ini, earlyerr.txt, admin.cfg) where neither the project nor the run directory has its own
// (seeds: the profile, bindings and device the player starts with). A seed never replaces the run
// directory's own copy, newer or not: the game rewrites its game.cfg as it quits every run [orig: Game_Run
// @ 0x4a7fff Game_SaveConfig after the loop], so an install's copy newer than the run's says only that the
// install was played since; the run keeps what its own runs chose (the device its dialog named above
// all), and a fresh Play (RunTake::fresh) seeds again. The seeds are not staged files (LaunchPlan::staged):
// the next Play keeps them as the game left them. The required files are checked before any is copied,
// so a missing one launches nothing. The game install and the build directory are only read. A build of
// the expansion `expansion` (ADR 0046 S16) is staged as prepare_expansion_run stages it, the install's
// own game.cfg and saves beside it, and the game launched `/w /d /exp <expansion> /FRISK`. `link` gives a
// file a second name, as prepare_expansion_run's.
bool prepare_retail_launch_plan(const std::string &retail_directory, const std::string &build_dir,
                                const std::string &run_dir, LaunchPlan &out, Diagnostic &error,
                                const std::string &expansion = std::string(),
                                const std::string &copy_cache = std::string(), const FileLink &link = link_file);

// Strict Play in the game install: the game as a player who dropped the install's Jointops.exe into
// the build's folder runs it. The run directory gets the build's files (one the game only reads linked,
// copied where the file system cannot link it; every other copied, as prepare_retail_launch_plan stages
// them, so no write reaches the build) and the install's executable and Bink DLL (copied), nothing else
// of the install: no configuration, save, score table, early error text or admin configuration (a
// game.cfg the build holds is the project's own and is staged with the rest), so the game boots on the
// build alone and writes its own, which strict Play's own run directory (`.opennova/run/strict/<n>`)
// keeps for the next strict Play, whatever Plays of another mode run between (a game.cfg naming the
// adapter spares it the device dialog: run/run_directory.h). Launched `/w /FRISK`, without `/d`: the
// game reads its archives first, as a player's launch does
// [orig: Game_ParseCommandLineAndInit @ 0x4a7667 sets the /d flag;
// Game_InitSubsystems turns on loose-first resolution after the archives mount, @ 0x4a6fa3 ->
// FileSystem_SetSearchLooseFirst @ 0x75a5a0; docs/vfs/vfs-pff-mount-re.md]. The executable and the
// Bink DLL are checked before anything is staged. An expansion (`expansion` not "") is refused
// (play.strict_expansion, strict_expansion_refusal): it would play over the install's archives, not
// over its base game's build, which a project cannot name yet.
bool prepare_strict_install_launch_plan(const std::string &install, const std::string &build_dir,
                                        const std::string &run_dir, const std::string &expansion, LaunchPlan &out,
                                        Diagnostic &error, const FileLink &link = link_file);

// Why strict Play of the expansion `expansion` is refused (play.strict_expansion): the one finding
// Play's refusal before a build and the staging both give.
Diagnostic strict_expansion_refusal(const std::string &expansion);

// Strict Play's first run. With no game.cfg beside it, the game's adapter name and GUID are empty, so it
// opens its device dialog (VIDEO_TEST: the adapters, OK and Cancel), modal, waiting on the player, before
// its menu [orig: Game_InitSubsystems @ 0x4a711c the compare (Mission_HasMapOrNameChanged @ 0x53de40, a
// misnomer), @ 0x4a7125 -> Game_RunVideoTestDialog @ 0x53ec10, DialogBoxParamA "VIDEO_TEST"]. OK saves
// game.cfg and boots on [@ 0x53edb6 Game_SaveConfig]; Cancel ends the dialog with 0, Game_InitSubsystems
// returns 0 [@ 0x4a712c -> @ 0x4a6d1f], and Game_Run skips the main loop, saves game.cfg anyway and
// returns: exit code 0, game.cfg written, no menu [orig: Game_Run @ 0x4a7fb0, Game_SaveConfig after the
// loop @ 0x4a7fff; WinMain returns 0, play_session.h]. Any quit at the menu (its window closed, a blank
// menu's one EXIT control) likewise writes game.cfg and exits with 0. So a first run may write its own
// game.cfg and quit before a player sees the game, and was seen to (2026-10-05, the hand-staged runs;
// and the editor's, the dialog answered OK and the window closed as the menu came up: 25 log lines,
// a game.cfg of 5552 bytes, exit code 0), and seen not to. Strict Play starts it once more when, and only
// when, the first run exited on its own with code 0 within this window of its start (the dialog and the
// device's set-up take tens of seconds: the menu came up ~36 s in, the dialog answered at ~8 s), and the
// run directory, which held no game.cfg before it started, holds one now. A game.cfg a Cancel wrote
// still names no adapter, so the game started again opens its device dialog again. A run directory that
// kept a run's game.cfg (run/run_directory.h) has no first run: its game opens no dialog while the cfg
// names the adapter the game finds (hw3d_deviceno's, by hw3d_name and hw3d_guid).
inline constexpr int64_t kStrictFirstRunWindowMs = 60000;
bool strict_first_run_starts_again(bool had_config, bool has_config, bool exited_on_its_own, int64_t exit_code,
                                   int64_t ran_ms, bool started_again);

// The plan as one line for a log or the Output window (arguments quoted when needed).
std::string launch_plan_command_line(const LaunchPlan &plan);

// How Play starts the game: the packaged runtime beside the editor, or a source run that
// drives the Godot binary at the checkout. The shell makes it (make_play_launcher: it knows
// where it runs from and allocates the MCP port); the settings can name another runtime.
struct PlayLauncher {
	bool source_run = false;
	std::string executable;          // opennova.exe, or the Godot binary for a source run
	std::string godot_project_dir;   // the checkout's godot/ directory (source runs only)
	std::vector<std::string> engine_args; // Godot options for the child, before the game flags
	int mcp_port = 0;                // 0 = no endpoint
};

// The launcher of an editor running as `editor_executable`: a source run drives that binary
// on `godot_project_dir`; a packaged editor runs the runtime the package puts beside it, in
// the release layout's runtime/ directory next to the editor's own
// (<editor dir>/../runtime/opennova.exe).
PlayLauncher make_play_launcher(bool source_run, const std::string &editor_executable,
                                const std::string &godot_project_dir, int mcp_port,
                                std::vector<std::string> engine_args);

// What Play launches, as its embedder tells the session when asked (ADR 0046 S13 A2): with
// `with_mcp_port` when the game is about to be spawned, its build landed, so the embedder allocates
// the port of the game's MCP endpoint then (0 when it has none to give); without, for what the
// view shows of the runtime (whether the run drives the source checkout, the binary).
using PlayLauncherSource = std::function<PlayLauncher(bool with_mcp_port)>;

} // namespace opennova::editor
