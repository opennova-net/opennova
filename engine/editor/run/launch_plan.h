#pragma once

#include <functional>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

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
};

// The packaged runtime (opennova.exe) on `build_dir`, in `run_dir`. `engine_args` are Godot's own
// options for the child (`--headless`, `--windowed`, `--quit-after N`), placed before
// the `--` that starts the game flags.
LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &run_dir, const std::string &game_code, int mcp_port,
                                 const std::string &mission = std::string(),
                                 const std::vector<std::string> &engine_args = {});

// The same run from source: `godot --path <project> res://game/game_runtime_root.tscn -- ...`.
LaunchPlan make_source_launch_plan(const std::string &godot_executable, const std::string &godot_project_dir,
                                   const std::string &build_dir, const std::string &run_dir,
                                   const std::string &game_code, int mcp_port,
                                   const std::string &mission = std::string(),
                                   const std::vector<std::string> &engine_args = {});

// The historical Jointops.exe /w /d /FRISK launch in `run_dir`: the game install's game opens its
// archives and, under /d, its loose files from its working directory [orig: PFF_OpenAllArchives @
// 0x4a4310, CWD-relative _lopen; docs/vfs/vfs-pff-mount-re.md] and writes there (game.cfg, its
// _filelog.txt), so the run directory gets the build's files (its archives linked, or copied where
// the file system cannot link them; its loose files copied, the game may rewrite them), the
// install's executable and Bink DLL, and a game.cfg, the build's own when the project has one,
// else the install's. Every file is checked before any is copied, so a missing one launches
// nothing. The game install and the build directory are only read.
bool prepare_retail_launch_plan(const std::string &retail_directory, const std::string &build_dir,
                                const std::string &run_dir, LaunchPlan &out, Diagnostic &error);

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
