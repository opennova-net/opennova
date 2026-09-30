#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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
// It is refresh_project, then project_rows. A validation stepped a file at a time over several
// polls (S13 A3, ProjectValidation below) runs the same parts in the same order:
// AssetGraph::update, ValidationCache::begin, then ValidationCache::file_findings for each file of
// validation_files, one step at a time, then ValidationCache::end; and the rows: the files' kept
// findings, run_use_checks (use_checks.h) and AssetGraph::diagnostics. Only the graph's update and
// file_findings read files; the use checks read the graph the update made and which files'
// records their own checks read, so they run after the last file. The scan and the open documents
// (their revisions) stay as the update saw them until the use checks ran: an edit between two
// steps starts the validation again, or a file's findings would be of another revision than the
// graph's.
std::vector<Diagnostic> validate_project(
		const ValidationInput &input, AssetGraph &graph, ValidationCache &cache);

// validate_project's first half, for a caller that keeps the rows it made (the session): the
// graph brought to the files and each file's own findings made or kept. True when a row the
// second half gives may have moved since the last validation over `graph` and `cache`: a file's
// findings made again, a file gone from the validation, or the graph changed.
bool refresh_project(const ValidationInput &input, AssetGraph &graph, ValidationCache &cache);
// Its second half: the rows, in validate_project's order, from what refresh_project left.
std::vector<Diagnostic> project_rows(
		const ValidationInput &input, const AssetGraph &graph, const ValidationCache &cache);

// The files validate_project gives their own checks, in its order: every file of the scan a
// document type opens, type by type in the registry's order, each type's in the scan's order.
std::vector<const AssetEntry *> validation_files(const AssetScan &scan);

// What a step of a stepped validation spends on a file besides the bytes of a closed file it
// loads: asking the cache, and a file's own checks when they are made again.
inline constexpr uint64_t kValidationFileCost = 4096;

// refresh_project a file at a time (ADR 0046 S13 A3): the graph brought to the files in the first
// step, then each file's own findings (validation_files' order), each within a step's budget of
// bytes, then the files the validation did not ask let go; what the session's poll runs a step of
// before the running operation's, so an editor validating a large project for the first time keeps
// drawing. `input` is the same from the first step to the last (its scan, the open documents and
// their revisions): a caller whose input moved starts another, which reuses what the cache kept.
// The graph and the cache are its caller's and move as it steps (the graph at its first step).
class ProjectValidation {
public:
	ProjectValidation(AssetGraph &graph, ValidationCache &cache);

	// One step within `budget` bytes (at least the graph's update, or one file); true once the
	// validation is done (moved() then says whether a row project_rows gives may have moved: a
	// file's findings made again, a file gone from the validation, or the graph changed).
	bool step(const ValidationInput &input, uint64_t budget);
	bool done() const { return phase_ == Phase::Done; }
	bool moved() const { return moved_; }
	// Whether the graph's update changed it (its first step).
	bool graph_moved() const;
	// Where it stands: the files asked and the files to ask (0 before the graph's update), and the
	// file asked last.
	size_t files_done() const { return next_; }
	size_t files_total() const { return files_.size(); }
	const std::string &current() const { return current_; }

private:
	enum class Phase : uint8_t { Graph, Files, Done };

	AssetGraph &graph_;
	ValidationCache &cache_;
	uint64_t generation_ = 0; // the graph's before its update
	std::vector<const AssetEntry *> files_;
	size_t next_ = 0;
	std::string current_;
	Phase phase_ = Phase::Graph;
	bool moved_ = false;
};

} // namespace opennova::editor
