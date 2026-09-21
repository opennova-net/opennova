#pragma once

#include <string>
#include <vector>

namespace opennova::editor {

// The argument vector Play spawns (ADR 0046 d8): the game runtime on a build directory,
// packed data only (no `/d`, no `--loose-root`), its log to a file in the build so the
// editor can tail it before the runtime MCP answers, and the MCP endpoint on the port
// the caller allocated. A source run drives the Godot binary at the project instead of a
// packaged executable; the game flags ride after `--` in both forms because the runtime
// reads Godot's own arguments and its user arguments in one pass.
struct LaunchPlan {
	std::string executable;
	std::vector<std::string> args; // without argv[0]
	std::string working_dir;       // the build directory
	std::string log_file;          // <build>/session.log
	int mcp_port = 0;              // 0 = no endpoint
	std::string build_dir;
};

// The packaged runtime (opennova.exe) on `build_dir`. `engine_args` are Godot's own
// options for the child (`--headless`, `--windowed`, `--quit-after N`), placed before
// the `--` that starts the game flags.
LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &game_code, int mcp_port,
                                 const std::string &mission = std::string(),
                                 const std::vector<std::string> &engine_args = {});

// The same run from source: `godot --path <project> res://game/game_runtime_root.tscn -- ...`.
LaunchPlan make_source_launch_plan(const std::string &godot_executable, const std::string &godot_project_dir,
                                   const std::string &build_dir, const std::string &game_code, int mcp_port,
                                   const std::string &mission = std::string(),
                                   const std::vector<std::string> &engine_args = {});

// The plan as one line for a log or the Output window (arguments quoted when needed).
std::string launch_plan_command_line(const LaunchPlan &plan);

} // namespace opennova::editor
