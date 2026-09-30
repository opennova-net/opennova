#pragma once

#include <string>

#include <editor/assets/asset_registry.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

// The project as the engine will see it, read to its end by the refresh the editor steps
// (project_refresh.h): the import pass first, so the scan lists what the importers made; the
// scan, carrying the pass's findings; then the requirements over that scan. `force` and `only` are
// the import pass's (import_run.h). The command line's (opennova-project), until it reads the
// project through the session (S13 A7), which leaves this with no caller.
struct ProjectState {
	ImportRunResult imports;
	AssetScan scan;
	RequirementReport requirements;
};

ProjectState refresh_project_state(const ProjectPaths &paths, const ProjectDocument &doc, bool force_import = false,
                                   const std::string &only = std::string());

} // namespace opennova::editor
