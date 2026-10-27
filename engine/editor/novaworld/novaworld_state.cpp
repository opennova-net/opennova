#include <editor/novaworld/novaworld_state.h>

#include <filesystem>
#include <system_error>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// Whether the file at `path` is there.
bool present(const std::string &path) {
	std::error_code ec;
	return !path.empty() && fs::is_regular_file(system_path(path), ec);
}

// `text` written at `path` with the service's folder made first.
bool write_state(const NovaWorldPaths &paths, const std::string &path, const std::string &text,
		std::string &error) {
	if (paths.service_dir.empty()) {
		error = "no NovaWorld service names the folder's state";
		return false;
	}
	std::error_code ec;
	fs::create_directories(system_path(paths.service_dir), ec);
	if (ec) {
		error = "cannot make " + paths.service_dir + ": " + ec.message();
		return false;
	}
	return io::write_file_atomic(path, text, error);
}

} // namespace

std::string novaworld_service_folder(const std::string &server) {
	std::string host = novaworld_host(server);
	for (char &c : host) {
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
		else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
			c = '_';
	}
	return host;
}

NovaWorldPaths NovaWorldPaths::for_project(const ProjectPaths &paths, const std::string &server) {
	NovaWorldPaths out;
	out.dir = join_path(paths.cache_dir, "novaworld");
	out.hashes_file = join_path(out.dir, "hashes.json");
	out.incoming_dir = join_path(out.dir, "incoming");
	const std::string host = novaworld_service_folder(server);
	if (!host.empty()) {
		out.service_dir = join_path(out.dir, host);
		out.state_file = join_path(out.service_dir, "state.json");
		out.base_file = join_path(out.service_dir, "base.json");
	}
	return out;
}

bool read_novaworld_state(
		const NovaWorldPaths &paths, NovaWorldWorkingCopy &out, std::string &error) {
	out = NovaWorldWorkingCopy();
	error.clear();
	if (!present(paths.state_file))
		return false;
	std::string text;
	io::JsonValue json;
	if (!io::read_file_text(paths.state_file, text, error) || !io::json_parse(text, json, error) ||
			!json.is_object()) {
		error = "the NovaWorld state " + paths.state_file + " is unreadable" +
				(error.empty() ? "" : ": " + error);
		return false;
	}
	const io::JsonValue *revision = json.get("revision");
	uint64_t number = 0;
	out.key = json.get_string("key", "");
	if (json.get_number("schema", 0) != kNovaWorldStateSchemaVersion ||
			!is_novaworld_key(out.key) || !revision || !io::json_exact_whole(*revision, number)) {
		error = "the NovaWorld state " + paths.state_file + " is not one this editor writes";
		out = NovaWorldWorkingCopy();
		return false;
	}
	out.revision = static_cast<int64_t>(number);
	return true;
}

bool write_novaworld_state(
		const NovaWorldPaths &paths, const NovaWorldWorkingCopy &copy, std::string &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema", io::json_number(kNovaWorldStateSchemaVersion));
	json.set("key", io::json_string(copy.key));
	json.set("revision", io::json_number(static_cast<double>(copy.revision)));
	return write_state(paths, paths.state_file, io::json_write(json), error);
}

bool read_novaworld_base(
		const NovaWorldPaths &paths, std::vector<project::ProjectFile> &out, std::string &error) {
	out.clear();
	error.clear();
	if (!present(paths.base_file))
		return true;
	std::string text;
	if (!io::read_file_text(paths.base_file, text, error))
		return false;
	if (!project::decode_manifest(text, out, error)) {
		error = "the base manifest " + paths.base_file + " is unreadable: " + error;
		return false;
	}
	return true;
}

bool write_novaworld_base(const NovaWorldPaths &paths, const std::vector<project::ProjectFile> &files,
		std::string &error) {
	return write_state(paths, paths.base_file, project::encode_manifest(files), error);
}

} // namespace opennova::editor
