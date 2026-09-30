#include <editor/project/local_settings.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/json.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

bool load_local_settings(const ProjectPaths &paths, LocalSettings &out, Diagnostic &error) {
	const std::string &path = paths.local_settings_file;
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
	// Pre-1.0, another schema reads as an error with no migration (a schema 1 file named the game
	// install retail_root): the game install is chosen again, which writes this schema.
	if (json.get_int("schema_version", -1) != kLocalSettingsSchemaVersion) {
		error = make_diagnostic(DiagnosticSeverity::Error, "local_settings.schema_version.unsupported",
		                        path + ": unsupported schema version (an older editor wrote it): "
		                               "choose the project's game install again in the editor's "
		                               "project settings, or delete the file.");
		return false;
	}
	LocalSettings settings;
	settings.runtime_executable = json.get_string("runtime_executable", "");
	settings.game_install = json.get_string("game_install", "");
	out = std::move(settings);
	return true;
}

bool save_local_settings(const ProjectPaths &paths, const LocalSettings &settings, Diagnostic &error) {
	io::JsonValue json = io::JsonValue::make_object();
	json.set("schema_version", io::JsonValue::make_number(kLocalSettingsSchemaVersion));
	json.set("runtime_executable", io::JsonValue::make_string(settings.runtime_executable));
	json.set("game_install", io::JsonValue::make_string(settings.game_install));
	std::string io_error;
	if (!ensure_project_cache_dir(paths, io_error) ||
	    !write_file_atomic(paths.local_settings_file, io::json_write(json), io_error)) {
		error = make_diagnostic(DiagnosticSeverity::Error, "local_settings.write", io_error);
		return false;
	}
	return true;
}

std::string absolute_install_path(const std::string &path) {
	if (path.empty()) return path;
	std::error_code ec;
	const fs::path absolute = fs::absolute(fs::path(path), ec);
	return (ec ? fs::path(path) : absolute).lexically_normal().generic_string();
}

bool open_local_settings(const ProjectPaths &paths, const std::string &seed_install, LocalSettings &out,
                         Diagnostic &error) {
	if (!load_local_settings(paths, out, error)) {
		out = LocalSettings();
		return false;
	}
	if (!out.game_install.empty() || seed_install.empty()) return true;
	LocalSettings seeded = out;
	seeded.game_install = absolute_install_path(seed_install);
	if (!save_local_settings(paths, seeded, error)) return false;
	out = std::move(seeded);
	return true;
}

} // namespace opennova::editor
