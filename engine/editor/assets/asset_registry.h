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
	std::string key;           // normalized_logical_name(logical_name), set by AssetScan::index
	std::string relative_path; // project-relative, '/'-separated
	AssetKind kind = AssetKind::Unknown;
	uint64_t size_bytes = 0;
	int64_t modified_ticks = 0; // the file system's own last-write ticks: compared, never shown (0 = unknown)
	std::string imported_from; // the source this file was imported from ("" = authored)
};

struct AssetScan {
	std::vector<AssetEntry> entries; // sorted by key (the normalized logical name), then path
	std::vector<Diagnostic> diagnostics;

	// The entry for a logical name (case-insensitive, the engine's lookup), or nullptr: a
	// binary search of the entries by key; of two files of a name, the first by path.
	const AssetEntry *find(std::string_view logical_name) const;
	// The entry at a project-relative path, or nullptr: a binary search of the path index.
	const AssetEntry *at_path(std::string_view relative_path) const;
	// Keys each entry, sorts the entries by key then path, and indexes their paths: what
	// scan_project_assets does with the files it found. A scan made by hand calls it before it
	// is read, and again after its entries change; until it does, the two lookups walk the
	// entries (a warning on the log sink says so) and never read past them.
	void index();

private:
	std::vector<size_t> by_path_; // the entries' indexes, by relative path
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
