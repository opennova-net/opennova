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
                                 const std::string &game_code, int mcp_port, const std::string &mission) {
	LaunchPlan plan;
	plan.executable = runtime_executable;
	plan.build_dir = fs::path(build_dir).generic_string();
	plan.working_dir = plan.build_dir;
	plan.log_file = (fs::path(build_dir) / "session.log").generic_string();
	plan.mcp_port = mcp_port;
	plan.args.push_back("--log-file");
	plan.args.push_back(plan.log_file);
	append_game_flags(plan, game_code, mission);
	return plan;
}

LaunchPlan make_source_launch_plan(const std::string &godot_executable, const std::string &godot_project_dir,
                                   const std::string &build_dir, const std::string &game_code, int mcp_port,
                                   const std::string &mission) {
	LaunchPlan plan;
	plan.executable = godot_executable;
	plan.build_dir = fs::path(build_dir).generic_string();
	plan.working_dir = plan.build_dir;
	plan.log_file = (fs::path(build_dir) / "session.log").generic_string();
	plan.mcp_port = mcp_port;
	plan.args.push_back("--path");
	plan.args.push_back(godot_project_dir);
	plan.args.push_back("res://game/game_runtime_root.tscn");
	plan.args.push_back("--log-file");
	plan.args.push_back(plan.log_file);
	append_game_flags(plan, game_code, mission);
	return plan;
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
