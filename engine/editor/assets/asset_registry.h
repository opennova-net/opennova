#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// One file of the project tree as the engine will see it (ADR 0046 d6): its identity is
// the flat logical name (the basename), its path is organization only.
struct AssetEntry {
	std::string logical_name;  // basename, original case
	std::string relative_path; // project-relative, '/'-separated
	AssetKind kind = AssetKind::Unknown;
	uint64_t size_bytes = 0;
	int64_t modified_ticks = 0; // the file system's own last-write ticks: compared, never shown (0 = unknown)
	std::string imported_from; // the source this file was imported from ("" = authored)
};

struct AssetScan {
	std::vector<AssetEntry> entries; // sorted by normalized logical name, then path
	std::vector<Diagnostic> diagnostics;

	// The entry for a logical name (case-insensitive, the engine's lookup), or nullptr.
	const AssetEntry *find(std::string_view logical_name) const;
};

// The engine's identity for a logical name: the PFF normalization (uppercase, trailing
// spaces trimmed) that the reader's lookup and the writer's directory share.
std::string normalized_logical_name(std::string_view name);

// The output-name rules a build must satisfy: at most PFF_NAME_SIZE bytes and not empty
// after normalization.
bool logical_name_fits_archive(std::string_view name);

// Walk the project tree. Skips the project file, `.opennova/`, every dot-directory and
// the export output directory; an import sidecar is no file of its own but lists the
// outputs its source's importer made under the cache (a record whose source is gone
// lists nothing and is an `import.orphan_record` warning; an output that is not there
// is an `import.output_missing` warning). Classifies each file (reading only the `.bin`
// files, whose kind needs a content peek); reports duplicate logical names, and over-long
// ones of a kind the build packs (a loose kind takes any name, check_file_name's rule), as
// errors and unknown kinds as warnings. It reads what is on disk: the import pass
// (project/project_state.h) runs first wherever the outputs must be current.
AssetScan scan_project_assets(const ProjectPaths &paths, const ProjectDocument &doc);

} // namespace opennova::editor
