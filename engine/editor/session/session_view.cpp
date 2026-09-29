#include <editor/session/session_view.h>

#include <algorithm>

namespace opennova::editor {

namespace {

// True while the document has the record the address names, of that kind.
bool record_exists(const Document &document, const NodeAddress &address) {
	if (!address.row) return false;
	if (!address.child) {
		const Node *row = document.row(address.row);
		return row && row->kind == address.kind;
	}
	return document.address_of(address.child) == address;
}

} // namespace

std::string editor_window_title(const SessionView &view) {
	const char *const product = "OpenNova Editor";
	if (!view.project_open) return product;
	const bool unsaved = std::any_of(view.documents.begin(), view.documents.end(),
	                                 [](const std::shared_ptr<const Document> &document) { return document->dirty(); });
	// U+25CF BLACK CIRCLE: the OS draws the title, not the editor's ASCII font.
	return view.document.title + (unsaved ? " \xE2\x97\x8F - " : " - ") + product;
}

void SessionView::select_only(const NodeAddress &address) {
	selection = address;
	selected.clear();
	if (address.row) selected.push_back(address);
	reveal_field.clear();
}

void SessionView::select(const std::string &path, const NodeAddress &address, SelectMode mode) {
	reveal_field.clear();
	// Joining a selection stays inside its document and row; anything else starts over.
	const bool joins = mode != SelectMode::Replace && path == active_document && address.row && selection.row == address.row;
	active_document = path;
	if (!joins) return select_only(address);
	const auto found = std::find(selected.begin(), selected.end(), address);
	if (mode == SelectMode::Toggle && found != selected.end()) {
		selected.erase(found);
		if (selection == address) selection = selected.empty() ? NodeAddress() : selected.back();
		return;
	}
	if (found == selected.end()) selected.push_back(address);
	selection = address;
}

void SessionView::select_added(const Document &document) {
	active_document = document.path();
	selected.clear();
	reveal_field.clear();
	for (const NodeId id : document.last_added_records()) {
		const NodeAddress address = document.address_of(id);
		if (address.row) selected.push_back(address);
	}
	// A batch that fills in what it adds (a window and its ACTIONs) selects the window.
	selected = document.outermost(selected);
	selection = selected.empty() ? NodeAddress() : selected.front();
}

void SessionView::update_previews() {
	for (const auto &document : documents) {
		if (!document || document->path() != active_document) continue;
		if (document->kind() == AssetKind::Menu && selection.row) {
			menu_preview.path = document->path();
			menu_preview.screen = selection.row;
		}
		if (document->kind() == AssetKind::Model || document->kind() == AssetKind::Animation ||
		    document->kind() == AssetKind::AnimationMap)
			model_preview.path = document->path();
		break;
	}
	const Document *menu = nullptr;
	bool model_open = false;
	for (const auto &document : documents) {
		if (document && document->path() == menu_preview.path) menu = document.get();
		model_open = model_open || (document && document->path() == model_preview.path);
	}
	if (!menu || !menu->row(menu_preview.screen)) menu_preview = MenuPreviewTarget();
	if (!model_open) model_preview = ModelPreviewTarget();
}

void SessionView::repair_selection(const Document &document, const NodeAddress &owner) {
	if (document.path() != active_document) return;
	std::vector<NodeAddress> kept;
	for (const NodeAddress &address : selected)
		if (record_exists(document, address) && std::find(kept.begin(), kept.end(), address) == kept.end()) kept.push_back(address);
	if (record_exists(document, selection)) {
		selected = kept;
	} else if (record_exists(document, owner)) {
		return select_only(owner);
	} else {
		selected = kept;
		selection = kept.empty() ? NodeAddress() : kept.back();
		reveal_field.clear(); // the record it was on is gone
	}
	if (selection.row && std::find(selected.begin(), selected.end(), selection) == selected.end()) selected.push_back(selection);
}

} // namespace opennova::editor
