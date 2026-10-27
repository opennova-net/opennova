#include <editor/novaworld/content_state.h>

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

// `text` written at `path` with the content folder made first.
bool write_state(const ContentPaths &paths, const std::string &path, const std::string &text,
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

ContentPaths ContentPaths::for_project(const ProjectPaths &paths) {
	ContentPaths out;
	out.dir = join_path(paths.cache_dir, "content");
	out.binding_file = join_path(out.dir, "binding.json");
	out.base_file = join_path(out.dir, "base.json");
	out.hashes_file = join_path(out.dir, "hashes.json");
	out.incoming_dir = join_path(out.dir, "incoming");
	return out;
}

bool read_content_binding(const ContentPaths &paths, ContentBinding &out, std::string &error) {
	out = ContentBinding();
	error.clear();
	if (!present(paths.binding_file))
		return false;
	std::string text;
	io::JsonValue json;
	if (!io::read_file_text(paths.binding_file, text, error) ||
			!io::json_parse(text, json, error) || !json.is_object()) {
		error = "the content binding " + paths.binding_file + " is unreadable" +
				(error.empty() ? "" : ": " + error);
		return false;
	}
	const io::JsonValue *revision = json.get("revision");
	uint64_t number = 0;
	out.server = json.get_string("server", "");
	out.key = json.get_string("key", "");
	if (json.get_number("schema_version", 0) != kContentBindingSchemaVersion ||
			out.server.empty() || out.key.empty() || !revision ||
			!io::json_exact_whole(*revision, number)) {
		error = "the content binding " + paths.binding_file + " is not one this editor writes";
		out = ContentBinding();
		return false;
	}
	out.revision = static_cast<int64_t>(number);
	return true;
}

bool write_content_binding(
		const ContentPaths &paths, const ContentBinding &binding, std::string &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::json_number(kContentBindingSchemaVersion));
	json.set("server", io::json_string(binding.server));
	json.set("key", io::json_string(binding.key));
	json.set("revision", io::json_number(static_cast<double>(binding.revision)));
	return write_state(paths, paths.binding_file, io::json_write(json), error);
}

bool read_content_base(
		const ContentPaths &paths, std::vector<project::ProjectFile> &out, std::string &error) {
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

bool write_content_base(const ContentPaths &paths, const std::vector<project::ProjectFile> &files,
		std::string &error) {
	return write_state(paths, paths.base_file, project::encode_manifest(files), error);
}

} // namespace opennova::editor
