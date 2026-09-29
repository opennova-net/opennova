#pragma once

#include <string>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// Machine-local, never-committed settings of one project (ADR 0046 d6):
// `.opennova/local.json`. Paths here are absolute on this machine; nothing the build
// output depends on lives here.
inline constexpr int kLocalSettingsSchemaVersion = 1;

struct LocalSettings {
	std::string runtime_executable; // the opennova.exe Play launches ("" = beside the editor)
	// The project's game install (ADR 0046 d6/d10: project-local): what it imports from,
	// depends on and plays in ("" = none). The editor and opennova-project both read it here.
	std::string retail_root;
};

// The project's local.json (paths.local_settings_file). A missing file reads as defaults; a
// present but invalid file is an error. The save makes the self-ignored `.opennova/` first
// (ensure_project_cache_dir), so a machine path never lands where it could be committed.
bool load_local_settings(const ProjectPaths &paths, LocalSettings &out, Diagnostic &error);
bool save_local_settings(const ProjectPaths &paths, const LocalSettings &settings, Diagnostic &error);

// A game install as the settings keep it: absolute (a relative path taken from the working
// directory it was given in) and lexically normal, so the editor and opennova-project, run
// from other directories, find the same install; "" (none) stays "".
std::string absolute_install_path(const std::string &path);

// A project's local settings as the editor and opennova-project open them: the file read
// (load_local_settings), and when it names no game install and `seed_install` is not empty
// (the editor's machine setting: the install last chosen), the seed written into it
// (absolute_install_path), so the project keeps that install from then on and the command
// line reads the same one. False with `error` when the file does not read (`out` then the
// defaults) or the seed does not write (`out` then as read).
bool open_local_settings(const ProjectPaths &paths, const std::string &seed_install, LocalSettings &out,
                         Diagnostic &error);

} // namespace opennova::editor
