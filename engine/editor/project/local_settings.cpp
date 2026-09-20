#include <editor/project/local_settings.h>

#include <filesystem>
#include <system_error>

#include <base/io/json.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

bool load_local_settings(const std::string &path, LocalSettings &out, Diagnostic &error) {
	std::error_code ec;
	if (!fs::exists(path, ec)) {
		out = LocalSettings();
		return true;
	}
	std::string text;
	std::string io_error;
	if (!read_file_text(path, text, io_error)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "local_settings.unreadable", io_error);
		return false;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) {
		error = make_diagnostic(DiagnosticSeverity::Error, "local_settings.json",
		                        path + ": " + (parse_error.empty() ? "not an object" : parse_error));
		return false;
	}
	if (json.get_int("schema_version", -1) != kLocalSettingsSchemaVersion) {
		error = make_diagnostic(DiagnosticSeverity::Error, "local_settings.schema_version.unsupported",
		                        path + ": unsupported schema version");
		return false;
	}
	LocalSettings settings;
	settings.runtime_executable = json.get_string("runtime_executable", "");
	settings.retail_root = json.get_string("retail_root", "");
	out = std::move(settings);
	return true;
}

bool save_local_settings(const std::string &path, const LocalSettings &settings, Diagnostic &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kLocalSettingsSchemaVersion));
	json.set("runtime_executable", io::JsonValue::make_string(settings.runtime_executable));
	json.set("retail_root", io::JsonValue::make_string(settings.retail_root));
	std::string io_error;
	if (!ensure_directory(fs::path(path).parent_path().generic_string(), io_error) ||
	    !write_file_atomic(path, io::json_write(json), io_error)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "local_settings.write", io_error);
		return false;
	}
	return true;
}

} // namespace opennova::editor
