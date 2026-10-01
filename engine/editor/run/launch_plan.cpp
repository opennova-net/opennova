#include <editor/run/launch_plan.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/run/run_directory.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

void append_game_flags(LaunchPlan &plan, const std::string &game_code, const std::string &mission) {
	plan.args.push_back("--");
	plan.args.push_back(kLaunchFlagResourceDir);
	plan.args.push_back(plan.build_dir);
	if (!game_code.empty()) {
		plan.args.push_back(kLaunchFlagGame);
		plan.args.push_back(game_code);
	}
	if (plan.mcp_port > 0) {
		plan.args.push_back(kLaunchFlagMcpPort);
		plan.args.push_back(std::to_string(plan.mcp_port));
	}
	if (!mission.empty()) {
		plan.args.push_back(kLaunchFlagMission);
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

// The runtime on `build_dir`, working in `run_dir`, its log there.
LaunchPlan runtime_plan(const std::string &build_dir, const std::string &run_dir, int mcp_port) {
	LaunchPlan plan;
	plan.build_dir = fs::path(build_dir).generic_string();
	plan.working_dir = fs::path(run_dir).generic_string();
	plan.log_file = (fs::path(run_dir) / kRunLogFileName).generic_string();
	plan.mcp_port = mcp_port;
	return plan;
}

// The file at `from` copied to `to`, a name the run directory does not hold yet (it is made
// empty: a copy never writes through a name, which may be a link to the build's file); false with
// the OS reason.
bool copy_to(const fs::path &from, const fs::path &to, std::string &reason) {
	std::error_code ec;
	fs::copy_file(system_path(from.generic_string()), system_path(to.generic_string()), ec);
	if (ec) reason = ec.message();
	return !ec;
}

// The files the game install's game may write beside itself (its configuration, its saves, the
// NovaWorld string tables, its logs): copied into the run directory, so its writes land there and
// never in the build. Every other file of the build (an archive, a video, a music bank, which the
// game only reads) is linked, copied where the file system will not link it (S13 A8).
bool game_may_write(const std::string &name) {
	for (const char *extension : {".cfg", ".sav", ".coo", ".txt"})
		if (strutil::ends_with_icase(name, extension)) return true;
	return false;
}

// The saves the game keeps beside itself, read at boot from its working directory and written
// back there as the player changes them [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0, the
// CWD-relative path build @0x54f68c-0x54f6b7; PlayerProfile_SaveToFiles @ 0x54be00].
constexpr const char *kInstallSaves[] = {"player.sav", "weapon.sav"};

} // namespace

LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &run_dir, const std::string &game_code, int mcp_port,
                                 const std::string &mission, const std::vector<std::string> &engine_args) {
	LaunchPlan plan = runtime_plan(build_dir, run_dir, mcp_port);
	plan.executable = runtime_executable;
	plan.args = engine_args;
	plan.args.push_back("--log-file");
	plan.args.push_back(plan.log_file);
	append_game_flags(plan, game_code, mission);
	return plan;
}

LaunchPlan make_source_launch_plan(const std::string &godot_executable, const std::string &godot_project_dir,
                                   const std::string &build_dir, const std::string &run_dir,
                                   const std::string &game_code, int mcp_port, const std::string &mission,
                                   const std::vector<std::string> &engine_args) {
	LaunchPlan plan = runtime_plan(build_dir, run_dir, mcp_port);
	plan.executable = godot_executable;
	plan.args.push_back("--path");
	plan.args.push_back(godot_project_dir);
	plan.args.push_back("res://game/game_runtime_root.tscn");
	for (const std::string &arg : engine_args) plan.args.push_back(arg);
	plan.args.push_back("--log-file");
	plan.args.push_back(plan.log_file);
	append_game_flags(plan, game_code, mission);
	// Godot's --path moves the process's working directory to the project, so the run directory
	// is named: the runtime keeps the files it writes beside itself there (LaunchFlags.working_dir),
	// as the packaged runtime does in the working directory it is started in.
	plan.args.push_back(kLaunchFlagWorkingDir);
	plan.args.push_back(plan.working_dir);
	return plan;
}

bool prepare_retail_launch_plan(const std::string &retail_directory, const std::string &build_dir,
                                const std::string &run_dir, LaunchPlan &out, Diagnostic &error) {
	out = LaunchPlan();
	std::error_code ec;
	if (retail_directory.empty() || !fs::is_directory(retail_directory, ec)) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "Choose the game install folder in File > Project settings... first.");
		return false;
	}
	const fs::path retail(retail_directory);
	const fs::path build(build_dir);
	const fs::path run(run_dir);
	// The build's files, but its record: a game.cfg among them is the project's own, as a save is.
	std::vector<std::string> built;
	fs::path config = retail / "game.cfg";
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(build_dir), ec)) {
		std::error_code kind;
		if (!entry.is_regular_file(kind)) continue;
		const std::string name = entry.path().filename().string();
		if (name == kBuildRecordFileName) continue;
		if (strutil::to_lower(name) == "game.cfg") config = build / name;
		else built.push_back(name);
	}
	if (ec) {
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not read the build " + build.generic_string() + ": " + ec.message());
		return false;
	}
	// Same three-file staging as the former GamePacker.stage_retail (4521b859e^), into the run
	// directory. JOTAC's underscored DLL is the real Bink; its plain DLL can be a hook shim.
	fs::path bink = retail / "binkw32_.dll";
	if (!fs::is_regular_file(bink, ec)) bink = retail / "binkw32.dll";
	const std::pair<fs::path, const char *> staged[] = {
		{retail / "Jointops.exe", "Jointops.exe"}, {bink, "binkw32.dll"}, {config, "game.cfg"}};
	// Every source checked before anything is copied, so a missing file launches nothing.
	for (const auto &[source, name] : staged) {
		if (fs::is_regular_file(system_path(source.generic_string()), ec)) continue;
		std::string message = "The game install has no " + source.generic_string();
		if (source == retail / "game.cfg") message += ". Run the game once from its install folder to create game.cfg.";
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error, message);
		return false;
	}
	// The install's saves where the project has none of its own, as its game.cfg is: the player's
	// profile and bindings the game starts with (one the game rewrites in the run stays there, as a
	// game.cfg does). A save the install lacks is the game's to make.
	std::vector<std::string> saves;
	for (const char *save : kInstallSaves) {
		const bool own = std::any_of(built.begin(), built.end(),
		                             [save](const std::string &name) { return strutil::to_lower(name) == save; });
		if (!own && fs::is_regular_file(system_path((retail / save).generic_string()), ec)) saves.push_back(save);
	}
	// The build's files beside the game: one the game may write copied, every other linked (the game
	// only reads it), copied where the file system will not link it.
	for (const std::string &name : built) {
		std::string reason;
		const bool linked = !game_may_write(name) &&
		                    link_file((build / name).generic_string(), (run / name).generic_string(), reason);
		if (linked || copy_to(build / name, run / name, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + (build / name).generic_string() + " in " + run.generic_string() + ": " + reason);
		return false;
	}
	for (const auto &[source, name] : staged) {
		std::string reason;
		if (copy_to(source, run / name, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + source.generic_string() + ": " + reason);
		return false;
	}
	for (const std::string &save : saves) {
		std::string reason;
		if (copy_to(retail / save, run / save, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + (retail / save).generic_string() + ": " + reason);
		return false;
	}
	out.executable = (run / "Jointops.exe").generic_string();
	out.build_dir = build.generic_string();
	out.working_dir = run.generic_string();
	out.log_file = (run / "_filelog.txt").generic_string();
	out.args = {"/w", "/d", "/FRISK"};
	return true;
}

PlayLauncher make_play_launcher(bool source_run, const std::string &editor_executable,
                                const std::string &godot_project_dir, int mcp_port,
                                std::vector<std::string> engine_args) {
	PlayLauncher launcher;
	launcher.source_run = source_run;
	launcher.mcp_port = mcp_port;
	launcher.engine_args = std::move(engine_args);
	if (source_run) {
		launcher.executable = editor_executable;
		launcher.godot_project_dir = godot_project_dir;
	} else {
		launcher.executable =
		        (fs::path(editor_executable).parent_path().parent_path() / "runtime" / "opennova.exe").generic_string();
	}
	return launcher;
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
