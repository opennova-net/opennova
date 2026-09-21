#pragma once

#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// A native file, either loose or a named member of one PFF. No retail-install
// dependency and no conversion pipeline: importing makes an editable project copy.
struct ImportSource {
	std::string path;
	std::string entry; // empty for a loose file
	std::string name() const;
};

struct ImportResult {
	std::vector<std::string> imported; // project-relative paths
	std::vector<Diagnostic> diagnostics;
};

// Expand selected PFFs into selectable members; loose files stay single choices.
std::vector<ImportSource> list_import_sources(const std::vector<std::string> &paths,
                                             std::vector<Diagnostic> &diagnostics);
ImportResult import_assets(const std::vector<ImportSource> &sources, const ProjectPaths &paths,
                           const ProjectDocument &document, bool replace_existing);

} // namespace opennova::editor
