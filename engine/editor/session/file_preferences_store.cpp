#include <editor/session/file_preferences_store.h>

#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/json.h>
#include <editor/model/diagnostic.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_files.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// A JSON number that is a whole number a double holds exactly (|n| < 2^53), so its cast to int64_t is
// defined and keeps it.
bool whole_number(const io::JsonValue &value) {
	return value.is_number() && std::isfinite(value.number) && std::floor(value.number) == value.number &&
	       std::fabs(value.number) < 9007199254740992.0;
}

} // namespace

bool FilePreferencesStore::load(Preferences &out, Diagnostic &finding) {
	const std::string &path = path_;
	std::error_code ec;
	if (!fs::exists(system_path(path), ec)) {
		out = Preferences();
		return true;
	}
	std::string text;
	std::string io_error;
	if (!read_file_text(path, text, io_error)) {
		finding =
				make_finding(CoreFinding::EditorSettingsUnreadable, DiagnosticSeverity::Error, io_error);
		return false;
	}
	io::JsonValue json;
	std::string parse_error;
	if (!io::json_parse(text, json, parse_error) || !json.is_object()) {
		finding = make_finding(CoreFinding::EditorSettingsJson, DiagnosticSeverity::Error,
				path + ": " + (parse_error.empty() ? "not an object" : parse_error));
		return false;
	}
	// Pre-1.0 there is no reader for another schema (schema 1 named the game install
	// retail_directory): the file is set aside, read as absent, and the next save writes a new one.
	if (json.get_int("schema_version", -1) != kPreferencesSchemaVersion) {
		finding = settings_set_aside(path, json, kPreferencesSchemaVersion,
				CoreFinding::EditorSettingsSchemaVersionUnsupported,
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
	// The recently placed items (S15), per game (the polish): `recent_items_by_game`, an object of each
	// game's list of whole numbers, the first kRecentItemsMax of each; a number that is not whole, or
	// past what a double holds exactly, is skipped before it is cast. S15's one list (`recent_items`,
	// its items of whichever game) is not read: the next save drops it.
	if (const io::JsonValue *recent = json.get("recent_items_by_game"); recent && recent->is_object()) {
		for (const io::JsonMember &game : recent->object) {
			if (game.key.empty() || !game.value.is_array()) continue;
			std::vector<int64_t> &items = settings.recent_items[game.key];
			for (const io::JsonValue &item : game.value.array) {
				if (items.size() >= kRecentItemsMax) break;
				if (whole_number(item)) items.push_back(int64_t(item.number));
			}
			if (items.empty()) settings.recent_items.erase(game.key);
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
	io::JsonValue by_game = io::JsonValue::make_object();
	for (const auto &game : settings.recent_items) {
		if (game.second.empty()) continue;
		io::JsonValue items = io::JsonValue::make_array();
		for (const int64_t item : game.second) items.push(io::JsonValue::make_number(double(item)));
		by_game.set(game.first, std::move(items));
	}
	json.set("recent_items_by_game", std::move(by_game));
	std::string io_error;
	if (!ensure_directory(utf8_of(path_of(path).parent_path()), io_error) ||
	    !write_file_atomic(path, io::json_write(json), io_error)) {
		error = make_finding(CoreFinding::EditorSettingsWrite, DiagnosticSeverity::Error, io_error);
		return false;
	}
	return true;
}

} // namespace opennova::editor
