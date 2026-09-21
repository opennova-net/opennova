#include <editor/session/editor_settings.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <base/io/json.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

bool load_editor_settings(const std::string &path, EditorSettings &out, Diagnostic &error) {
	std::error_code ec;
	if (!fs::exists(path, ec)) {
		out = EditorSettings();
		return true;
	}
	std::string text;
	std::string io_error;
	if (!read_file_text(path, text, io_error)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "editor_settings.unreadable", io_error);
		return false;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) {
		error = make_diagnostic(DiagnosticSeverity::Error, "editor_settings.json",
		                        path + ": " + (parse_error.empty() ? "not an object" : parse_error));
		return false;
	}
	if (json.get_int("schema_version", -1) != kEditorSettingsSchemaVersion) {
		error = make_diagnostic(DiagnosticSeverity::Error, "editor_settings.schema_version.unsupported",
		                        path + ": unsupported schema version");
		return false;
	}
	EditorSettings settings;
	settings.runtime_executable = json.get_string("runtime_executable", "");
	settings.retail_directory = json.get_string("retail_directory", "");
	settings.play_retail = json.get_bool("play_retail", false);
	if (const io::JsonValue *recent = json.get("recent_projects"); recent && recent->is_array()) {
		for (const io::JsonValue &item : recent->array) {
			if (item.is_string() && !item.string.empty()) settings.recent_projects.push_back(item.string);
		}
	}
	out = std::move(settings);
	return true;
}

bool save_editor_settings(const std::string &path, const EditorSettings &settings, Diagnostic &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kEditorSettingsSchemaVersion));
	json.set("runtime_executable", io::JsonValue::make_string(settings.runtime_executable));
	json.set("retail_directory", io::JsonValue::make_string(settings.retail_directory));
	json.set("play_retail", io::JsonValue::make_bool(settings.play_retail));
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

void remember_recent_project(EditorSettings &settings, const std::string &root) {
	forget_recent_project(settings, root);
	settings.recent_projects.insert(settings.recent_projects.begin(), root);
	if (settings.recent_projects.size() > kRecentProjectsMax) {
		settings.recent_projects.resize(kRecentProjectsMax);
	}
}

void forget_recent_project(EditorSettings &settings, const std::string &root) {
	settings.recent_projects.erase(
	        std::remove(settings.recent_projects.begin(), settings.recent_projects.end(), root),
	        settings.recent_projects.end());
}

} // namespace opennova::editor
