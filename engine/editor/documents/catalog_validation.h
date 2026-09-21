#pragma once
#include <editor/assets/asset_registry.h>
#include <editor/documents/editable_document.h>

namespace opennova::editor {
// Used by the editor, CLI validate and Build. Open documents replace their disk
// versions so diagnostics describe the current draft.
std::vector<Diagnostic> validate_catalogs(const ProjectPaths &paths, const ProjectDocument &project,
	const AssetScan &scan, const std::vector<std::shared_ptr<const EditableDocument>> &open = {});
} // namespace opennova::editor
