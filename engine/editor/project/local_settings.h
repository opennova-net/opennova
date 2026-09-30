#pragma once

#include <string>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/session/finding_codes.h>

namespace opennova::io {
struct JsonValue;
} // namespace opennova::io

namespace opennova::editor {

// Machine-local, never-committed settings of one project (ADR 0046 d6):
// `.opennova/local.json`. Paths here are absolute on this machine; nothing the build
// output depends on lives here. Schema 2 (S13 A4) renamed the game install's key
// ("game_install"); pre-1.0 there is no reader for schema 1: such a file is set aside, read as
// absent with a warning naming what it held (settings_set_aside), and the next write makes a new
// file of schema 2.
inline constexpr int kLocalSettingsSchemaVersion = 2;

struct LocalSettings {
	std::string runtime_executable; // the opennova.exe Play launches ("" = beside the editor)
	// The project's game install (ADR 0046 d6/d10: project-local): what it imports from,
	// depends on and plays in ("" = none). The editor and opennova-project both read it here.
	std::string game_install;
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

} // namespace opennova::editor
