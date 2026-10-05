#pragma once

#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

// Create missing (ADR 0046 d7): the rows of the enabled phases whose roles `roles` names
// (each once, in manifest order; none names nothing, and an optional row is made when it is
// named), filled with their blank factories' files under the kind's folder. A row whose
// file is in the project is refused (create_missing.exists), as is one whose target is on
// disk though the report missed it: an existing file is never overwritten, of the right kind
// or the wrong one. A row without a factory is reported as unavailable; a role no row of
// the report has is refused (create_missing.unknown). A file a made blank names is made
// with it (blank_companion: the pointer a menu names) where `scan`, the project's files the
// report was evaluated over, has none of that name and its target is free.
struct CreateMissingResult {
	std::vector<std::string> created;     // project-relative paths written
	std::vector<std::string> unavailable; // required file names with no factory yet
	std::vector<Diagnostic> diagnostics;  // write failures, refusals
};

CreateMissingResult create_missing_requirements(const ProjectPaths &paths, const ProjectDocument &doc, const AssetScan &scan,
                                                const RequirementReport &report, const std::vector<std::string> &roles);

} // namespace opennova::editor
