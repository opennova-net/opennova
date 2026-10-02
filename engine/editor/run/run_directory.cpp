#include <editor/run/run_directory.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <system_error>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A run directory's name: a number from 1, written without leading zeros (a few digits: a run root
// holds the runs whose games may still run, and one more).
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
	if (!read_file_text((dir / kRunRecordFileName).string(), text, error)) return false;
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

} // namespace

bool take_run_directory(const std::string &runs_root, const LeaseLiveness &liveness, std::string &out,
                        std::string &error) {
	if (!ensure_directory(runs_root, error)) return false;
	// The numbered directories there, each with whether its game may still run.
	std::map<unsigned long, bool> runs;
	std::error_code ec;
	for (const fs::directory_entry &entry :
	     fs::directory_iterator(system_path(runs_root), fs::directory_options::skip_permission_denied, ec)) {
		unsigned long number = 0;
		std::error_code kind;
		if (!entry.is_directory(kind) || !run_number(entry.path().filename().string(), number)) continue;
		runs[number] = held(entry.path(), liveness);
	}
	// The first one free, emptied (one whose files will not go, a process holding them, is passed
	// over too); every other one whose game is gone removed.
	unsigned long taken = 0;
	for (unsigned long number = 1; taken == 0; ++number) {
		const auto found = runs.find(number);
		if (found != runs.end() && found->second) continue;
		const fs::path dir = system_path((fs::path(runs_root) / std::to_string(number)).generic_string());
		std::error_code removed;
		fs::remove_all(dir, removed);
		if (fs::exists(dir, removed)) continue;
		taken = number;
	}
	for (const auto &[number, busy] : runs) {
		if (busy || number == taken) continue;
		std::error_code removed;
		fs::remove_all(system_path((fs::path(runs_root) / std::to_string(number)).generic_string()), removed);
	}
	out = (fs::path(runs_root) / std::to_string(taken)).generic_string();
	return ensure_directory(out, error);
}

bool claim_run_directory(const std::string &dir, int64_t pid, const ProcessIdentity &identity, std::string &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kRunRecordSchemaVersion));
	json.set("pid", io::JsonValue::make_number(double(pid)));
	json.set("image", io::JsonValue::make_string(identity.image));
	// A string, as a lease writes it: a creation time passes a JSON number's exact range.
	json.set("created", io::JsonValue::make_string(identity.created));
	return write_file_atomic((fs::path(dir) / kRunRecordFileName).generic_string(), io::json_write(json), error);
}

void release_run_directory(const std::string &dir) {
	if (dir.empty()) return;
	std::error_code ec;
	fs::remove(system_path((fs::path(dir) / kRunRecordFileName).generic_string()), ec);
}

} // namespace opennova::editor
