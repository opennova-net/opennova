#include <editor/session/file_preferences_store.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/json.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

bool FilePreferencesStore::load(Preferences &out, Diagnostic &finding) {
	const std::string &path = path_;
	std::error_code ec;
	if (!fs::exists(path, ec)) {
		out = Preferences();
		return true;
	}
	std::string text;
	std::string io_error;
	if (!read_file_text(path, text, io_error)) {
		finding =
				make_diagnostic(DiagnosticSeverity::Error, "editor_settings.unreadable", io_error);
		return false;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) {
		finding = make_diagnostic(DiagnosticSeverity::Error, "editor_settings.json",
				path + ": " + (parse_error.empty() ? "not an object" : parse_error));
		return false;
	}
	// Pre-1.0 there is no reader for another schema (schema 1 named the game install
	// retail_directory): the file is set aside, read as absent, and the next save writes a new one.
	if (json.get_int("schema_version", -1) != kPreferencesSchemaVersion) {
		finding = settings_set_aside(path, json, kPreferencesSchemaVersion,
				"editor_settings.schema_version.unsupported",
				"The editor starts from its defaults, as with no settings file, "
				"and its next save writes a new file.");
		out = Preferences();
		return true;
	}
	Preferences settings;
	settings.runtime_executable = json.get_string("runtime_executable", "");
	settings.game_install = json.get_string("game_install", "");
	settings.play_in_install = json.get_bool("play_in_install", false);
	settings.import_dependencies = json.get_bool("import_dependencies", true);
	if (const io::JsonValue *recent = json.get("recent_projects"); recent && recent->is_array()) {
		for (const io::JsonValue &item : recent->array) {
			if (item.is_string() && !item.string.empty()) settings.recent_projects.push_back(item.string);
		}
	}
	out = std::move(settings);
	return true;
}

bool FilePreferencesStore::save(const Preferences &settings, Diagnostic &error) {
	const std::string &path = path_;
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kPreferencesSchemaVersion));
	json.set("runtime_executable", io::JsonValue::make_string(settings.runtime_executable));
	json.set("game_install", io::JsonValue::make_string(settings.game_install));
	json.set("play_in_install", io::JsonValue::make_bool(settings.play_in_install));
	json.set("import_dependencies", io::JsonValue::make_bool(settings.import_dependencies));
	io::JsonValue recent = io::JsonValue::make_array();
	for (const std::string &root : settings.recent_projects) recent.push(io::JsonValue::make_string(root));
	json.set("recent_projects", std::move(recent));
	std::string io_error;
	if (!ensure_directory(fs::path(path).parent_path().generic_string(), io_error) ||
	    !write_file_atomic(path, io::json_write(json), io_error)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "editor_settings.write", io_error);
		return false;
	}
	return true;
}

} // namespace opennova::editor
