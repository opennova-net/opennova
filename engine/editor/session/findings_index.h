#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/value.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

// A view's findings by where they are (ADR 0046 S13 V1): by file, and by the record of a file
// a finding is on, made once per change of the findings, so a window that reads one file's or
// one record's (the Inspector's record findings, a stylesheet's status line) never scans every
// finding on every frame. Indices are into the view's diagnostics, in their order.
class FindingsIndex {
public:
	// What the index is kept by: the view's revision, which moves with the findings.
	static uint64_t cache_key(const SessionView &view) { return view.revision; }

	// Made again from the view's findings when the view or its cache key moved since.
	void follow(const SessionView &view);
	// The findings about a file (project-relative).
	const std::vector<size_t> &of_file(const std::string &file) const;
	// The findings on a record of a file: on its row (`child` 0: the row's own and every record's
	// it holds), or on one record it holds.
	std::vector<size_t> of_record(const std::string &file, NodeId row, NodeId child) const;

private:
	const SessionView *view_ = nullptr;
	uint64_t key_ = 0;
	bool made_ = false;
	std::unordered_map<std::string, std::vector<size_t>> files_;
	std::map<std::pair<std::string, NodeId>, std::vector<size_t>> rows_;
};

} // namespace opennova::editor
