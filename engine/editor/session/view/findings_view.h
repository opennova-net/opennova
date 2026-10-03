#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

class AssetGraph;
class ProjectAssetSource;
class ProjectChecks;
struct OriginalData;

// What the session marks each Problems row with (the UX round's problems lane), index for index with the
// rows: whether it is about the game's own data (S15, decided per finding: session/original_files.h) and
// whether a build is refused for it (one of the rows the build's plan is refused over:
// project_build/build_plan.h's build_blockers). Made again whenever the rows or what the game's own data's
// baseline found move; `rows` is how many rows they were made for
// (marks made for other rows are read as none: problem_query.h's in_original_data, blocks_the_build).
struct FindingMarks {
	size_t rows = 0;
	std::vector<uint8_t> original, blocking;
	size_t blocking_count = 0;
	bool operator==(const FindingMarks &o) const {
		return rows == o.rows && original == o.original && blocking == o.blocking;
	}
};

// What the project's validation found, as the view shows it (ADR 0046 S13 V4; the Findings and
// Graph concerns): the Problems rows, and what the rows were made from and the windows read too,
// each shared (made with the session, which fills them as it validates: null only in a view no
// session made) and forward-declared, so a header naming the view pulls none of the graph's, the
// asset source's or the project checks' headers.
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
	// menu_render_check), whose render of a screen answers the menu_render query.
	std::shared_ptr<const ProjectChecks> project_checks;
	// What the game install makes of its own files (session/original_files.h, ADR 0046 S15): validated as a
	// whole, the findings it makes in each file it serves, by key. Problems shows a finding the install makes
	// too in the file of the same name apart, as the original's. Never null while a session holds the view.
	std::shared_ptr<const OriginalData> originals;
	// The rows' marks (above); null in a view no session made, whose readers then work them out from what
	// the view holds.
	std::shared_ptr<const FindingMarks> marks;
};

} // namespace opennova::editor
