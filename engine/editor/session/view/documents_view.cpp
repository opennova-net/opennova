#include <editor/session/view/documents_view.h>

#include <editor/model/document.h>

namespace opennova::editor {

void DocumentsView::update_previews() {
	for (const auto &document : open) {
		if (!document || document->path() != active) continue;
		if (document->kind() == AssetKind::Menu && selection.primary.row) {
			previews.menu.path = document->path();
			previews.menu.screen = selection.primary.row;
		}
		if (document->kind() == AssetKind::Model || document->kind() == AssetKind::Animation ||
		    document->kind() == AssetKind::AnimationMap)
			previews.model.path = document->path();
		break;
	}
	const Document *menu = nullptr;
	bool model_open = false;
	for (const auto &document : open) {
		if (document && document->path() == previews.menu.path) menu = records_of(*document);
		model_open = model_open || (document && document->path() == previews.model.path);
	}
	if (!menu || !menu->row(previews.menu.screen)) previews.menu = MenuPreviewTarget();
	if (!model_open) previews.model = ModelPreviewTarget();
}

} // namespace opennova::editor
