#pragma once

#include <editor/documents/editable_document.h>
#include <editor/assets/asset_import.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/project_build/build_session.h>
#include <editor/requirements/requirements.h>
#include <editor/run/play_session.h>

namespace opennova::editor {

// Everything the editor's windows draw (ADR 0046 d10): the records in. The session owns
// one and rewrites it as the project changes; the windows read it by const reference
// every frame and never reach into the session. `revision` bumps on every change so a
// window can cache derived text until it moves.
struct SessionView {
	uint64_t revision = 0;

	std::vector<std::shared_ptr<const EditableDocument>> documents;
	std::string active_document;
	CatalogAddress selection;
	bool unsaved_prompt = false;
	bool quit_requested = false;
	bool project_open = false;
	std::string project_root;
	ProjectDocument document;
	AssetScan scan;
	RequirementReport requirements;
	// The project's current findings (scan + requirements) followed by the last action's.
	std::vector<Diagnostic> diagnostics;

	bool build_running = false;
	size_t build_done = 0;
	size_t build_total = 0;
	std::string build_step;
	bool has_build = false;
	BuildReport last_build;

	PlayState play_state = PlayState::Stopped;
	int64_t play_pid = -1;
	std::string play_command_line;
	bool play_exited_on_its_own = false;
	std::string runtime_executable; // what Play launches (resolved; "" = none found)
	bool source_run = false;        // OpenNova Play drives the Godot binary at the source project
	std::string retail_directory;
	bool play_retail = false;

	std::vector<ImportSource> import_sources;
	bool import_open = false;

	std::vector<std::string> output; // the build log and the running game's log, oldest first
	std::vector<std::string> recent_projects;
	std::string status; // the last thing that happened, one line
};

} // namespace opennova::editor
