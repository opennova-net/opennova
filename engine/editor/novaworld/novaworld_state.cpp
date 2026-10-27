#include <editor/novaworld/novaworld_state.h>

#include <filesystem>
#include <system_error>

#include <base/io/file_io.h>
#include <base/io/json.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// Whether the file at `path` is there.
bool present(const std::string &path) {
	std::error_code ec;
	return fs::is_regular_file(system_path(path), ec);
}

// `text` written at `path` with the NovaWorld folder made first.
bool write_state(const NovaWorldPaths &paths, const std::string &path, const std::string &text,
		std::string &error) {
	std::error_code ec;
	fs::create_directories(system_path(paths.dir), ec);
	if (ec) {
		error = "cannot make " + paths.dir + ": " + ec.message();
		return false;
	}
	return io::write_file_atomic(path, text, error);
}

} // namespace

NovaWorldPaths NovaWorldPaths::for_project(const ProjectPaths &paths) {
	NovaWorldPaths out;
	out.dir = join_path(paths.cache_dir, "novaworld");
	out.binding_file = join_path(out.dir, "binding.json");
	out.base_file = join_path(out.dir, "base.json");
	out.hashes_file = join_path(out.dir, "hashes.json");
	out.incoming_dir = join_path(out.dir, "incoming");
	return out;
}

bool read_novaworld_binding(const NovaWorldPaths &paths, NovaWorldBinding &out, std::string &error) {
	out = NovaWorldBinding();
	error.clear();
	if (!present(paths.binding_file))
		return false;
	std::string text;
	io::JsonValue json;
	if (!io::read_file_text(paths.binding_file, text, error) ||
			!io::json_parse(text, json, error) || !json.is_object()) {
		error = "the NovaWorld binding " + paths.binding_file + " is unreadable" +
				(error.empty() ? "" : ": " + error);
		return false;
	}
	const io::JsonValue *revision = json.get("revision");
	uint64_t number = 0;
	out.server = json.get_string("server", "");
	out.key = json.get_string("key", "");
	if (json.get_number("schema_version", 0) != kNovaWorldBindingSchemaVersion ||
			out.server.empty() || out.key.empty() || !revision ||
			!io::json_exact_whole(*revision, number)) {
		error = "the NovaWorld binding " + paths.binding_file + " is not one this editor writes";
		out = NovaWorldBinding();
		return false;
	}
	out.revision = static_cast<int64_t>(number);
	return true;
}

bool write_novaworld_binding(
		const NovaWorldPaths &paths, const NovaWorldBinding &binding, std::string &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::json_number(kNovaWorldBindingSchemaVersion));
	json.set("server", io::json_string(binding.server));
	json.set("key", io::json_string(binding.key));
	json.set("revision", io::json_number(static_cast<double>(binding.revision)));
	return write_state(paths, paths.binding_file, io::json_write(json), error);
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
