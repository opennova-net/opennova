#include "record_reveal.h"

#include <algorithm>

#include <editor/session/view/session_view.h>

#include <imgui.h>

namespace opennova::editor {

void RecordReveal::follow(const SessionView &view, const Document &document) {
	moved_ = document.identity() != document_ || view.documents.selection != seen_ || asked_;
	document_ = document.identity();
	seen_ = view.documents.selection;
	asked_ = false;
	if (!moved_) return;
	path_.clear();
	// The selection is the active document's.
	moved_ = view.documents.selection.row != 0 && view.documents.active == document.path();
	if (!moved_) return;
	path_ = document.ancestors(view.documents.selection);
	path_.push_back(view.documents.selection);
}

bool RecordReveal::holds(const NodeAddress &record) const {
	return moved_ && std::find(path_.begin(), path_.end(), record) != path_.end();
}

void RecordReveal::open_record(const NodeAddress &record) const {
	if (holds(record) && record != path_.back()) ImGui::SetNextItemOpen(true);
}

void RecordReveal::open_collection(const NodeAddress &owner, NodeKind kind) const {
	if (!moved_) return;
	for (size_t i = 0; i + 1 < path_.size(); ++i)
		if (path_[i] == owner && path_[i + 1].kind == kind) {
			ImGui::SetNextItemOpen(true);
			return;
		}
}

int RecordReveal::index_in(const std::vector<NodeId> &ids) const {
	if (!moved_) return -1;
	for (const NodeAddress &record : path_) {
		if (!record.child) continue;
		const auto at = std::find(ids.begin(), ids.end(), record.child);
		if (at != ids.end()) return static_cast<int>(at - ids.begin());
	}
	return -1;
}

void RecordReveal::scroll_to(const NodeAddress &record, bool holder) const {
	if (holds(record) && (holder || record == path_.back()) && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
}

} // namespace opennova::editor
