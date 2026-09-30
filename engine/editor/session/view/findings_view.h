#pragma once

#include <memory>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

class AssetGraph;
class ProjectAssetSource;
class ProjectChecks;

// What the project's validation found, as the view shows it (ADR 0046 S13 V4; the Findings and
// Graph concerns): the Problems rows, and what the rows were made from and the windows read too,
// each shared (null before the first validation) and forward-declared, so a header naming the
// view pulls none of the graph's, the asset source's or the project checks' headers.
struct FindingsView {
	// The project's current findings (scan + requirements) followed by the last action's.
	std::vector<Diagnostic> diagnostics;
	// The project's asset graph (S7), rebuilt with every validation: the inspector's
	// badges, the pickers, "find references" and the Problems rows read the same edges.
	std::shared_ptr<const AssetGraph> graph;
	// The project's files by logical name, the open documents standing in for theirs: what
	// the menu preview reads the way the game reads its mounted files. Follows every
	// rescan and every change to the open documents (its generation moves).
	std::shared_ptr<const ProjectAssetSource> assets;
	// The document types' project checks (S13 V9), one per type that has one, by its
	// DocumentTypeId, run with every validation: their findings are Problems rows (never a
	// build's gate). The menu type's is the render check, every menu screen of the project
	// compiled headless as the game draws it (S9j2; preview/menu_render_check.h's
	// menu_render_check), whose render of a screen answers the MCP's editor_menu_preview render.
	std::shared_ptr<const ProjectChecks> project_checks;
};

} // namespace opennova::editor
