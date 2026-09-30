#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

// A view's findings by where they are (ADR 0046 S13 V1): by file, and by the record of a file
// a finding is on, made once per change of the findings, so a window that reads one file's or
// one record's (the Inspector's record findings, a stylesheet's status line) never scans every
// finding on every frame. Indices are into the view's diagnostics, in their order; a lookup
// leaves out any past their end, so an index the findings outran reads nothing.
class FindingsIndex {
public:
	// What the index is kept by (session_revisions.h): the findings.
	static RevisionKey cache_key(const SessionView &view) {
		return revision_key(view.revisions, {ViewConcern::Findings});
	}

	// Made again from the view's findings when the view or its cache key moved since, or the
	// findings are not the ones it was made from (another count, or moved in memory).
	void follow(const SessionView &view);
	// The findings about a file (project-relative).
	std::vector<size_t> of_file(const std::string &file) const;
	// The findings on a record of a file: on its row (`child` 0: the row's own and every record's
	// it holds), or on one record it holds.
	std::vector<size_t> of_record(const std::string &file, NodeId row, NodeId child) const;

private:
	// A file's findings, and those on each of its rows: a file's name is kept once, not once a
	// finding.
	struct File {
		std::vector<size_t> findings;
		std::unordered_map<NodeId, std::vector<size_t>> rows;
	};

	const SessionView *view_ = nullptr;
	RevisionKey key_;
	size_t count_ = 0;
	const Diagnostic *data_ = nullptr;
	bool made_ = false;
	std::unordered_map<std::string, File> files_;
};

} // namespace opennova::editor
