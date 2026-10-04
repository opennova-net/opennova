#pragma once

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
};

// The packaged runtime (opennova.exe) on `build_dir`, in `run_dir`. `engine_args` are Godot's own
// options for the child (`--headless`, `--windowed`, `--quit-after N`), placed before
// the `--` that starts the game flags. A build of the expansion `expansion` (ADR 0046 S16) runs on
// the run directory prepare_expansion_run staged, with `/exp <expansion>`.
LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &run_dir, const std::string &game_code, int mcp_port,
                                 const std::string &mission = std::string(),
                                 const std::vector<std::string> &engine_args = {},
                                 const std::string &expansion = std::string());

// The same run from source: `godot --path <project> res://game/game_runtime_root.tscn -- ...`, with
// `--working-dir <run>` among the game flags, since Godot's `--path` moves the process's working
// directory to the project and the game keeps its saves in the directory it was started in.
LaunchPlan make_source_launch_plan(const std::string &godot_executable, const std::string &godot_project_dir,
                                   const std::string &build_dir, const std::string &run_dir,
                                   const std::string &game_code, int mcp_port,
                                   const std::string &mission = std::string(),
                                   const std::vector<std::string> &engine_args = {},
                                   const std::string &expansion = std::string());

// The run directory of a build of the expansion `expansion` (ADR 0046 S16), laid out as an install
// the game runs `/exp <expansion>` in: at its root the install's boot archives, the loose files it
// ships beside them (list_install_loose_files) but a file the expansion's archives pack (under /d the
// root copy would stand over the packed edit [orig: FileSystem_OpenFile @ 0x75b1c0]), and the files
// the game reads from its folder by name (score.ini, earlyerr.txt, admin.cfg); and
// `expansion/<expansion>/` a directory of its own holding the build's expansion folder's files, never
// a link to the folder: the game writes its expansion's weapon.sav there [orig:
// PlayerProfile_LoadAllFromDisk @ 0x54f4d0, @ 0x54f6c7]. A file the game only reads (its archives,
// videos, music and dialog banks, NovaWorld screens) is linked; every other, which the game may write
// (its configuration, saves, the NovaWorld cookie jar nw_cdata.coo, its logs), is copied fresh every
// run, never linked and never cached, so no write reaches the install, the build or the cache. Where
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
                           const FileLink &link = link_file);

// The historical Jointops.exe /w /d /FRISK launch in `run_dir`: the game install's game opens its
// archives and, under /d, its loose files from its working directory [orig: PFF_OpenAllArchives @
// 0x4a4310, CWD-relative _lopen; docs/vfs/vfs-pff-mount-re.md] and writes there (game.cfg, its
// saves, its _filelog.txt), so the run directory gets the build's files (one the game only reads
// linked, copied where the file system cannot link it; every other, which the game may write,
// copied), the install's executable and Bink DLL, a game.cfg, the build's own when the project has
// one, else the install's, and the install's player.sav and weapon.sav and the files it reads from
// its folder by name (score.ini, earlyerr.txt, admin.cfg) where the project has none of its own (the
// profile and bindings the player starts with). The required files are checked before any
// is copied, so a missing one launches nothing. The game install and the build directory are only
// read. A build of the expansion `expansion` (ADR 0046 S16) is staged as prepare_expansion_run
// stages it, the install's own game.cfg and saves beside it, and the game launched `/w /d /exp
// <expansion> /FRISK`.
bool prepare_retail_launch_plan(const std::string &retail_directory, const std::string &build_dir,
                                const std::string &run_dir, LaunchPlan &out, Diagnostic &error,
                                const std::string &expansion = std::string(),
                                const std::string &copy_cache = std::string());

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
