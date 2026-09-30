#include <editor/run/play_lease.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

#include <base/io/json.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A build directory's name and the path it lives at.
fs::path normalized_dir(const std::string &build_dir) {
	fs::path dir = fs::path(build_dir).lexically_normal();
	if (dir.filename().empty()) dir = dir.parent_path();
	return dir;
}

// The lease the game `pid` holds on `build_dir`: <build-id>.<pid>.lease beside it.
fs::path lease_path_of(const std::string &build_dir, int64_t pid) {
	const fs::path dir = normalized_dir(build_dir);
	return dir.parent_path() / (dir.filename().string() + "." + std::to_string(pid) + kPlayLeaseSuffix);
}

// The build id and the pid a lease's file name carries (<build-id>.<pid>.lease); false for any
// other name.
bool parse_lease_name(const fs::path &path, std::string &build_id, int64_t &pid) {
	if (path.extension() != kPlayLeaseSuffix) return false;
	const std::string stem = path.stem().string(); // <build-id>.<pid>
	const size_t dot = stem.find('.');
	if (dot == std::string::npos) return false;
	build_id = stem.substr(0, dot);
	const std::string digits = stem.substr(dot + 1);
	if (!is_build_id(build_id) || digits.empty() || digits.size() > 18 ||
	    !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
		return false;
	pid = std::stoll(digits);
	return true;
}

// The lease record at `path`, when it is one and names the build id and the pid its name does.
bool read_lease(const fs::path &path, const std::string &build_id, int64_t pid, std::string &executable) {
	std::string text, error;
	if (!read_file_text(path.generic_string(), text, error)) return false;
	io::JsonValue json;
	if (!io::json_parse(text, json, error) || !json.is_object()) return false;
	if (json.get_int("schema_version", -1) != kPlayLeaseSchemaVersion) return false;
	if (json.get_string("build_id", "") != build_id) return false;
	const double number = json.get_number("pid", -1.0);
	if (number < 0.0 || number != std::floor(number) || static_cast<int64_t>(number) != pid) return false;
	executable = json.get_string("executable", "");
	return true;
}

} // namespace

bool write_play_lease(const PlayLease &lease, std::string &error) {
	const std::string build_id = normalized_dir(lease.build_dir).filename().string();
	if (!is_build_id(build_id) || lease.pid < 0) {
		error = "not a build directory and a process: " + lease.build_dir;
		return false;
	}
	const fs::path path = lease_path_of(lease.build_dir, lease.pid);
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kPlayLeaseSchemaVersion));
	json.set("build_id", io::JsonValue::make_string(build_id));
	json.set("pid", io::JsonValue::make_number(double(lease.pid)));
	json.set("executable", io::JsonValue::make_string(lease.executable));
	return write_file_atomic(path.generic_string(), io::json_write(json), error);
}

void remove_play_lease(const std::string &build_dir, int64_t pid) {
	if (!is_build_id(normalized_dir(build_dir).filename().string()) || pid < 0) return;
	std::error_code ec;
	fs::remove(lease_path_of(build_dir, pid), ec);
}

std::vector<std::string> leased_build_dirs(const std::string &output_root, const LeaseLiveness &liveness) {
	std::vector<std::string> dirs;
	std::error_code ec;
	for (const fs::directory_entry &entry :
			fs::directory_iterator(output_root, fs::directory_options::skip_permission_denied, ec)) {
		if (ec) break;
		const fs::path &path = entry.path();
		std::string build_id, executable;
		int64_t pid = -1;
		if (!entry.is_regular_file(ec) || !parse_lease_name(path, build_id, pid) ||
		    !read_lease(path, build_id, pid, executable))
			continue; // not a lease: never ours to read further or delete
		const ProcessLiveness state = liveness ? liveness(pid, executable) : ProcessLiveness::Unknown;
		if (state == ProcessLiveness::Dead) {
			std::error_code removed;
			fs::remove(path, removed);
			continue;
		}
		const std::string dir = (fs::path(output_root) / build_id).generic_string();
		if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
	}
	return dirs;
}

} // namespace opennova::editor
