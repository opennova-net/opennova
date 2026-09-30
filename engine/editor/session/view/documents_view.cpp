#include <editor/session/view/documents_view.h>

#include <algorithm>

#include <editor/model/document.h>

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

bool holds(const std::vector<NodeAddress> &selected, const NodeAddress &address) {
	return std::find(selected.begin(), selected.end(), address) != selected.end();
}

void DocumentsView::select_only(const NodeAddress &address) {
	selection = address;
	selected.clear();
	if (address.row) selected.push_back(address);
}

void DocumentsView::select(const std::string &path, const NodeAddress &address, SelectMode mode) {
	// Joining a selection stays inside its document and row; anything else starts over.
	const bool joins = mode != SelectMode::Replace && path == active && address.row &&
			selection.row == address.row;
	active = path;
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

void DocumentsView::select_added(const Document &document) {
	active = document.path();
	selected.clear();
	for (const NodeId id : document.last_added_records()) {
		const NodeAddress address = document.address_of(id);
		if (address.row) selected.push_back(address);
	}
	// A batch that fills in what it adds (a window and its ACTIONs) selects the window.
	selected = document.outermost(selected);
	selection = selected.empty() ? NodeAddress() : selected.front();
}

void DocumentsView::update_previews() {
	for (const auto &document : open) {
		if (!document || document->path() != active) continue;
		if (document->kind() == AssetKind::Menu && selection.row) {
			previews.menu.path = document->path();
			previews.menu.screen = selection.row;
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

void DocumentsView::repair_selection(const Document &document, const NodeAddress &owner) {
	if (document.path() != active) return;
	std::vector<NodeAddress> kept;
	for (const NodeAddress &address : selected)
		if (record_exists(document, address) &&
				std::find(kept.begin(), kept.end(), address) == kept.end())
			kept.push_back(address);
	if (record_exists(document, selection)) {
		selected = kept;
	} else if (record_exists(document, owner)) {
		return select_only(owner);
	} else {
		selected = kept;
		selection = kept.empty() ? NodeAddress() : kept.back();
	}
	if (selection.row && std::find(selected.begin(), selected.end(), selection) == selected.end())
		selected.push_back(selection);
}

} // namespace opennova::editor
