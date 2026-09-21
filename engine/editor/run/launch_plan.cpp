#include <editor/run/launch_plan.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

void append_game_flags(LaunchPlan &plan, const std::string &game_code, const std::string &mission) {
	plan.args.push_back("--");
	plan.args.push_back("--resource-dir");
	plan.args.push_back(plan.build_dir);
	if (!game_code.empty()) {
		plan.args.push_back("/game");
		plan.args.push_back(game_code);
	}
	if (plan.mcp_port > 0) {
		plan.args.push_back("--mcp-port");
		plan.args.push_back(std::to_string(plan.mcp_port));
	}
	if (!mission.empty()) {
		plan.args.push_back("--mission");
		plan.args.push_back(mission);
	}
}

std::string quote(const std::string &arg) {
	if (arg.find_first_of(" \t\"") == std::string::npos) return arg;
	std::string out = "\"";
	for (const char c : arg) {
		if (c == '"') out += "\\\"";
		else out.push_back(c);
	}
	out += "\"";
	return out;
}

} // namespace

LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &game_code, int mcp_port, const std::string &mission,
                                 const std::vector<std::string> &engine_args) {
	LaunchPlan plan;
	plan.executable = runtime_executable;
	plan.build_dir = fs::path(build_dir).generic_string();
	plan.working_dir = plan.build_dir;
	plan.log_file = (fs::path(build_dir) / "session.log").generic_string();
	plan.mcp_port = mcp_port;
	plan.args = engine_args;
	plan.args.push_back("--log-file");
	plan.args.push_back(plan.log_file);
	append_game_flags(plan, game_code, mission);
	return plan;
}

LaunchPlan make_source_launch_plan(const std::string &godot_executable, const std::string &godot_project_dir,
                                   const std::string &build_dir, const std::string &game_code, int mcp_port,
                                   const std::string &mission, const std::vector<std::string> &engine_args) {
	LaunchPlan plan;
	plan.executable = godot_executable;
	plan.build_dir = fs::path(build_dir).generic_string();
	plan.working_dir = plan.build_dir;
	plan.log_file = (fs::path(build_dir) / "session.log").generic_string();
	plan.mcp_port = mcp_port;
	plan.args.push_back("--path");
	plan.args.push_back(godot_project_dir);
	plan.args.push_back("res://game/game_runtime_root.tscn");
	for (const std::string &arg : engine_args) plan.args.push_back(arg);
	plan.args.push_back("--log-file");
	plan.args.push_back(plan.log_file);
	append_game_flags(plan, game_code, mission);
	return plan;
}

bool prepare_retail_launch_plan(const std::string &retail_directory, const std::string &build_dir,
                                LaunchPlan &out, Diagnostic &error) {
	out = LaunchPlan();
	std::error_code ec;
	if (retail_directory.empty() || !fs::is_directory(retail_directory, ec)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "play.retail_missing",
		                        "Choose a valid retail install folder under Settings.");
		return false;
	}
	const fs::path retail(retail_directory);
	const fs::path build(build_dir);
	fs::path sources[] = {retail / "Jointops.exe", retail / "binkw32_.dll", retail / "game.cfg"};
	const char *names[] = {"Jointops.exe", "binkw32.dll", "game.cfg"};
	const bool has_config = fs::is_regular_file(build / "game.cfg", ec);
	// Same three-file staging as the former GamePacker.stage_retail (4521b859e^).
	// JOTAC's underscored DLL is the real Bink; its plain DLL can be a hook shim.
	if (!fs::is_regular_file(sources[1], ec)) sources[1] = retail / "binkw32.dll";
	for (const fs::path &source : sources) {
		if (source.filename() == "game.cfg" && has_config) continue;
		if (!fs::is_regular_file(source, ec)) {
			std::string message = "Missing retail file: " + source.generic_string();
			if (source.filename() == "game.cfg")
				message += ". Run retail once from its install folder to create game.cfg.";
			error = make_diagnostic(DiagnosticSeverity::Error, "play.retail_missing", message);
			return false;
		}
	}
	// Check every required source before copying, so a missing file cannot launch
	// a stale executable left by an earlier run. Built game data is not rewritten.
	for (size_t i = 0; i < 3; ++i) {
		if (i == 2 && has_config) continue; // keep authored or previously adjusted video settings
		fs::copy_file(sources[i], build / names[i], fs::copy_options::overwrite_existing, ec);
		if (ec) {
			error = make_diagnostic(DiagnosticSeverity::Error, "play.retail_copy",
			                        "Could not stage " + sources[i].generic_string() + ": " + ec.message());
			return false;
		}
	}
	out.executable = (build / "Jointops.exe").generic_string();
	out.build_dir = build.generic_string();
	out.working_dir = out.build_dir;
	out.log_file = (build / "_filelog.txt").generic_string();
	out.args = {"/w", "/d", "/FRISK"};
	return true;
}

std::string launch_plan_command_line(const LaunchPlan &plan) {
	std::string line = quote(plan.executable);
	for (const std::string &arg : plan.args) {
		line.push_back(' ');
		line += quote(arg);
	}
	return line;
}

} // namespace opennova::editor
