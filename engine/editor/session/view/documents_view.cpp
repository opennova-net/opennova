#include <editor/session/view/documents_view.h>

#include <editor/assets/asset_kinds.h>
#include <editor/model/document.h>

namespace opennova::editor {

void DocumentsView::update_previews() {
	const DocumentBase *shown = nullptr;
	for (const auto &document : open)
		if (document && document->path() == active) shown = document.get();
	for (size_t i = 0; i < kViewportKindCount; ++i) {
		const auto kind = static_cast<ViewportKind>(i);
		const ViewportKindRow &row = viewport_kind_row(kind);
		PreviewTarget &target = previews[kind];
		if (row.role != ViewportRole::Preview) {
			target = PreviewTarget();
			continue;
		}
		// The active document, when the kind shows its type: a kind that shows a row of it follows
		// the row a selection lands in, and keeps the one it had while none is selected.
		if (shown && viewport_kind_shows(kind, asset_kind_row(shown->kind()).document)) {
			if (!row.part) target = {shown->path(), 0};
			else if (selection.primary.row) target = {shown->path(), selection.primary.row};
		}
		// A target whose document closed, or whose row is gone, clears.
		const DocumentBase *document = nullptr;
		for (const auto &open_document : open)
			if (open_document && open_document->path() == target.path) document = open_document.get();
		const Document *records = document ? records_of(*document) : nullptr;
		if (!document || (row.part && (!records || !records->row(target.part))))
			target = PreviewTarget();
	}
}

} // namespace opennova::editor
