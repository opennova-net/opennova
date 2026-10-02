#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

struct AssetScan;
struct ImportedSource;
struct ProjectDocument;
struct RequirementReport;

// The open project as the view shows it (ADR 0046 S13 V4; the Project, Files and Preferences
// concerns): whether one is open, its folder and its project document, the files the scan lists,
// the requirements over them, the project's importable sources, the game install's file names,
// what the settings' last Apply could not write, and the editor's settings the windows read. The
// heavy members are shared and never null (a ProjectView made empty holds empty ones), so a
// header naming the view pulls none of the scan's, the requirements' or the import pass's
// headers: the session replaces each whole, and a window reads it through the pointer.
struct ProjectView {
	ProjectView(); // each shared member made, empty

	bool open = false;
	std::string root;
	std::shared_ptr<const ProjectDocument> document;
	std::shared_ptr<const AssetScan> scan;
	std::shared_ptr<const RequirementReport> requirements;
	// The project's importable sources (a PNG) with the outputs their importers made
	// (editor/import), refreshed with the scan.
	std::shared_ptr<const std::vector<ImportedSource>> imports;
	// The logical names the game install under `retail_directory` resolves, sorted by their
	// normalized form; empty when no install is set or it mounts nothing.
	std::vector<std::string> retail_files;
	// What the last ApplyProjectSettings could not write (each also a finding); its
	// SettingsApplied event (view_events.h) says it came.
	struct SettingsResult {
		std::vector<Diagnostic> failures;
	};
	SettingsResult settings_result;

	// The editor's settings (Preferences, editor_preferences.h): the recent projects, the game
	// install the editor imports from and plays in (the open project's, else the last chosen),
	// Play in the game install, the runtime the settings name ("" = the one packaged beside the
	// editor), and whether a preview the windows raise plans the files the chosen ones need.
	std::vector<std::string> recent_projects;
	std::string retail_directory;
	bool play_retail = false;
	std::string runtime_setting;
	bool import_dependencies = true;
};

} // namespace opennova::editor
