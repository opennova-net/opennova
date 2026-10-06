#include <editor/run/run_directory.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <system_error>
#include <utility>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/project/project_files.h>
#include <editor/run/launch_plan.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A Play mode's directory under the run root names it (kRunMode*): one of the three, never a path.
bool run_mode_known(const std::string &mode) {
	return mode == kRunModeRuntime || mode == kRunModeInstall || mode == kRunModeStrict;
}

// A run directory's name: a number from 1, written without leading zeros (a few digits: a mode's
// directory holds the runs whose games may still run, and one more).
bool run_number(const std::string &name, unsigned long &out) {
	if (name.empty() || name.size() > 6 || name[0] == '0' ||
	    !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
		return false;
	const auto parsed = strutil::parse_ulong(name);
	if (!parsed) return false;
	out = *parsed;
	return true;
}

// Whether the game a run directory records may still run: false for no record (a Play that never
// started its game, or one whose game stopped) and for a game the platform says is gone.
bool held(const fs::path &dir, const LeaseLiveness &liveness) {
	std::string text, error;
	if (!read_file_text(utf8_of(dir / kRunRecordFileName), text, error)) return false;
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object() ||
	    json.get_int("schema_version", -1) != kRunRecordSchemaVersion)
		return false;
	const double pid = json.get_number("pid", -1.0);
	if (pid < 0.0 || pid != std::floor(pid)) return false;
	const ProcessLiveness state =
	        liveness ? liveness(static_cast<int64_t>(pid), json.get_string("created", "")) : ProcessLiveness::Unknown;
	return state != ProcessLiveness::Dead;
}

// A staging record's path under its run directory: relative, inside it ("" for any other, which a record
// never names: a record edited by hand removes nothing outside the directory).
fs::path staged_path(const std::string &name) {
	if (name.empty()) return fs::path();
	const fs::path path = path_of(name).lexically_normal();
	if (path.empty() || path.has_root_name() || path.has_root_directory()) return fs::path();
	for (const fs::path &part : path)
		if (part == ".." || part == ".") return fs::path();
	return path;
}

// The staging record `dir` holds; false for none, one that cannot be read, or another schema's.
bool read_run_staging(const fs::path &dir, RunStaging &out) {
	std::string text, error;
	if (!read_file_text(utf8_of(dir / kRunStagingFileName), text, error)) return false;
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object() ||
	    json.get_int("schema_version", -1) != kRunStagingSchemaVersion)
		return false;
	const io::JsonValue *files = json.get("files");
	if (!files || !files->is_array()) return false;
	out.mode = json.get_string("mode", "");
	out.files.clear();
	for (const io::JsonValue &file : files->array)
		if (file.is_string()) out.files.push_back(file.string);
	return !out.mode.empty();
}

// The files under `dir`, '/'-separated paths under it, sorted (directories walked, not listed).
std::vector<std::string> files_under(const fs::path &dir) {
	std::vector<std::string> files;
	const fs::path root = system_path(utf8_of(dir));
	std::error_code ec;
	for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
	     !ec && it != end; it.increment(ec)) {
		std::error_code kind;
		if (!it->is_directory(kind)) files.push_back(utf8_of(it->path().lexically_relative(root)));
	}
	std::sort(files.begin(), files.end());
	return files;
}

// `dir`, a directory of `take`'s mode, readied for a Play of it: what the Play before staged there
// removed, with the logs a run writes for Play to read and the game's record, every other file kept
// (`kept`); emptied for a fresh take or one with no staging record of its mode (a guard: the directory
// is the mode's own, so the record names it unless edited by hand). False when a file that must go
// would not (a process holding it).
bool ready_run_directory(const fs::path &dir, const RunTake &take, std::vector<std::string> &kept) {
	kept.clear();
	const fs::path system = system_path(utf8_of(dir));
	std::error_code ec;
	if (!fs::exists(system, ec)) return true;
	RunStaging before;
	if (take.fresh || !read_run_staging(dir, before) || before.mode != take.mode) {
		fs::remove_all(system, ec);
		return !fs::exists(system, ec);
	}
	std::vector<std::string> going = before.files;
	for (const char *own : {kRunStagingFileName, kRunRecordFileName, kRunLogFileName, kInstallFileLogName})
		going.push_back(own);
	bool gone = true;
	for (const std::string &name : going) {
		const fs::path path = staged_path(name);
		if (path.empty()) continue;
		// The name removed, never written through: a staged file may be a link to the build's or the
		// install's.
		const fs::path file = system_path(utf8_of(dir / path));
		std::error_code removed;
		const fs::file_status status = fs::symlink_status(file, removed);
		if (!fs::exists(status) || fs::is_directory(status)) continue;
		fs::remove(file, removed);
		if (fs::exists(fs::symlink_status(file, removed))) gone = false;
	}
	if (gone) kept = files_under(dir);
	return gone;
}

} // namespace

bool take_run_directory(const std::string &runs_root, const LeaseLiveness &liveness, const RunTake &take,
                        std::string &out, std::vector<std::string> &kept, std::string &error) {
	kept.clear();
	if (!run_mode_known(take.mode)) {
		error = "no Play mode is named \"" + take.mode + "\"";
		return false;
	}
	// The mode's own directory: what a Play takes, empties and removes is its mode's alone.
	const std::string mode_root = join_path(runs_root, take.mode);
	if (!ensure_directory(mode_root, error)) return false;
	// The numbered directories there, each with whether its game may still run.
	std::map<unsigned long, bool> runs;
	std::error_code ec;
	for (const fs::directory_entry &entry :
	     fs::directory_iterator(system_path(mode_root), fs::directory_options::skip_permission_denied, ec)) {
		unsigned long number = 0;
		std::error_code kind;
		if (!entry.is_directory(kind) || !run_number(utf8_of(entry.path().filename()), number)) continue;
		runs[number] = held(entry.path(), liveness);
	}
	// The first one free, readied (one whose files will not go, a process holding them, is passed over
	// too); every other one of the mode whose game is gone removed.
	unsigned long taken = 0;
	for (unsigned long number = 1; taken == 0; ++number) {
		const auto found = runs.find(number);
		if (found != runs.end() && found->second) continue;
		if (!ready_run_directory(path_of(join_path(mode_root, std::to_string(number))), take, kept)) continue;
		taken = number;
	}
	for (const auto &[number, busy] : runs) {
		if (busy || number == taken) continue;
		std::error_code removed;
		fs::remove_all(system_path(join_path(mode_root, std::to_string(number))), removed);
	}
	out = join_path(mode_root, std::to_string(taken));
	return ensure_directory(out, error);
}

bool record_run_staging(const std::string &dir, const RunStaging &staging, std::string &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kRunStagingSchemaVersion));
	json.set("mode", io::JsonValue::make_string(staging.mode));
	io::JsonValue files = io::JsonValue::make_array();
	for (const std::string &file : staging.files) files.push(io::JsonValue::make_string(file));
	json.set("files", std::move(files));
	return write_file_atomic(join_path(dir, kRunStagingFileName), io::json_write(json), error);
}

bool claim_run_directory(const std::string &dir, int64_t pid, const ProcessIdentity &identity, std::string &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kRunRecordSchemaVersion));
	json.set("pid", io::JsonValue::make_number(double(pid)));
	json.set("image", io::JsonValue::make_string(identity.image));
	// A string, as a lease writes it: a creation time passes a JSON number's exact range.
	json.set("created", io::JsonValue::make_string(identity.created));
	return write_file_atomic(join_path(dir, kRunRecordFileName), io::json_write(json), error);
}

void release_run_directory(const std::string &dir) {
	if (dir.empty()) return;
	std::error_code ec;
	fs::remove(system_path(join_path(dir, kRunRecordFileName)), ec);
}

} // namespace opennova::editor
