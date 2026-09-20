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
	int64_t modified_time = 0; // Unix seconds, 0 when unknown
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

// Walk the project tree. Skips the project file, import sidecars, `.opennova/`, every
// dot-directory and the export output directory; classifies each file (reading only the
// `.bin` files, whose kind needs a content peek); reports duplicate or over-long logical
// names as errors and unknown kinds as warnings.
AssetScan scan_project_assets(const ProjectPaths &paths, const ProjectDocument &doc);

} // namespace opennova::editor
