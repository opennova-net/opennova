#pragma once

#include <editor/model/document.h>
#include <editor/ui/editor_host.h>

namespace opennova::editor {

// The row a document's tab starts with: Reload, Undo and Redo as a row that wraps in a
// narrow dock (Save is the File menu's, Close the tab's), then the blocked or
// dropped-lines notice. Each acts on this document, whichever is active.
void draw_document_toolbar(EditorHost &host, const Document &document);

} // namespace opennova::editor
