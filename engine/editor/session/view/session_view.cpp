#include <editor/session/view/session_view.h>

#include <algorithm>

#include <editor/model/document.h>
#include <editor/project/project_document.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_operation.h>

namespace opennova::editor {

const DocumentBase *DocumentsView::open_at(const std::string &path) const {
	for (const std::shared_ptr<const DocumentBase> &document : open)
		if (document && document->path() == path) return document.get();
	return nullptr;
}

std::string editor_window_title(const SessionView &view) {
	const char *const product = "OpenNova Editor";
	if (!view.project.open) return product;
	const std::vector<std::shared_ptr<const DocumentBase>> &open = view.documents.open;
	const bool unsaved = std::any_of(open.begin(), open.end(),
			[](const std::shared_ptr<const DocumentBase> &document) { return document->unsaved(); });
	// U+25CF BLACK CIRCLE: the OS draws the title, not the editor's ASCII font.
	return view.project.document->title + (unsaved ? " \xE2\x97\x8F - " : " - ") + product;
}

bool SessionView::allows(EditorRequestKind kind) const {
	return !busy_refuses(kind, activity.operation);
}

std::string update_refusal(const SessionView &view) {
	const std::vector<std::shared_ptr<const DocumentBase>> &open = view.documents.open;
	if (std::any_of(open.begin(), open.end(),
				[](const std::shared_ptr<const DocumentBase> &document) { return document->unsaved(); }))
		return "Save or discard the unsaved changes first: the update quits the editor.";
	if (view.activity.play_state != PlayState::Stopped)
		return "Stop the game Play started first: the update quits the editor.";
	if (!view.allows(EditorRequestKind::Quit))
		return std::string("Wait for ") + operation_kind_row(view.activity.operation.kind).noun +
		       " to finish: the update quits the editor.";
	return std::string();
}

} // namespace opennova::editor
