#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <editor/model/value.h>

namespace opennova::editor {

// What a record document's content was read from (Document::source_state): the source findings of
// that read, and, for a kind whose game reader ends a line at CR LF alone (assets/asset_kinds.h,
// LineReader), the bytes read, decoded, where an LF in them ends a line alone (null for none): what
// the line-ends rule (documents/line_ends.h) reads, and what its fix reads again with every line
// ending CR LF. A load and a save's read-back make it; a step that reads the source again changes it,
// and its undo gives the one before back (EditStep::before_source).
struct SourceState {
	std::vector<SourceIssue> issues;
	std::shared_ptr<const std::string> odd_lines;
};

// The offset of the first LF in `text` that no CR comes before, and how many there are; npos and 0
// for none.
size_t first_lone_lf(std::string_view text, size_t *count = nullptr);
// `text` with every LF that no CR comes before written CR LF, each other byte as it was (a CR alone
// too).
std::string with_crlf_line_ends(std::string_view text);

} // namespace opennova::editor
