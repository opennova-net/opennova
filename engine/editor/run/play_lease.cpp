#include <editor/run/play_lease.h>

#include <cmath>
#include <filesystem>
#include <system_error>

#include <base/io/json.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// The lease of `build_dir`: its name and ".lease", beside it.
fs::path lease_path_of(const std::string &build_dir) {
	fs::path dir = fs::path(build_dir).lexically_normal();
	if (dir.filename().empty()) dir = dir.parent_path();
	return dir.parent_path() / (dir.filename().string() + kPlayLeaseSuffix);
}

// The lease record at `path`, when the file is one.
bool read_lease(const fs::path &path, int64_t &pid, std::string &executable) {
	std::string text, error;
	if (!read_file_text(path.generic_string(), text, error)) return false;
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object()) return false;
	if (json.get_int("schema_version", -1) != kPlayLeaseSchemaVersion) return false;
	const std::string id = path.stem().string();
	if (json.get_string("build_id", "") != id) return false;
	const double number = json.get_number("pid", -1.0);
	if (number < 0.0 || number != std::floor(number)) return false;
	pid = static_cast<int64_t>(number);
	executable = json.get_string("executable", "");
	return true;
}

} // namespace

bool write_play_lease(const PlayLease &lease, std::string &error) {
	const fs::path path = lease_path_of(lease.build_dir);
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kPlayLeaseSchemaVersion));
	json.set("build_id", io::JsonValue::make_string(path.stem().string()));
	json.set("pid", io::JsonValue::make_number(double(lease.pid)));
	json.set("executable", io::JsonValue::make_string(lease.executable));
	return write_file_atomic(path.generic_string(), io::json_write(json), error);
}

void remove_play_lease(const std::string &build_dir) {
	std::error_code ec;
	fs::remove(lease_path_of(build_dir), ec);
}

std::vector<std::string> live_leased_dirs(const std::string &output_root, ProcessPlatform &platform,
		int64_t running_pid) {
	std::vector<std::string> dirs;
	std::error_code ec;
	for (const fs::directory_entry &entry :
			fs::directory_iterator(output_root, fs::directory_options::skip_permission_denied, ec)) {
		if (ec) break;
		const fs::path &path = entry.path();
		if (!entry.is_regular_file(ec) || path.extension() != kPlayLeaseSuffix || !is_build_id(path.stem().string()))
			continue;
		int64_t pid = -1;
		std::string executable;
		if (!read_lease(path, pid, executable)) continue; // not a lease: never ours to delete
		const bool live = (running_pid >= 0 && pid == running_pid) || platform.process_alive(pid, executable);
		if (live) {
			dirs.push_back((fs::path(output_root) / path.stem()).generic_string());
		} else {
			std::error_code removed;
			fs::remove(path, removed);
		}
	}
	return dirs;
}

} // namespace opennova::editor
