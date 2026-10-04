#pragma once

#include <cstdint>
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
	// For a project that builds as an expansion (ADR 0046 S16), the names its base game serves (the
	// install as a stock launch mounts it, no expansion, no renames: list_base_file_names), sorted
	// likewise, which its build's gate reads (BaseNames); empty for a standalone project, and when
	// the install mounts nothing (build.expansion.base_missing).
	std::vector<std::string> base_files;
	// The expansions the game install under `retail_directory` has (ADR 0046 S16: the ones the game
	// mounts, vfs_list_expansions), each by its folder's name with the name and the description the
	// Mods list shows for it (vfs_expansion_info): what a project builds on. Read with the install,
	// whether a project is open or not.
	struct InstallExpansion {
		std::string name;
		std::string title;
		std::string description;
	};
	std::vector<InstallExpansion> install_expansions;
	// The same of the install a new project opens with (the editor's last chosen, whatever install an
	// open project names), which the New project form offers and New project weighs against.
	std::vector<InstallExpansion> new_project_expansions;
	// What the last ApplyProjectSettings could not write (each also a finding); its
	// SettingsApplied event (view_events.h) says it came.
	struct SettingsResult {
		std::vector<Diagnostic> failures;
	};
	SettingsResult settings_result;

	// The editor's settings (Preferences, editor_preferences.h): the recent projects, the game
	// install the editor imports from and plays in (the open project's, else the last chosen),
	// Play in the game install, the runtime the settings name ("" = the one packaged beside the
	// editor), whether a preview the windows raise plans the files the chosen ones need, and the items
	// most recently placed in a mission (ADR 0046 S15), most recent first.
	std::vector<std::string> recent_projects;
	std::string retail_directory;
	bool play_retail = false;
	std::string runtime_setting;
	bool import_dependencies = true;
	std::vector<int64_t> recent_items;
	// The folder the project's last Build to folder built into (its local settings' build_folder; "" for
	// none): Build > Build to <it> builds there again (the UX round's problems lane).
	std::string build_folder;
};

} // namespace opennova::editor
