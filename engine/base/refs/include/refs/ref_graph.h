// Whole-root reference graph over the per-format extractors.
//
// RefGraph turns a file listing plus a read callback into a queryable edge set:
// references_of (outgoing), referrers_of (incoming), and broken_references
// (validation). It owns NO I/O and NO resolution policy — the caller supplies
// the file list (the VFS / resource index), the reader, and the existence
// check, so VFS precedence and texture extension fallbacks keep their single
// source of truth.
//
// Rebuilds are memoized per file on (size_bytes, modified_time): a rescan only
// re-extracts files whose stamp moved. Entries with modified_time == 0 (PFF
// archive members carry no mtime) re-extract on every build — the accepted
// blast radius. All graph keys and target matches are lowercase-canonical (the
// engine's lookups are case-insensitive); stored Reference records keep their
// verbatim names.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "refs/refs.h"

namespace opennova::refs {

// One row of the caller's file listing (mirrors the resource index's entries).
struct GraphFileInfo {
    std::string path;
    uint64_t size_bytes = 0;
    uint64_t modified_time = 0;  // 0 = unknown (PFF): re-extract every build
};

// Fills `out` with `path`'s bytes; false = unreadable (the file is skipped and
// recorded in errors()).
using ReadFileFn = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;

// Caller-owned existence semantics for broken_references (e.g. VFS has_file +
// texture candidate probing).
using ExistsFn = std::function<bool(const std::string& target_name, const std::string& target_kind)>;

// Counters from the last build(), for tests and progress reporting.
struct BuildStats {
    size_t files = 0;       // rows in the listing
    size_t recognized = 0;  // rows with an extractor
    size_t extracted = 0;   // rows actually (re-)extracted this build
    size_t memo_hits = 0;   // rows served from the (size, mtime) memo
    size_t failed = 0;      // rows whose extraction errored (see errors())
};

class RefGraph {
public:
    // (Re)build from the listing: extract every recognized file through
    // refs::extract, memoizing unchanged files. Files absent from the listing
    // drop out of the graph. Parse failures fail soft: the row contributes no
    // edges and its error is kept.
    BuildStats build(const std::vector<GraphFileInfo>& files, const ReadFileFn& read_file);

    // Outgoing edges of `path` (case-insensitive; empty when unknown).
    std::vector<Reference> references_of(const std::string& path) const;

    // Incoming edges whose target_name matches `target_name` (case-insensitive).
    std::vector<Reference> referrers_of(const std::string& target_name) const;

    // Every edge whose target does not exist per the caller's `exists`.
    std::vector<Reference> broken_references(const ExistsFn& exists) const;

    // path (lowercase) -> extraction error, from the last build.
    const std::map<std::string, std::string>& errors() const { return errors_; }

private:
    struct Entry {
        uint64_t size_bytes = 0;
        uint64_t modified_time = 0;
        std::vector<Reference> edges;
    };

    // lowercase source path -> extracted entry
    std::map<std::string, Entry> by_source_;
    // lowercase target name -> copies of every edge naming it
    std::map<std::string, std::vector<Reference>> by_target_;
    std::map<std::string, std::string> errors_;

    void rebuild_target_index();
};

}  // namespace opennova::refs
