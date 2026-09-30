#include <editor/project/local_settings.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/json.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A value as the file wrote it, on one line (a string quoted).
std::string written_value(const io::JsonValue &value) {
	std::string text = io::json_write(value);
	if (!text.empty() && text.back() == '\n')
		text.pop_back();
	return text;
}

// A member of a file set aside as its warning names it: a list or an object by its size, any
// other value as written.
std::string held_member(const io::JsonMember &member) {
	const io::JsonValue &value = member.value;
	if (value.is_array())
		return member.key + " (" + std::to_string(value.array.size()) +
				(value.array.size() == 1 ? " entry)" : " entries)");
	if (value.is_object())
		return member.key + " (" + std::to_string(value.object.size()) +
				(value.object.size() == 1 ? " member)" : " members)");
	return member.key + " " + written_value(value);
}

} // namespace

Diagnostic settings_set_aside(const std::string &path, const io::JsonValue &json,
		int schema_version, CoreFinding code, const char *afterwards) {
	const io::JsonValue *version = json.get("schema_version");
	const std::string reads = "this editor reads schema " + std::to_string(schema_version);
	std::string origin = "it names no schema version (" + reads + ")";
	if (version)
		origin = "another version of the editor wrote it (schema " + written_value(*version) +
				"; " + reads + ")";
	std::string held;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "schema_version")
			continue;
		held += (held.empty() ? "" : ", ") + held_member(member);
	}
	return make_finding(code, DiagnosticSeverity::Warning,
			path + " is set aside: " + origin +
					", so nothing of it is read, and what it held is gone: " +
					(held.empty() ? std::string("nothing") : held) + ". " + afterwards);
}

bool load_local_settings(const ProjectPaths &paths, LocalSettings &out, Diagnostic &finding) {
	const std::string &path = paths.local_settings_file;
	std::error_code ec;
	if (!fs::exists(path, ec)) {
		out = LocalSettings();
		return true;
	}
	std::string text;
	std::string io_error;
	if (!read_file_text(path, text, io_error)) {
		finding = make_finding(CoreFinding::LocalSettingsUnreadable, DiagnosticSeverity::Error, io_error);
		return false;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) {
		finding = make_finding(CoreFinding::LocalSettingsJson, DiagnosticSeverity::Error,
				path + ": " + (parse_error.empty() ? "not an object" : parse_error));
		return false;
	}
	// Pre-1.0 there is no reader for another schema (schema 1 named the game install retail_root):
	// the file is set aside, read as absent, and the next write makes a new one.
	if (json.get_int("schema_version", -1) != kLocalSettingsSchemaVersion) {
		finding = settings_set_aside(path, json, kLocalSettingsSchemaVersion,
				CoreFinding::LocalSettingsSchemaVersionUnsupported,
				"The project reads as having no local settings, as a project with "
				"no local.json does, and the next write makes a new file.");
		out = LocalSettings();
		return true;
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
		error = make_finding(CoreFinding::LocalSettingsWrite, DiagnosticSeverity::Error, io_error);
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

bool open_local_settings(const ProjectPaths &paths, const std::string &seed_install,
		LocalSettings &out, Diagnostic &finding) {
	if (!load_local_settings(paths, out, finding)) {
		out = LocalSettings();
		return false;
	}
	if (!out.game_install.empty() || seed_install.empty()) return true;
	LocalSettings seeded = out;
	seeded.game_install = absolute_install_path(seed_install);
	if (!save_local_settings(paths, seeded, finding)) return false;
	out = std::move(seeded);
	return true;
}

} // namespace opennova::editor
