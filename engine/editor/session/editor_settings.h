#pragma once

#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The editor's own settings, per machine and per user, not per project (ADR 0046 d6):
// the recent-projects list, the game runtime Play launches, the game install, and whether
// an import brings the files the chosen ones need. The shell names the file (its user
// directory); a project's `.opennova/local.json` overrides the runtime for that project
// alone.
inline constexpr int kEditorSettingsSchemaVersion = 1;
inline constexpr size_t kRecentProjectsMax = 10;

struct EditorSettings {
	std::vector<std::string> recent_projects; // project roots, most recent first
	std::string runtime_executable;           // "" = the runtime packaged beside the editor
	std::string retail_directory;             // the game install (Joint Operations), on this machine
	bool play_retail = false;
	// The import dialog's "Include the files these need" (ADR 0046 S11g): what a preview the
	// windows raise plans with; a file that does not say reads as on.
	bool import_dependencies = true;
};

// A missing file reads as defaults; a present but invalid file is an error.
bool load_editor_settings(const std::string &path, EditorSettings &out, Diagnostic &error);
bool save_editor_settings(const std::string &path, const EditorSettings &settings, Diagnostic &error);

// Move (or add) `root` to the front of the recent list, capped at kRecentProjectsMax.
void remember_recent_project(EditorSettings &settings, const std::string &root);
void forget_recent_project(EditorSettings &settings, const std::string &root);

} // namespace opennova::editor
