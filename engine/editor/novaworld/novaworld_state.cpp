#include <editor/novaworld/novaworld_state.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <base/io/base64.h>
#include <base/io/sha256.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/novaworld/novaworld_api.h>
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
	out.server = novaworld_server(server);
	out.dir = join_path(paths.cache_dir, "novaworld");
	out.hashes_file = join_path(out.dir, "hashes.json");
	out.incoming_dir = join_path(out.dir, "incoming");
	const std::string host = novaworld_service_folder(server);
	if (!host.empty()) {
		out.service_dir = join_path(out.dir, host);
		out.state_file = join_path(out.service_dir, "state.json");
		out.base_file = join_path(out.service_dir, "base.json");
		out.lobby_file = join_path(out.service_dir, "lobby.json");
		out.version_state = join_path(out.service_dir, "version.json");
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

std::vector<NovaWorldServiceCopy> read_novaworld_copies(const ProjectPaths &paths) {
	std::vector<NovaWorldServiceCopy> out;
	NovaWorldPaths service = NovaWorldPaths::for_project(paths, "");
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(system_path(service.dir), ec)) {
		if (!entry.is_directory(ec))
			continue;
		NovaWorldServiceCopy found;
		found.host = io::utf8_path(entry.path().filename());
		service.state_file = join_path(join_path(service.dir, found.host), "state.json");
		std::string error;
		if (read_novaworld_state(service, found.copy, error))
			out.push_back(std::move(found));
	}
	std::sort(out.begin(), out.end(), [](const NovaWorldServiceCopy &a, const NovaWorldServiceCopy &b) {
		return a.host < b.host;
	});
	return out;
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

bool cache_novaworld_lobby(const NovaWorldPaths &paths, const std::string &key,
		const RemoteLobby &lobby, std::string &error) {
	const std::string &encoded = lobby.gate_tag_file;
	if (encoded.empty() || encoded.size() > 10924) {
		error = "the service's lobby file is missing or too large";
		return false;
	}
	// Strictly: whitespace, bad padding and trailing bits refused (one text per file).
	std::vector<uint8_t> bytes;
	if (!io::base64_decode_strict(encoded, bytes)) {
		error = "the service's lobby file is not base64";
		return false;
	}
	if (!is_novaworld_key(key) || lobby.slug.empty() || lobby.protocol.code.empty() ||
			bytes.empty() || bytes.size() > 8191) {
		error = "the service's lobby file is missing or invalid";
		return false;
	}
	const std::string sha = io::sha256_hex(bytes.data(), bytes.size());
	const std::string dir = join_path(paths.service_dir, "lobbies/" + sha);
	if (paths.service_dir.empty() || !io::ensure_directory(dir, error))
		return false;
	const std::string file = join_path(dir, "gt.ssc");
	std::vector<uint8_t> existing;
	if ((!io::read_file_bytes(file, existing, error) || existing != bytes) &&
			!io::write_file_atomic(file, bytes.data(), bytes.size(), error))
		return false;
	error.clear();
	io::JsonValue json = io::JsonValue::make_object();
	json.set("key", io::json_string(key));
	json.set("server", io::json_string(paths.server));
	json.set("protocol", io::json_string(lobby.protocol.code));
	json.set("slug", io::json_string(lobby.slug));
	json.set("display_name", io::json_string(lobby.display_name));
	json.set("sha256", io::json_string(sha));
	return write_state(paths, paths.lobby_file, io::json_write(json), error);
}

std::string cached_novaworld_gate_tag(
		const NovaWorldPaths &paths, const std::string &key, const std::string &protocol) {
	std::string text, error;
	io::JsonValue json;
	if (!io::read_file_text(paths.lobby_file, text, error) || !io::json_parse(text, json, error) ||
			json.get_string("server", "") != paths.server || json.get_string("key", "") != key ||
			json.get_string("protocol", "") != protocol)
		return {};
	const std::string sha = json.get_string("sha256", "");
	if (!project::is_sha256_hex(sha))
		return {};
	const std::string path = join_path(paths.service_dir, "lobbies/" + sha + "/gt.ssc");
	std::vector<uint8_t> bytes;
	if (!io::read_file_bytes(path, bytes, error) || bytes.empty() || bytes.size() > 8191 ||
			io::sha256_hex(bytes.data(), bytes.size()) != sha)
		return {};
	return path;
}


// The same bound as the lobby file's: the service writes one line.
constexpr size_t kVersionFileMax = 8191;

bool cache_novaworld_version_file(const NovaWorldPaths &paths, const std::string &key,
		const std::string &encoded, std::string &error) {
	if (paths.service_dir.empty()) {
		error = "no service folder";
		return false;
	}
	std::error_code ignored;
	if (encoded.empty()) {
		fs::remove(system_path(paths.version_state), ignored);
		return true;
	}
	std::vector<uint8_t> bytes;
	if (encoded.size() > 10924 || !io::base64_decode_strict(encoded, bytes) || bytes.empty() ||
			bytes.size() > kVersionFileMax || !is_novaworld_key(key)) {
		fs::remove(system_path(paths.version_state), ignored);
		error = "the service's version file is missing or invalid";
		return false;
	}
	const std::string sha = io::sha256_hex(bytes.data(), bytes.size());
	const std::string dir = join_path(paths.service_dir, "versions/" + sha);
	if (!io::ensure_directory(dir, error))
		return false;
	const std::string file = join_path(dir, "version.txt");
	std::vector<uint8_t> existing;
	if ((!io::read_file_bytes(file, existing, error) || existing != bytes) &&
			!io::write_file_atomic(file, bytes.data(), bytes.size(), error))
		return false;
	error.clear();
	io::JsonValue json = io::JsonValue::make_object();
	json.set("key", io::json_string(key));
	json.set("server", io::json_string(paths.server));
	json.set("sha256", io::json_string(sha));
	return write_state(paths, paths.version_state, io::json_write(json), error);
}

std::string cached_novaworld_version_file(const NovaWorldPaths &paths, const std::string &key) {
	std::string text, error;
	io::JsonValue json;
	if (!io::read_file_text(paths.version_state, text, error) || !io::json_parse(text, json, error) ||
			json.get_string("server", "") != paths.server || json.get_string("key", "") != key)
		return {};
	const std::string sha = json.get_string("sha256", "");
	if (!project::is_sha256_hex(sha))
		return {};
	const std::string path = join_path(paths.service_dir, "versions/" + sha + "/version.txt");
	std::vector<uint8_t> bytes;
	if (!io::read_file_bytes(path, bytes, error) || bytes.empty() || bytes.size() > kVersionFileMax ||
			io::sha256_hex(bytes.data(), bytes.size()) != sha)
		return {};
	return path;
}

} // namespace opennova::editor
