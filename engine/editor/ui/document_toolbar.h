#pragma once

#include <editor/model/document_base.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// The row a document's tab starts with: Reload, Undo, Redo and Used by as a row that wraps in a
// narrow dock (Save is the File menu's, Close the tab's), then what a save changes on the row (its words
// in the tooltip) or the blocked notice on one line. Each acts on this document, whichever is active, of
// any kind.
// `blocked_notice`: what a blocked document's view says of it in place of the input notice (a
// text document read only: a shipped file its text form cannot carry, nothing to correct).
void draw_document_toolbar(Workspace &workspace, const DocumentBase &document,
                           const char *blocked_notice = nullptr);

} // namespace opennova::editor
