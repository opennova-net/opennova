#pragma once

#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/project/play_mode.h>
#include <editor/project/project_document.h>

namespace opennova::io {
struct JsonValue;
} // namespace opennova::io

namespace opennova::editor {

// Machine-local, never-committed settings of one project (ADR 0046 d6):
// `.opennova/local.json`. Paths here are absolute on this machine; nothing the build
// output depends on lives here, and neither does anything a clone of the project should inherit (how
// Play runs it here: the game install's program, strictly or not, is this checkout's choice). Schema 2 (S13 A4) renamed the game install's key
// ("game_install"); pre-1.0 there is no reader for schema 1: such a file is set aside, read as
// absent with a warning naming what it held (settings_set_aside), and the next write makes a new
// file of schema 2.
inline constexpr int kLocalSettingsSchemaVersion = 2;

struct LocalSettings {
	std::string runtime_executable; // the opennova.exe Play launches ("" = beside the editor)
	// The project's game install (ADR 0046 d6/d10: project-local): what it imports from,
	// depends on and plays in ("" = none). The editor and opennova-project both read it here.
	std::string game_install;
	// The folder Build to folder last built into (the UX round's problems lane: a build for players, a
	// folder outside the project), which Build > Build to <it> builds into again ("" = none yet).
	std::string build_folder;
	// The documents open as the project last closed or the editor quit (the UX round's project lane), in
	// their tabs' order, each with the record selected in it (its Document::locator, "" for none), and the
	// one active: what the project reopens with. Written only when they changed.
	struct OpenDocument {
		std::string path;
		std::string locator;
		bool operator==(const OpenDocument &o) const { return path == o.path && locator == o.locator; }
	};
	std::vector<OpenDocument> open_documents;
	std::string active_document;
	// How this project plays on this checkout ("play_mode"), and whether Play saves every file with
	// unsaved edits first, as Save all does, instead of asking ("save_before_play", DI-26). The project's
	// own, never the editor's: a choice of one project (Strict Play in the game install) never reaches
	// another, nor another editor on the machine. A file that does not say plays in the OpenNova runtime
	// and saves first; one naming a mode no PlayMode has plays in the runtime too, so Play starts the game
	// install only for a project set to it.
	PlayMode play_mode = PlayMode::Runtime;
	bool save_before_play = true;
};

// The project's local.json (paths.local_settings_file). A missing file reads as defaults, and so
// does one of another schema, `finding` then the warning that it was set aside
// (`local_settings.schema_version.unsupported`); a present but invalid file is an error (false,
// `finding` saying why). `finding` is left as it was given when there is nothing to say. The save
// makes the self-ignored `.opennova/` first (ensure_project_cache_dir), so a machine path never
// lands where it could be committed.
bool load_local_settings(const ProjectPaths &paths, LocalSettings &out, Diagnostic &finding);
bool save_local_settings(const ProjectPaths &paths, const LocalSettings &settings, Diagnostic &error);

// A game install as the settings keep it: absolute (a relative path taken from the working
// directory it was given in) and lexically normal, so the editor and opennova-project, run
// from other directories, find the same install; "" (none) stays "".
std::string absolute_install_path(const std::string &path);

// A project's local settings as the editor and opennova-project open them: the file read
// (load_local_settings), and when it names no game install and `seed_install` is not empty
// (the editor's machine setting: the install last chosen), the seed written into it
// (absolute_install_path), so the project keeps that install from then on and the command
// line reads the same one. False with `finding` the error when the file does not read (`out`
// then the defaults) or the seed does not write (`out` then as read); true with `finding` the
// warning when a file of another schema was set aside (read as absent: a seed writes a new file
// over it).
bool open_local_settings(const ProjectPaths &paths, const std::string &seed_install,
		LocalSettings &out, Diagnostic &finding);

// A settings file of another schema (a project's local.json, the editor's settings file): pre-1.0
// there is no reader for one, so it is set aside, read as absent, and the next write makes a new
// file. The warning `code` names the file at `path`, the schema it has and what it held, now
// gone: each member but its schema version, a string or a switch with its value as written, a
// list by its length, in the file's order; `afterwards` says what is in effect instead.
Diagnostic settings_set_aside(const std::string &path, const io::JsonValue &json,
		int schema_version, CoreFinding code, const char *afterwards);

// A setting a file held, as a warning or a note names it: `key` and its value as written on one line
// (a string quoted), a list by its length, an object by its count of members.
std::string held_setting(const std::string &key, const io::JsonValue &value);

} // namespace opennova::editor
