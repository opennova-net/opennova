#pragma once

#include <cstddef>
#include <string>

#include <editor/model/document.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// A text field of a record edited in place in a table cell (the string tables, the
// stylesheets), the whole cell with no label (the column's header names it; the id is
// "##<field>"): a coalesced Set while typing, the group closed when the cell lets go. The
// field's schema says what it holds (its capacity in bytes, the terminator included) and
// whether it runs over several lines (a string's text: the cell as tall as its lines).
void text_cell(Workspace &workspace, const Document &document, NodeAddress address, const char *field);

} // namespace opennova::editor
