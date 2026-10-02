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

class ProjectChecks;

// What a project's findings are made of beside its files (ADR 0046 S12): the project as the
// one refresh read it (project_refresh.h: the scan, carrying the import pass's findings, and the
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
	const std::vector<std::shared_ptr<const DocumentBase>> &open;
	const std::vector<std::string> &boot_missing;
	const std::vector<Diagnostic> &play;
	const std::vector<Diagnostic> &open_findings;
	const std::vector<Diagnostic> &build;
};

// `rows`: every finding, the Problems rows the editor shows and `opennova-project validate`
// prints, in order: the scan's, the requirements', the boot report's, the last Play's, the
// documents' (each file's own, the use checks', the asset graph's: graph/project_validation.h)
// and the open documents' own, the document types' project checks' (documents/project_check.h,
// in the registry's order: the menu type's render check's notes) and the last build's. The rows
// from `gate_begin` to `gate_end` are the documents' and the open documents' own: what a build
// gates on beside the scan and the requirements (a project check's findings never are). Each
// finding is copied into the rows once; the session moves them into its view.
struct ProjectFindings {
	std::vector<Diagnostic> rows;
	size_t gate_begin = 0;
	size_t gate_end = 0;
};

// The project's findings, composed one way for the editor and the command line: the graph
// updated and each file's own findings kept in `cache`, then the document types' project checks
// (`checks`, one per type that has one) brought to the project over `files` (the project's files
// as the game looks them up, the open documents standing in). The session keeps the graph, the
// cache and the checks from one validation to the next; the command line makes them for its one
// call.
ProjectFindings compose_project_findings(const ProjectFindingsInput &input, AssetGraph &graph, ValidationCache &cache,
                                         ProjectChecks &checks, const FileSource &files);

// compose_project_findings in two halves, for a caller that keeps the rows it made (the session):
// the graph, each file's own findings and the project checks brought to the project, each once,
// true when a row they give may have moved (graph/project_validation.h's refresh_project, or a
// check that says its findings moved); then the rows, from what the first half left.
bool refresh_project_findings(const ProjectFindingsInput &input, AssetGraph &graph, ValidationCache &cache,
                              ProjectChecks &checks, const FileSource &files);
ProjectFindings collect_project_findings(const ProjectFindingsInput &input, const AssetGraph &graph,
                                         const ValidationCache &cache, const ProjectChecks &checks);

} // namespace opennova::editor
