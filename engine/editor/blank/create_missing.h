#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

// "Create all missing" (ADR 0046 d7): fill every unmet Required row of the enabled
// phases with its blank factory's file, placed under the kind's folder. A row whose
// file is present but of the wrong kind is never overwritten; a row without a factory
// is reported as unavailable. `only_role` narrows the run to one row (and then also
// admits an optional row a user asked for by name).
struct CreateMissingResult {
	std::vector<std::string> created;     // project-relative paths written
	std::vector<std::string> unavailable; // required file names with no factory yet
	std::vector<Diagnostic> diagnostics;  // write failures, wrong-kind refusals
};

CreateMissingResult create_missing_requirements(const ProjectPaths &paths, const ProjectDocument &doc,
                                                const RequirementReport &report,
                                                std::string_view only_role = std::string_view());

} // namespace opennova::editor
