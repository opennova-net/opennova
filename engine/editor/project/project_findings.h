#pragma once

#include <memory>
#include <string>
#include <vector>

#include <base/vfs/file_source.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

class MenuRenderCheck;

// What a project's findings are made of beside its files (ADR 0046 S12): the project as the
// one refresh read it (project_state.h: the scan, carrying the import pass's findings, and the
// requirements), the documents open in the editor (standing in for their files), and what
// only the editor knows: the files the last Play's game reported missing when it booted and
// the last Play's own findings (a game that ended with a nonzero exit code), the open
// documents' own findings (a file changed outside the editor that was not read again) and the
// last build's own (those its report adds to the rows it was gated on). The command line has
// none of those.
struct ProjectFindingsInput {
	const ProjectPaths &paths;
	const ProjectDocument &project;
	const AssetScan &scan;
	const RequirementReport &requirements;
	const std::vector<std::shared_ptr<const Document>> &open;
	const std::vector<std::string> &boot_missing;
	const std::vector<Diagnostic> &play;
	const std::vector<Diagnostic> &open_findings;
	const std::vector<Diagnostic> &build;
};

// `documents`: every document type's over the files and the asset graph's
// (validate_open_documents), then the open documents' own: what a build gates on beside the
// scan and the requirements. `rows`: every finding, the Problems rows the editor shows and
// `opennova-project validate` prints, in order: the scan's, the requirements', the boot
// report's, the last Play's, the documents', the menu render check's notes (never a build's gate) and the last
// build's.
struct ProjectFindings {
	std::vector<Diagnostic> documents;
	std::vector<Diagnostic> rows;
};

// The project's findings, composed one way for the editor and the command line: the graph
// updated and the files read through `cache` (the session keeps both from one validation to
// the next; the command line makes them for its one call), and every menu screen compiled
// headless by `render_check` over `files` (the project's files as the game looks them up,
// the open documents standing in).
ProjectFindings compose_project_findings(const ProjectFindingsInput &input, AssetGraph &graph, ValidationCache &cache,
                                         MenuRenderCheck &render_check, const FileSource &files);

} // namespace opennova::editor
