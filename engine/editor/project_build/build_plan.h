#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <editor/project_build/archive_routing.h>
#include <editor/requirements/requirements.h>

namespace opennova::editor {

// What one build will write (ADR 0046 d8): every packable asset routed into its
// archive, the loose files beside them, and the validation gate. A plan with blocking
// diagnostics (a document finding, a scan error, an unmet Required row, a file the
// engine cannot store) is not run: Play and Export both refuse rather than pack a game
// that cannot boot. An import source and a file of no kind the game knows are left out
// (S13 A8): the first's outputs pack, the second the game never asks for.
struct BuildEntry {
	std::string logical_name;  // the engine-facing name (the archive entry name)
	std::string source_path;   // absolute path of the project file
	uint64_t size_bytes = 0;
};

struct BuildArchive {
	ArchiveSlot slot = ArchiveSlot::Resource;
	std::string file_name;           // "language.pff", ...
	std::vector<BuildEntry> entries; // sorted by normalized logical name
};

struct BuildPlan {
	std::vector<BuildArchive> archives; // always the three boot-table archives, in slot order
	std::vector<BuildEntry> loose;      // copied beside the archives
	std::vector<Diagnostic> diagnostics;
	bool ok = false;                    // false when a diagnostic blocks the build
	// The file the build keeps each file's content hash in, by the size and last write it was
	// hashed at (the project's `.opennova/build_cache.json`, machine-local: S13 A8), so a build
	// reads only the files that changed since; "" keeps none, every file then hashed.
	std::string hash_cache;
	// Every file read again, whatever the cache says, which it then keeps afresh (the build
	// request's `rehash`, the command line's --rehash).
	bool rehash = false;
};

// `document_findings` is the document validation over this scan
// (graph/project_validation.h validate_project) the caller already has: the session's
// last validation's gate, or the command line's one pass. The plan does not validate
// again.
BuildPlan plan_build(const ProjectPaths &paths, const AssetScan &scan, const RequirementReport &requirements,
                     const std::vector<Diagnostic> &document_findings);

} // namespace opennova::editor
