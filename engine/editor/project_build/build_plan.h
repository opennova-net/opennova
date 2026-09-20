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
// diagnostics (a scan error, an unmet Required row, a file the engine cannot store) is
// not run: Play and Export both refuse rather than pack a game that cannot boot.
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
};

BuildPlan plan_build(const ProjectPaths &paths, const ProjectDocument &doc, const AssetScan &scan,
                     const RequirementReport &requirements);

} // namespace opennova::editor
