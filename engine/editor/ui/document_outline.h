#pragma once

#include <editor/model/document.h>
#include <editor/ui/editor_host.h>
#include <editor/ui/record_reveal.h>

namespace opennova::editor {

// A document's records as a tree, straight from the type's declarations
// (Document::collections): each row, each collection a record holds, and each record by
// the name its type shows (Document::record_title, the token in its tooltip), selected on a
// click, marked when it was added or changed since the last save; a
// collection adds a record at its end (+), and the selected record has Duplicate, Remove,
// Up and Down where its collection is not fixed (the type refuses what its format does
// not allow, and says why in Problems). The rows are a list the same way where the file
// adds rows of their kind (RecordKindRow::add_label: an animation table's rows, one Add per
// kind); a file's other rows are its fixed records. Long collections are clipped, so a model's
// thousands of bullet faces draw only what shows; a deep tree scrolls sideways. The selection
// moved there (a Go to) is revealed: the records and collections holding it open, and it
// scrolls into view (`reveal`, the outline's across frames). The fields are the inspector's.
void draw_document_outline(EditorHost &host, const Document &document, RecordReveal &reveal);

} // namespace opennova::editor
