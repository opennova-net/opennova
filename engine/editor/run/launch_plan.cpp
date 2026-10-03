#include <editor/run/launch_plan.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/file_time.h>
#include <base/io/hash.h>
#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <editor/project_build/build_run.h>
#include <editor/run/run_directory.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

void append_game_flags(LaunchPlan &plan, const std::string &game_code, const std::string &mission) {
	plan.args.push_back("--");
	plan.args.push_back(kLaunchFlagResourceDir);
	plan.args.push_back(plan.resource_dir);
	if (!plan.expansion.empty()) {
		plan.args.push_back(kLaunchFlagExpansion);
		plan.args.push_back(plan.expansion);
	}
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

// The runtime on `build_dir` (an expansion's on the run directory, with /exp), working in `run_dir`,
// its log there.
LaunchPlan runtime_plan(const std::string &build_dir, const std::string &run_dir, int mcp_port,
                        const std::string &expansion) {
	LaunchPlan plan;
	plan.build_dir = utf8_of(path_of(build_dir));
	plan.working_dir = utf8_of(path_of(run_dir));
	plan.log_file = join_path(run_dir, kRunLogFileName);
	plan.mcp_port = mcp_port;
	plan.expansion = expansion;
	plan.resource_dir = expansion.empty() ? plan.build_dir : plan.working_dir;
	return plan;
}

// The file at `from` copied to `to`, a name the run directory does not hold yet (it is made
// empty: a copy never writes through a name, which may be a link to the build's file); false with
// the OS reason.
bool copy_to(const fs::path &from, const fs::path &to, std::string &reason) {
	std::error_code ec;
	fs::copy_file(system_path(utf8_of(from)), system_path(utf8_of(to)), ec);
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

// The record of an install's files a copy cache holds (prepare_expansion_run): each file's name
// with the size and last write of the install's file it was copied from.
constexpr const char *kInstallCopyRecordFileName = "install_copy.json";

// A file's last write, as two reads of the same file compare it (0 when it cannot be read).
int64_t last_write_of(const std::string &path) {
	return io::file_modified_ticks(system_path(path));
}

// The install's files a run's copy cache keeps for it, by name: the size and last write each was
// copied at.
struct InstallCopies {
	std::string dir;  // copy_cache/<the install's key>
	io::JsonValue record = io::JsonValue::make_object();
	bool changed = false;
};

// The folder of `copy_cache` an install's copies go in: one per install, named by its path's hash.
std::string install_copy_dir(const std::string &copy_cache, const std::string &install) {
	const std::string key = strutil::to_lower(utf8_of(path_of(install).lexically_normal()));
	return join_path(copy_cache, io::hex64(io::fnv1a64_bytes(io::kFnv1a64Offset, key.data(), key.size())));
}

// The install's file `name` at `to` in the run directory: linked, else (another volume) linked from
// its copy in the copy cache, copied there first when the cache has none of its size and last
// write, else copied. False with the OS reason.
bool stage_install_file(const fs::path &install, const std::string &name, const fs::path &to, InstallCopies &copies,
                        const FileLink &link, std::string &reason) {
	const std::string from = utf8_of(install / path_of(name));
	if (link(from, utf8_of(to), reason)) return true;
	if (copies.dir.empty()) return copy_to(path_of(from), to, reason);
	std::error_code ec;
	const uint64_t size = fs::file_size(system_path(from), ec);
	if (ec) {
		reason = ec.message();
		return false;
	}
	const int64_t written = last_write_of(from);
	const std::string cached = join_path(copies.dir, name);
	const io::JsonValue *was = copies.record.get(strutil::to_lower(name));
	const bool fresh = was && was->is_object() && uint64_t(was->get_number("size", -1.0)) == size &&
	                   was->get_string("modified", "") == io::hex64(uint64_t(written)) &&
	                   fs::file_size(system_path(cached), ec) == size && !ec;
	if (!fresh) {
		// Copied beside its place, then put there: a cache a copy was cut short in holds no torn file.
		const std::string partial = cached + ".tmp";
		fs::remove(system_path(partial), ec);
		if (!ensure_directory(copies.dir, reason) || !copy_to(path_of(from), path_of(partial), reason) ||
		    !replace_file(partial, cached, reason))
			return false;
		io::JsonValue entry = io::JsonValue::make_object();
		entry.set("size", io::JsonValue::make_number(double(size)));
		entry.set("modified", io::JsonValue::make_string(io::hex64(uint64_t(written))));
		copies.record.set(strutil::to_lower(name), std::move(entry));
		copies.changed = true;
	}
	return link(cached, utf8_of(to), reason) || copy_to(path_of(cached), to, reason);
}

// The install's boot archives, as its root spells them (the boot table's names, found without case
// as the game's _lopen finds them on its file system [orig: PFF_OpenAllArchives @ 0x4a4310]).
std::vector<std::string> install_archives(const std::string &install) {
	std::vector<std::string> found;
	std::error_code ec;
	for (const char *slot : kBootArchiveTable) {
		for (const fs::directory_entry &entry : fs::directory_iterator(system_path(install), ec)) {
			std::error_code kind;
			const std::string name = utf8_of(entry.path().filename());
			if (entry.is_regular_file(kind) && strutil::iequals(name, slot)) {
				found.push_back(name);
				break;
			}
		}
	}
	return found;
}

} // namespace

bool prepare_expansion_run(const std::string &install, const std::string &build_dir, const std::string &expansion,
                           const std::string &run_dir, const std::string &copy_cache, Diagnostic &error,
                           const FileLink &link) {
	std::error_code ec;
	if (install.empty() || !fs::is_directory(system_path(install), ec)) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "An expansion plays over the game install: choose its folder in File > Project settings... "
		                     "first.");
		return false;
	}
	const std::vector<std::string> archives = install_archives(install);
	if (archives.empty()) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "The game install " + install + " has none of the game's archives for the expansion to play over.");
		return false;
	}
	const std::string folder = expansion_folder(expansion);
	const fs::path built = path_of(build_dir) / path_of(folder);
	if (!fs::is_directory(system_path(utf8_of(built)), ec)) {
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "The build " + build_dir + " holds no expansion " + expansion + ".");
		return false;
	}
	const fs::path install_root = path_of(install);
	const fs::path run = path_of(run_dir);
	InstallCopies copies;
	if (!copy_cache.empty()) {
		copies.dir = install_copy_dir(copy_cache, install);
		std::string text, message;
		io::JsonValue record;
		if (read_file_text(join_path(copies.dir, kInstallCopyRecordFileName), text, message) &&
		    io::json_parse(text, record, message) && record.is_object())
			copies.record = std::move(record);
	}
	const auto staged = [&](bool ok, const std::string &from, const std::string &reason) {
		if (!ok)
			error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
			                     "Could not stage " + from + " in " + run_dir + ": " + reason);
		return ok;
	};
	// The install's base game: its archives and the loose files it ships beside them.
	std::vector<std::string> base = archives;
	for (const std::string &loose : list_install_loose_files(install)) base.push_back(loose);
	bool ok = true;
	for (const std::string &name : base) {
		std::string reason;
		ok = staged(stage_install_file(install_root, name, run / path_of(name), copies, link, reason),
		            utf8_of(install_root / path_of(name)), reason);
		if (!ok) break;
	}
	if (copies.changed) {
		std::string message;
		write_file_atomic(join_path(copies.dir, kInstallCopyRecordFileName), io::json_write(copies.record), message);
	}
	if (!ok) return false;
	// The expansion's folder, a directory of the run's own (the game writes its weapon.sav there), its
	// files the build's.
	std::string reason;
	const fs::path into = run / path_of(folder);
	if (!staged(ensure_directory(utf8_of(into), reason), utf8_of(built), reason)) return false;
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(utf8_of(built)), ec)) {
		std::error_code kind;
		if (!entry.is_regular_file(kind)) continue;
		const std::string name = utf8_of(entry.path().filename());
		const fs::path from = built / path_of(name);
		const bool linked = !game_may_write(name) && link(utf8_of(from), utf8_of(into / path_of(name)), reason);
		if (!staged(linked || copy_to(from, into / path_of(name), reason), utf8_of(from), reason)) return false;
	}
	return staged(!ec, utf8_of(built), ec ? ec.message() : std::string());
}

LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &run_dir, const std::string &game_code, int mcp_port,
                                 const std::string &mission, const std::vector<std::string> &engine_args,
                                 const std::string &expansion) {
	LaunchPlan plan = runtime_plan(build_dir, run_dir, mcp_port, expansion);
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
                                   const std::vector<std::string> &engine_args, const std::string &expansion) {
	LaunchPlan plan = runtime_plan(build_dir, run_dir, mcp_port, expansion);
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
                                const std::string &run_dir, LaunchPlan &out, Diagnostic &error,
                                const std::string &expansion, const std::string &copy_cache) {
	out = LaunchPlan();
	std::error_code ec;
	if (retail_directory.empty() || !fs::is_directory(system_path(retail_directory), ec)) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "Choose the game install folder in File > Project settings... first.");
		return false;
	}
	const fs::path retail = path_of(retail_directory);
	const fs::path build = path_of(build_dir);
	const fs::path run = path_of(run_dir);
	// The build's files, but its record: a game.cfg among them is the project's own, as a save is. An
	// expansion's build holds its folder alone (a configuration is no file of an expansion:
	// AssetKindRow::expansion_loose), which prepare_expansion_run stages.
	std::vector<std::string> built;
	fs::path config = retail / "game.cfg";
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(build_dir), ec)) {
		std::error_code kind;
		if (!expansion.empty() || !entry.is_regular_file(kind)) continue;
		const std::string name = utf8_of(entry.path().filename());
		if (name == kBuildRecordFileName) continue;
		if (strutil::to_lower(name) == "game.cfg") config = build / path_of(name);
		else built.push_back(name);
	}
	if (ec) {
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not read the build " + utf8_of(build) + ": " + ec.message());
		return false;
	}
	// Same three-file staging as the former GamePacker.stage_retail (4521b859e^), into the run
	// directory. JOTAC's underscored DLL is the real Bink; its plain DLL can be a hook shim.
	fs::path bink = retail / "binkw32_.dll";
	if (!fs::is_regular_file(system_path(utf8_of(bink)), ec)) bink = retail / "binkw32.dll";
	const std::pair<fs::path, const char *> staged[] = {
		{retail / "Jointops.exe", "Jointops.exe"}, {bink, "binkw32.dll"}, {config, "game.cfg"}};
	// Every source checked before anything is copied, so a missing file launches nothing.
	for (const auto &[source, name] : staged) {
		if (fs::is_regular_file(system_path(utf8_of(source)), ec)) continue;
		std::string message = "The game install has no " + utf8_of(source);
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
		if (!own && fs::is_regular_file(system_path(utf8_of(retail / save)), ec)) saves.push_back(save);
	}
	// An expansion's: the install's base game and the build's expansion folder.
	if (!expansion.empty() && !prepare_expansion_run(retail_directory, build_dir, expansion, run_dir, copy_cache, error))
		return false;
	// The build's files beside the game: one the game may write copied, every other linked (the game
	// only reads it), copied where the file system will not link it.
	for (const std::string &name : built) {
		std::string reason;
		const fs::path from = build / path_of(name);
		const fs::path to = run / path_of(name);
		const bool linked = !game_may_write(name) && link_file(utf8_of(from), utf8_of(to), reason);
		if (linked || copy_to(from, to, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + utf8_of(from) + " in " + utf8_of(run) + ": " + reason);
		return false;
	}
	for (const auto &[source, name] : staged) {
		std::string reason;
		if (copy_to(source, run / name, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + utf8_of(source) + ": " + reason);
		return false;
	}
	for (const std::string &save : saves) {
		std::string reason;
		if (copy_to(retail / save, run / save, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + utf8_of(retail / save) + ": " + reason);
		return false;
	}
	out.executable = utf8_of(run / "Jointops.exe");
	out.build_dir = utf8_of(build);
	out.working_dir = utf8_of(run);
	out.resource_dir = out.working_dir; // the game opens what it mounts from its working directory
	out.expansion = expansion;
	out.log_file = utf8_of(run / "_filelog.txt");
	out.args = {"/w", "/d"};
	if (!expansion.empty()) {
		out.args.push_back(kLaunchFlagExpansion);
		out.args.push_back(expansion);
	}
	out.args.push_back("/FRISK");
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
		        utf8_of(path_of(editor_executable).parent_path().parent_path() / "runtime" / "opennova.exe");
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
