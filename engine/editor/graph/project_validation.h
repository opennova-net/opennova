#pragma once

#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/validation_cache.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

class AssetGraph;

// The findings of the project's files (ADR 0046 S13 D4), in one pass: the asset graph brought to
// the files (the open documents standing in for theirs), each file's own findings through `cache`
// (ValidationCache::file_findings: its document type's validate_file, made again only when the
// file or its open document changed, a closed file loaded for them and let go), what other files
// make of what each file defines (use_checks.h), then the graph's findings (a reference nothing
// resolves, a native file it could not read). The rows come in that order: the files type by
// type in the registry's order, each type's in the scan's order (validation_files); the use
// checks' rows in their table's order; the graph's. What a build gates on beside the scan and the
// requirements. The session keeps `graph` and `cache` from one validation to the next, so an edit
// of one open document validates it alone and extracts it alone; the command line makes both for
// its one call.
//
// A validation stepped a file at a time over several polls (S13 A3) runs the same parts in the
// same order: AssetGraph::update, ValidationCache::begin, then ValidationCache::file_findings for
// each file of validation_files, one step at a time, then run_use_checks (use_checks.h),
// ValidationCache::end and AssetGraph::diagnostics. Only the graph's update and file_findings read
// files; the use checks read the graph the update made and which files' records their own checks
// read, so they run after the last file. The scan and the open documents (their revisions) stay
// as the update saw them until the use checks ran: an edit between two steps starts the
// validation again, or a file's findings would be of another revision than the graph's.
std::vector<Diagnostic> validate_project(
		const ValidationInput &input, AssetGraph &graph, ValidationCache &cache);

// The files validate_project gives their own checks, in its order: every file of the scan a
// document type opens, type by type in the registry's order, each type's in the scan's order.
std::vector<const AssetEntry *> validation_files(const AssetScan &scan);

} // namespace opennova::editor
