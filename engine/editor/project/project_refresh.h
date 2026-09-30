#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_scan.h>
#include <editor/import/import_pass.h>
#include <editor/import/import_run.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

// The project read again from its files, the one way the editor and the command line share (ADR
// 0046 d4 CLI parity, d8 "import changed sources"), a step at a time (S13 A3): the import pass
// first (ImportPass), so the scan lists what the importers made; the scan (ProjectScan), the pass's
// findings riding it (AssetScan::set_import_findings), so whatever reads the scan (the Problems
// rows, `validate`, the build's gate) sees them; then the requirements over that scan. `force` and
// `only` are the import pass's (import_run.h): a Reimport is this refresh with the sources it
// names forced. Each walk steps by a budget of bytes, so the session runs a refresh as an
// operation's steps (Open, Refresh, and the refresh an import's write or an import source's
// rename ends with) and a caller that waits runs it to its end in one call; either way it comes to
// the same scan.
class ProjectRefresh {
public:
	ProjectRefresh(const ProjectPaths &paths, const ProjectDocument &doc, bool force_import = false,
			const std::string &only = std::string());

	// One step within `budget` bytes (at least one file listed, taken or visited); true once the
	// refresh is done, its imports, scan and requirements made.
	bool step(uint64_t budget);
	bool done() const { return done_; }
	// Where it stands, in files: the import sources and the project's files gone through, of those
	// listed so far (a count that grows as each walk lists, and stands once both have), and what it
	// works on now ("Scanning menus/main.mnu").
	uint64_t files_done() const;
	uint64_t files_total() const;
	std::string label() const;
	// The project files its scan read.
	size_t files_scanned() const { return walk_.files_visited(); }

	// What it came to, once done.
	ImportRunResult &imports() { return imports_; }
	AssetScan &scan() { return scan_; }
	RequirementReport &requirements() { return requirements_; }

private:
	ProjectDocument doc_;
	ImportPass pass_;
	ProjectScan walk_;
	ImportRunResult imports_;
	AssetScan scan_;
	RequirementReport requirements_;
	bool done_ = false;
};

} // namespace opennova::editor
