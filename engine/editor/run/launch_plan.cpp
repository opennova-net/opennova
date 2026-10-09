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
#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <formats/filelog/file_access_log.h>
#include <formats/pff/pff.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <editor/project_build/build_run.h>
#include <editor/project_build/export_build.h>
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

// The runtime on `build_dir` (an expansion's on the run directory, with /exp, as a build staged there:
// `on_run_dir`), working in `run_dir`, its log there.
LaunchPlan runtime_plan(const std::string &build_dir, const std::string &run_dir, int mcp_port,
                        const std::string &expansion, bool on_run_dir) {
	LaunchPlan plan;
	plan.build_dir = utf8_of(path_of(build_dir));
	plan.working_dir = utf8_of(path_of(run_dir));
	plan.log_file = join_path(run_dir, kRunLogFileName);
	plan.mcp_port = mcp_port;
	plan.expansion = expansion;
	plan.resource_dir = expansion.empty() && !on_run_dir ? plan.build_dir : plan.working_dir;
	return plan;
}

// The file at `from` copied to `to`, a name the run directory does not hold (free_name made it free, or
// it never held it: a copy never writes through a name, which may be a link to the build's file); false
// with the OS reason.
bool copy_to(const fs::path &from, const fs::path &to, std::string &reason) {
	std::error_code ec;
	fs::copy_file(system_path(utf8_of(from)), system_path(utf8_of(to)), ec);
	if (ec) reason = ec.message();
	return !ec;
}

// The name `to` in the run directory made free for a file the staging puts there: a file the run kept
// under it (one the game wrote, or a seed) removed by its name, never written through (a link names the
// build's or the install's file). False with the reason when it will not go.
bool free_name(const fs::path &to, std::string &reason) {
	const fs::path name = system_path(utf8_of(to));
	std::error_code ec;
	const fs::file_status status = fs::symlink_status(name, ec);
	if (!fs::exists(status)) return true;
	if (fs::is_directory(status)) {
		reason = "a folder of the run directory has its name";
		return false;
	}
	fs::remove(name, ec);
	if (ec) reason = ec.message();
	return !ec;
}

// The file `relative` (under the run directory) added to what the staging put there (LaunchPlan::staged).
void add_staged(std::vector<std::string> *staged, const fs::path &relative) {
	if (staged) staged->push_back(utf8_of(relative));
}

// The files the game only reads: its archives, its videos, its music banks, its dialog banks and the
// NovaWorld screens, which it opens through the archives' and the streams' readers alone. Only these
// are linked into a run directory (copied where the file system will not link them, S13 A8). Every
// other file is one the game may write beside itself, and is copied fresh into the run directory
// every run, never linked and never kept in a cache: a write through a link lands in the file it
// names, the install's, the build's or the cache's. The game writes its configuration [orig:
// Game_SaveConfig @ 0x54c4af, "game.cfg" "w"], its saves [orig: PlayerProfile_SaveToFiles @ 0x54be00],
// the NovaWorld cookie jar over the install's own file [orig: sub_63BA60 @ 0x63ba9a "wb", from
// Menu_TeardownShellAndCloseBinkVideos @ 0x54e4aa with "nw_cdata.coo"], `score.ini` when it has none
// [orig: GameType_CreateDefaultSettings @ 0x52f528 -> ScoreConfig_SaveFile @ 0x52cdd0], and its logs
// and lists [orig: ErrorLog_WriteTimestamped @ 0x53c6e7 "_errlog.txt" "a"; BanList_SaveToFile
// @ 0x4fdda3; HighScore_SaveToFile @ 0x56310b; CAdminServer_Construct @ 0x402c84 "admin_log.txt"].
bool game_only_reads(const std::string &name) {
	for (const char *extension : {".pff", ".bik", ".sbf", ".lwf", ".mnx"})
		if (strutil::ends_with_icase(name, extension)) return true;
	return false;
}

bool game_may_write(const std::string &name) {
	return !game_only_reads(name);
}

// The install's own files beside the game that it reads by name from its folder whatever the
// expansion, staged (copied: game_may_write) where the build has none of its own, as a player's
// install holds them: the score table [orig: ScoreConfig_LoadFile @ 0x52d8a0, @ 0x52da99], the early
// error text [orig: Game_ShowEarlyError @ 0x4a68c0] and the admin server's configuration [orig:
// Game_InitSubsystems @ 0x4a72b8 -> CAdminServer_LoadConfig @ 0x406d80].
constexpr const char *kInstallRootReads[] = {"score.ini", "earlyerr.txt", "admin.cfg"};

// The file `name` of a folder (the install's, the run directory's), as the folder spells it (found without
// case, as the game's file calls find it); "" when the folder has none.
std::string file_named(const std::string &folder, const char *name) {
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(folder), ec)) {
		std::error_code kind;
		const std::string found = utf8_of(entry.path().filename());
		if (entry.is_regular_file(kind) && strutil::iequals(found, name)) return found;
	}
	return std::string();
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
// copied at. A copy is trusted by those alone only when the install file's last write had settled when
// it was made (io::file_stamp_settled, git's racy rule: a rewrite of the same size in the same clock tick
// keeps the stamp); one that had not is copied again the next run. The cache keeps what a run used, and
// the current install's alone: every other copy, and every other install's folder, is removed.
struct InstallCopies {
	std::string dir;  // copy_cache/<the install's key>
	io::JsonValue record = io::JsonValue::make_object();
	bool changed = false;
	int64_t pass_began = io::file_clock_now_ticks();
	std::vector<std::string> used; // the names (lowercase) a run linked from the cache
};

// The folder of `copy_cache` an install's copies go in: one per install, named by its path's hash.
std::string install_copy_dir(const std::string &copy_cache, const std::string &install) {
	const std::string key = strutil::to_lower(utf8_of(path_of(install).lexically_normal()));
	return join_path(copy_cache, io::hex64(io::fnv1a64_bytes(io::kFnv1a64Offset, key.data(), key.size())));
}

// The install's file `name` at `to` in the run directory: one the game may write copied from the
// install, fresh, never linked or cached; one it only reads linked, else (another volume) linked
// from its copy in the copy cache, copied there first when the cache has none of its size and last
// write, else copied; the name the run directory held freed first (free_name). False with the OS reason.
bool stage_install_file(const fs::path &install, const std::string &name, const fs::path &to, InstallCopies &copies,
                        const FileLink &link, std::string &reason) {
	const std::string from = utf8_of(install / path_of(name));
	if (!free_name(to, reason)) return false;
	if (game_may_write(name)) return copy_to(path_of(from), to, reason);
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
		if (!io::ensure_directory(copies.dir, reason) || !copy_to(path_of(from), path_of(partial), reason) ||
		    !io::replace_file(partial, cached, reason))
			return false;
		io::JsonValue entry = io::JsonValue::make_object();
		entry.set("size", io::JsonValue::make_number(double(size)));
		entry.set("modified", io::JsonValue::make_string(io::hex64(uint64_t(written))));
		if (io::file_stamp_settled(written, copies.pass_began)) copies.record.set(strutil::to_lower(name), std::move(entry));
		else copies.record.set(strutil::to_lower(name), io::JsonValue::make_null()); // not trusted next run
		copies.changed = true;
	}
	copies.used.push_back(strutil::to_lower(name));
	return link(cached, utf8_of(to), reason) || copy_to(path_of(cached), to, reason);
}

// The copy cache pruned to what this run used of the current install (InstallCopies).
void prune_install_copies(const std::string &copy_cache, InstallCopies &copies) {
	std::error_code ec;
	const fs::path current = path_of(copies.dir).filename();
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(copy_cache), ec)) {
		std::error_code removed;
		if (entry.path().filename() != current) fs::remove_all(entry.path(), removed);
	}
	io::JsonValue kept = io::JsonValue::make_object();
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(copies.dir), ec)) {
		const std::string name = strutil::to_lower(utf8_of(entry.path().filename()));
		if (name == kInstallCopyRecordFileName) continue;
		if (std::find(copies.used.begin(), copies.used.end(), name) != copies.used.end()) {
			const io::JsonValue *was = copies.record.get(name);
			if (was && was->is_object()) kept.set(name, *was);
			continue;
		}
		std::error_code removed;
		fs::remove(entry.path(), removed);
		copies.changed = true;
	}
	if (copies.record.object.size() != kept.object.size()) copies.changed = true;
	copies.record = std::move(kept);
}

// The install's Bink DLL. JOTAC's underscored DLL is the real Bink; its plain DLL can be a hook shim
// (the former GamePacker.stage_retail, 4521b859e^).
fs::path install_bink(const fs::path &install) {
	std::error_code ec;
	const fs::path underscored = install / "binkw32_.dll";
	return fs::is_regular_file(system_path(utf8_of(underscored)), ec) ? underscored : install / "binkw32.dll";
}

// The build's files `names` beside the game in `run`, each in place of a file the run kept under its name
// (the project's own wins: the build is what the modder made): one the game may write copied, every other
// linked (the game only reads it), copied where the file system will not link it; each added to `staged`.
// False with `error`.
bool stage_build_files(const fs::path &build, const fs::path &run, const std::vector<std::string> &names,
                       const FileLink &link, Diagnostic &error, std::vector<std::string> *staged) {
	for (const std::string &name : names) {
		std::string reason;
		const fs::path from = build / path_of(name);
		const fs::path to = run / path_of(name);
		add_staged(staged, path_of(name));
		if (!free_name(to, reason)) {
			error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
			                     "Could not stage " + utf8_of(from) + " in " + utf8_of(run) + ": " + reason);
			return false;
		}
		const bool linked = !game_may_write(name) && link(utf8_of(from), utf8_of(to), reason);
		if (linked || copy_to(from, to, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + utf8_of(from) + " in " + utf8_of(run) + ": " + reason);
		return false;
	}
	return true;
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

// The build's expansion folder `folder` (expansion/<b>) staged in `run` as a directory of the run's own,
// never a link to the folder: the game writes its expansion's weapon.sav there [orig:
// PlayerProfile_LoadAllFromDisk @ 0x54f4d0, @ 0x54f6c7], which the run keeps. Each of the build's files in
// place of what the run kept under its name, one the game only reads linked, every other copied; each added
// to `staged`. False with `error` (play.install_copy).
bool stage_expansion_folder(const fs::path &build, const fs::path &run, const std::string &folder,
                            const FileLink &link, Diagnostic &error, std::vector<std::string> *staged_files) {
	std::error_code ec;
	const fs::path built = build / path_of(folder);
	const auto staged = [&](bool ok, const std::string &from, const std::string &reason) {
		if (!ok)
			error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
			                     "Could not stage " + from + " in " + utf8_of(run) + ": " + reason);
		return ok;
	};
	std::string reason;
	const fs::path into = run / path_of(folder);
	if (!staged(io::ensure_directory(utf8_of(into), reason), utf8_of(built), reason)) return false;
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(utf8_of(built)), ec)) {
		std::error_code kind;
		if (!entry.is_regular_file(kind)) continue;
		const std::string name = utf8_of(entry.path().filename());
		const fs::path from = built / path_of(name);
		add_staged(staged_files, path_of(folder) / path_of(name));
		if (!staged(free_name(into / path_of(name), reason), utf8_of(from), reason)) return false;
		const bool linked = !game_may_write(name) && link(utf8_of(from), utf8_of(into / path_of(name)), reason);
		if (!staged(linked || copy_to(from, into / path_of(name), reason), utf8_of(from), reason)) return false;
	}
	return staged(!ec, utf8_of(built), ec ? ec.message() : std::string());
}

} // namespace

bool prepare_expansion_run(const std::string &install, const std::string &build_dir, const std::string &expansion,
                           const std::string &run_dir, const std::string &copy_cache, Diagnostic &error,
                           const FileLink &link, std::vector<std::string> *staged_files) {
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
		if (io::read_file_text(join_path(copies.dir, kInstallCopyRecordFileName), text, message) &&
		    io::json_parse(text, record, message) && record.is_object())
			copies.record = std::move(record);
	}
	const auto staged = [&](bool ok, const std::string &from, const std::string &reason) {
		if (!ok)
			error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
			                     "Could not stage " + from + " in " + run_dir + ": " + reason);
		return ok;
	};
	// The names the expansion's archives hold: under /d (the stock game's Play) the file system's front
	// door reads the expansion's folder, then the working directory, then the archives [orig:
	// FileSystem_OpenFile @ 0x75b1c0, the loose walk @ 0x75b203..0x75b27b before the archives, run
	// when searchLooseFirst is set; /d sets it @ 0x4a6fac], so an install's root copy of a file the
	// expansion packs (a NovaWorld screen it edits) would stand over the packed edit a player's launch,
	// without /d, reads. Such a root copy is left out of the run directory.
	std::vector<std::string> packed;
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(utf8_of(built)), ec)) {
		std::error_code kind;
		if (!entry.is_regular_file(kind) || !strutil::ends_with_icase(utf8_of(entry.path().filename()), ".pff")) continue;
		pff::PffArchive archive{};
		if (pff::pff_open(&archive, utf8_of(entry.path()).c_str()) != 0) continue;
		for (uint32_t i = 0; i < archive.entry_count; ++i) {
			char name[32];
			pff::pff_norm_name(archive.entries[i].filename, sizeof(archive.entries[i].filename), name, sizeof(name));
			packed.push_back(name);
		}
		pff::pff_close(&archive);
	}
	const auto packs = [&packed](const std::string &name) {
		const std::string wanted = normalized_logical_name(name);
		return std::any_of(packed.begin(), packed.end(),
		                   [&wanted](const std::string &held) { return normalized_logical_name(held) == wanted; });
	};
	// The install's base game: its archives and the loose files it ships beside them, but a file the
	// expansion packs, staged; and the files it reads from its folder by name (kInstallRootReads), copied
	// where the run directory holds none of its own (seeds: the copy a run before kept stays).
	std::vector<std::string> base = archives;
	for (const std::string &loose : list_install_loose_files(install))
		if (!packs(loose)) base.push_back(loose);
	std::vector<std::string> seeds;
	for (const char *read : kInstallRootReads) {
		const std::string found = file_named(install, read);
		if (!found.empty() && file_named(run_dir, read).empty()) seeds.push_back(found);
	}
	bool ok = true;
	for (const std::string &name : base) {
		std::string reason;
		add_staged(staged_files, path_of(name));
		ok = staged(stage_install_file(install_root, name, run / path_of(name), copies, link, reason),
		            utf8_of(install_root / path_of(name)), reason);
		if (!ok) break;
	}
	for (const std::string &name : seeds) {
		if (!ok) break;
		std::string reason;
		ok = staged(copy_to(install_root / path_of(name), run / path_of(name), reason), utf8_of(install_root / path_of(name)),
		            reason);
	}
	if (ok && !copy_cache.empty()) prune_install_copies(copy_cache, copies);
	if (copies.changed) {
		std::string message;
		io::write_file_atomic(join_path(copies.dir, kInstallCopyRecordFileName), io::json_write(copies.record), message);
	}
	if (!ok) return false;
	// The expansion's folder, a directory of the run's own (the game writes its weapon.sav there, which
	// the run keeps), its files the build's.
	return stage_expansion_folder(path_of(build_dir), run, folder, link, error, staged_files);
}

LaunchPlan make_play_launch_plan(const std::string &runtime_executable, const std::string &build_dir,
                                 const std::string &run_dir, const std::string &game_code, int mcp_port,
                                 const std::string &mission, const std::vector<std::string> &engine_args,
                                 const std::string &expansion, bool on_run_dir) {
	LaunchPlan plan = runtime_plan(build_dir, run_dir, mcp_port, expansion, on_run_dir);
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
                                   const std::vector<std::string> &engine_args, const std::string &expansion,
                                   bool on_run_dir) {
	LaunchPlan plan = runtime_plan(build_dir, run_dir, mcp_port, expansion, on_run_dir);
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

bool prepare_runtime_run(const std::string &build_dir, const std::string &run_dir, Diagnostic &error,
                         std::vector<std::string> *staged, const FileLink &link) {
	std::error_code ec;
	std::vector<std::string> built;
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(build_dir), ec)) {
		std::error_code kind;
		const std::string name = utf8_of(entry.path().filename());
		if (entry.is_regular_file(kind) && name != kBuildRecordFileName) built.push_back(name);
	}
	if (ec) {
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not read the build " + build_dir + ": " + ec.message());
		return false;
	}
	return stage_build_files(path_of(build_dir), path_of(run_dir), built, link, error, staged);
}

bool prepare_retail_launch_plan(const std::string &retail_directory, const std::string &build_dir,
                                const std::string &run_dir, LaunchPlan &out, Diagnostic &error,
                                const std::string &expansion, const std::string &copy_cache, const FileLink &link,
                                const std::string &base_game) {
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
	fs::path config; // the build's own game.cfg ("" for none)
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
	// The run directory's own copy of a file the game writes, kept from a run before (run/run_directory.h).
	const auto run_holds = [&run_dir](const char *name) { return !file_named(run_dir, name).empty(); };
	// The install's executable and Bink DLL, and the build's game.cfg, staged; the install's game.cfg seeded
	// where neither the build nor the run directory has one. The same three files as the former
	// GamePacker.stage_retail (4521b859e^), into the run directory.
	std::vector<std::pair<fs::path, const char *>> staged = {{retail / kInstallExecutable, kInstallExecutable},
	                                                         {install_bink(retail), "binkw32.dll"}};
	if (!config.empty()) staged.emplace_back(config, "game.cfg");
	// The install's game.cfg and saves (the player's profile and bindings the game starts with) and, beside
	// a standalone build's game, the files it reads by name, as a player's install holds them (an
	// expansion's run seeds those with its base game), where neither the project nor the run directory has
	// its own: seeds, which the run keeps as the game rewrites them. A save the install lacks is the game's
	// to make.
	std::vector<std::string> seeds;
	const bool seed_config = config.empty() && !run_holds("game.cfg");
	if (seed_config) seeds.push_back("game.cfg");
	const auto seed_unless_own = [&](const char *name) {
		const bool own = std::any_of(built.begin(), built.end(),
		                             [name](const std::string &held) { return strutil::iequals(held, name); });
		const std::string found = file_named(retail_directory, name);
		if (!own && !found.empty() && !run_holds(name)) seeds.push_back(found);
	};
	for (const char *save : kInstallSaves) seed_unless_own(save);
	if (expansion.empty())
		for (const char *read : kInstallRootReads) seed_unless_own(read);
	// Every source checked before anything is copied, so a missing file launches nothing.
	for (const auto &[source, name] : staged) {
		if (fs::is_regular_file(system_path(utf8_of(source)), ec)) continue;
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "The game install has no " + utf8_of(source));
		return false;
	}
	if (seed_config && !fs::is_regular_file(system_path(utf8_of(retail / "game.cfg")), ec)) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "The game install has no " + utf8_of(retail / "game.cfg") +
		                             ". Run the game once from its install folder to create game.cfg.");
		return false;
	}
	// An expansion's: the install's base game and the build's expansion folder.
	if (!expansion.empty() && !prepare_expansion_run(base_game.empty() ? retail_directory : base_game, build_dir, expansion,
	                                                 run_dir, copy_cache, error, link, &out.staged))
		return false;
	// The build's files beside the game.
	if (!stage_build_files(build, run, built, link, error, &out.staged)) return false;
	for (const auto &[source, name] : staged) {
		std::string reason;
		add_staged(&out.staged, path_of(name));
		if (free_name(run / name, reason) && copy_to(source, run / name, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + utf8_of(source) + ": " + reason);
		return false;
	}
	for (const std::string &seed : seeds) {
		std::string reason;
		if (copy_to(retail / path_of(seed), run / path_of(seed), reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + utf8_of(retail / path_of(seed)) + ": " + reason);
		return false;
	}
	out.executable = utf8_of(run / kInstallExecutable);
	out.build_dir = utf8_of(build);
	out.working_dir = utf8_of(run);
	out.resource_dir = out.working_dir; // the game opens what it mounts from its working directory
	out.expansion = expansion;
	out.log_file = utf8_of(run / filelog::kInstallFileLogName);
	out.args = {"/w", "/d"};
	if (!expansion.empty()) {
		out.args.push_back(kLaunchFlagExpansion);
		out.args.push_back(expansion);
	}
	out.args.push_back("/FRISK");
	return true;
}

Diagnostic strict_expansion_refusal(const std::string &expansion) {
	return make_finding(CoreFinding::PlayStrictExpansion, DiagnosticSeverity::Error,
	                    "Strict Play of an expansion needs its base game's build: the project builds as the expansion " +
	                            expansion + " on the game install's base game, whose own archives it would play over. "
	                            "Name the base game's project in File > Project settings... (ADR 0046 T5), or turn "
	                            "Strict off to play it over the game install.");
}

bool prepare_strict_install_launch_plan(const std::string &install, const std::string &build_dir,
                                        const std::string &run_dir, const std::string &expansion, LaunchPlan &out,
                                        Diagnostic &error, const FileLink &link, const std::string &base_game) {
	out = LaunchPlan();
	if (!expansion.empty() && (base_game.empty() || base_game == install)) {
		error = strict_expansion_refusal(expansion);
		return false;
	}
	std::error_code base_ec;
	if (!expansion.empty() && !fs::is_directory(system_path(base_game), base_ec)) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "The expansion " + expansion + " plays over its base game's export, and there is none at " +
		                             base_game + ": export the base game's project first.");
		return false;
	}
	std::error_code ec;
	if (install.empty() || !fs::is_directory(system_path(install), ec)) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "Choose the game install folder in File > Project settings... first.");
		return false;
	}
	const fs::path retail = path_of(install);
	const fs::path build = path_of(build_dir);
	const fs::path run = path_of(run_dir);
	// The install's program and its Bink DLL, the game's one import beside the system's, alone of the
	// install; both checked before anything is staged.
	const std::pair<fs::path, const char *> staged[] = {
		{retail / kInstallExecutable, kInstallExecutable}, {install_bink(retail), "binkw32.dll"}};
	for (const auto &[source, name] : staged) {
		if (fs::is_regular_file(system_path(utf8_of(source)), ec)) continue;
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "The game install has no " + utf8_of(source));
		return false;
	}
	// Every file of the build but its record (a game.cfg among them is the project's own). An expansion's
	// build holds its folder alone (expansion/<b>/), which plays over its base game's export (T5): every file
	// of that folder but the export's and the build's records, the base game as a player's folder holds it,
	// then the expansion's folder beside it.
	const fs::path files_from = expansion.empty() ? build : path_of(base_game);
	std::vector<std::string> built;
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(utf8_of(files_from)), ec)) {
		std::error_code kind;
		const std::string name = utf8_of(entry.path().filename());
		if (entry.is_regular_file(kind) && name != kBuildRecordFileName && name != kExportRecordFileName)
			built.push_back(name);
	}
	if (ec) {
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not read " + utf8_of(files_from) + ": " + ec.message());
		return false;
	}
	if (!expansion.empty() && install_archives(base_game).empty()) {
		error = make_finding(CoreFinding::PlayInstallMissing, DiagnosticSeverity::Error,
		                     "The base game's export " + base_game + " has none of the game's archives for the expansion " +
		                             expansion + " to play over: export the base game's project first.");
		return false;
	}
	const std::string folder = expansion.empty() ? std::string() : expansion_folder(expansion);
	if (!expansion.empty() && !fs::is_directory(system_path(utf8_of(build / path_of(folder))), ec)) {
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "The build " + build_dir + " holds no expansion " + expansion + ".");
		return false;
	}
	// Both staged in place of what the run kept under their names; nothing of the install seeded: the game
	// writes its own configuration and saves, which the run directory keeps for the next strict Play.
	if (!stage_build_files(files_from, run, built, link, error, &out.staged)) return false;
	if (!expansion.empty() && !stage_expansion_folder(build, run, folder, link, error, &out.staged)) return false;
	for (const auto &[source, name] : staged) {
		std::string reason;
		add_staged(&out.staged, path_of(name));
		if (free_name(run / name, reason) && copy_to(source, run / name, reason)) continue;
		error = make_finding(CoreFinding::PlayInstallCopy, DiagnosticSeverity::Error,
		                     "Could not stage " + utf8_of(source) + ": " + reason);
		return false;
	}
	out.executable = utf8_of(run / kInstallExecutable);
	out.build_dir = utf8_of(build);
	out.working_dir = utf8_of(run);
	out.resource_dir = out.working_dir;
	out.log_file = utf8_of(run / filelog::kInstallFileLogName);
	out.expansion = expansion;
	out.args = {"/w"};
	if (!expansion.empty()) {
		out.args.push_back(kLaunchFlagExpansion);
		out.args.push_back(expansion);
	}
	out.args.push_back("/FRISK");
	return true;
}

bool strict_first_run_starts_again(bool had_config, bool has_config, bool exited_on_its_own, int64_t exit_code,
                                   int64_t ran_ms, bool started_again) {
	return !started_again && !had_config && has_config && exited_on_its_own && exit_code == 0 && ran_ms >= 0 &&
	       ran_ms <= kStrictFirstRunWindowMs;
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
