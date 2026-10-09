#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <editor/assets/asset_kind.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>
#include <formats/pff/pff.h>

namespace opennova::editor {

// One file of the project tree as the engine will see it (ADR 0046 d6): its identity is
// the flat logical name (the basename), its path is organization only.
struct AssetEntry {
	std::string logical_name;  // basename, original case
	std::string key;           // pff::normalized_logical_name(logical_name), set by AssetScan::index
	std::string relative_path; // project-relative, '/'-separated
	AssetKind kind = AssetKind::Unknown;
	uint64_t size_bytes = 0;
	int64_t modified_ticks = 0; // the file system's own last-write ticks: compared, never shown (0 = unknown)
	std::string imported_from; // the source this file was imported from ("" = authored)
};

struct AssetScan {
	std::vector<AssetEntry> entries; // sorted by key (the normalized logical name), then path
	// The walk's findings (each file's, in the order of the files' paths), then the names' (too
	// long, blank, two files of one name, a kind the game does not use: in the entries' order),
	// then the import pass's that neither made (set_import_findings).
	std::vector<Diagnostic> diagnostics;

	// The entry for a logical name (case-insensitive, the engine's lookup), or nullptr: a
	// binary search of the entries by key; of two files of a name, the first by path.
	const AssetEntry *find(std::string_view logical_name) const;
	// The entry at a project-relative path, or nullptr: a binary search of the path index.
	const AssetEntry *at_path(std::string_view relative_path) const;
	// The entry a request names, its case aside (names are the game's, case-insensitive in its
	// archives and in the project): the one at that path, else that path in another case, else, for
	// a name alone, find()'s. A path names its folder: the file of its name in another folder is not
	// it. Null for none, and when two files answer to the path in another case (a file system that
	// keeps case may hold both): `ambiguous` then true.
	const AssetEntry *named(std::string_view file, bool *ambiguous = nullptr) const;
	// Keys each entry, sorts the entries by key then path, and indexes their paths: what
	// scan_project_assets does with the files it found. A scan made by hand calls it before it
	// is read, and again after its entries change; until it does, the two lookups walk the
	// entries (a warning on the log sink says so) and never read past them.
	void index();

	// What the walk made of one project file (assets/project_scan.h, S13 A3): its entries (the file
	// itself, or the outputs an import record lists) and its findings, and the file's own size and last
	// write as the visit found them (an import record's too, which is no entry: what a look for changes
	// made outside the editor compares, assets/disk_changes.h). A scan keeps each by the file's
	// project-relative path, so a file visited again replaces what it made, alone.
	struct Visit {
		std::vector<AssetEntry> entries;
		std::vector<Diagnostic> findings;
		uint64_t size_bytes = 0;
		int64_t modified_ticks = 0; // the file system's own ticks (0 = unknown)
		// An import record's inputs (S20), project-relative: the files at them are ImportInputs.
		std::vector<std::string> inputs;
	};
	// The scan of the walk's visits, by path ("" the project's folder itself, when it cannot be
	// read): the entries indexed and the findings made from them (what a ProjectScan hands over).
	void set_visits(std::map<std::string, Visit> visits);
	// The walk's visits, by project-relative path: every file it listed (an import record among them).
	const std::map<std::string, Visit> &visits() const { return visits_; }
	// The folders the walk descended into, by project-relative path ("" the project's own), each with
	// its last write as the walk took it before listing what it holds (ADR 0046 DI-01): a folder's last
	// write moves when a file is made, deleted or renamed in it, so a folder whose stamp moved since is
	// one to list again. Set by the walk (ProjectScan); an update keeps them as the walk took them.
	const std::map<std::string, int64_t> &folders() const { return folders_; }
	void set_folders(std::map<std::string, int64_t> folders) { folders_ = std::move(folders); }
	// The files at `changed` (project-relative) visited again as the walk visits them
	// (scan_project_file), and nothing else read: a file gone, or one the walk does not reach,
	// leaves the scan; an import record is visited with its source and a source with its record (a
	// PNG's kind follows its record, a record's findings its source). The entries and the findings
	// are made again from the visits, so a scan updated equals a scan of the files made afresh. The
	// files it read: those found. A scan made by hand holds no visits and is not updated.
	size_t update(const ProjectPaths &paths, const ProjectDocument &doc, const std::vector<std::string> &changed);
	// The import pass's findings (import/import_run.h), each one neither the walk nor the names
	// made listed after theirs, so whatever reads the scan (the Problems rows, `validate`, the
	// build's gate) sees them; an update keeps them.
	void set_import_findings(std::vector<Diagnostic> findings);
	const std::vector<Diagnostic> &import_findings() const { return import_findings_; }

private:
	// The entries from the visits, indexed, and the findings made again.
	void compose();

	std::vector<size_t> by_path_;         // the entries' indexes, by relative path
	std::map<std::string, Visit> visits_; // the walk's visits, by project-relative path
	std::map<std::string, int64_t> folders_; // the walk's folders and their stamps (folders())
	std::vector<Diagnostic> import_findings_;
};

// Walk the project tree to its end (ProjectScan, assets/project_scan.h, steps the same walk by
// bytes). Skips the project file, `.opennova/`, every dot-directory and the export output
// directory; an import sidecar is no file of its own but lists the outputs its source's importer
// made under the cache (a record whose source is gone lists nothing and is an
// `import.orphan_record` warning; an output that is not there is an `import.output_missing`
// warning). Classifies each file (reading only the `.bin` files, whose kind needs a content
// peek); reports duplicate logical names, and over-long ones of a kind the build packs (a loose
// kind takes any name, check_file_name's rule), as errors and unknown kinds as warnings. It reads
// what is on disk: the import pass (project/project_refresh.h) runs first wherever the outputs
// must be current.
AssetScan scan_project_assets(const ProjectPaths &paths, const ProjectDocument &doc);

// The files a filter matches (the UX round's project lane: Files' filter and the files query), as indices
// into `scan.entries` in the scan's order: those of `kind` (kCount: any) whose project-relative path holds
// `text` as normalized names compare; then, where the text names a kind (asset_kind_named_by: "texture",
// "waves") and `kind` is any, the files of that kind not listed yet, so "texture" lists every texture; for
// "kind:<k>" the files of that kind alone. An empty text matches every file of `kind`.
std::vector<size_t> match_files(const AssetScan &scan, const std::string &text, AssetKind kind = AssetKind::kCount);

} // namespace opennova::editor
