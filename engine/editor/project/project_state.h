#pragma once

#include <string>

#include <editor/assets/asset_registry.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

// The project as the engine will see it, read the one way the editor and the command
// line share (ADR 0046 d4 CLI parity, d8 "import changed sources"): the import pass
// first, so the scan lists what the importers made; the scan; then the requirements
// over that scan. `force` and `only` are the import pass's (import_run.h): a Reimport
// is this refresh with the sources it names forced. The pass's findings ride in the
// scan's diagnostics, so whatever reads the scan (the Problems rows, `validate`, the
// build's gate) sees them.
struct ProjectState {
	ImportRunResult imports;
	AssetScan scan;
	RequirementReport requirements;
};

ProjectState refresh_project_state(const ProjectPaths &paths, const ProjectDocument &doc, bool force_import = false,
                                   const std::string &only = std::string());

} // namespace opennova::editor
