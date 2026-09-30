#include <editor/session/view/session_view.h>

#include <algorithm>

#include <editor/model/document.h>
#include <editor/project/project_document.h>
#include <editor/session/request_kinds.h>

namespace opennova::editor {

std::string editor_window_title(const SessionView &view) {
	const char *const product = "OpenNova Editor";
	if (!view.project.open) return product;
	const std::vector<std::shared_ptr<const Document>> &open = view.documents.open;
	const bool unsaved = std::any_of(open.begin(), open.end(),
			[](const std::shared_ptr<const Document> &document) { return document->dirty(); });
	// U+25CF BLACK CIRCLE: the OS draws the title, not the editor's ASCII font.
	return view.project.document->title + (unsaved ? " \xE2\x97\x8F - " : " - ") + product;
}

bool SessionView::allows(EditorRequestKind kind) const {
	return !busy_refuses(kind, activity.operation);
}

} // namespace opennova::editor
