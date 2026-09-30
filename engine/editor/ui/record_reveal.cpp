#include "record_reveal.h"

#include <algorithm>

#include <editor/session/view/session_view.h>

#include <imgui.h>

namespace opennova::editor {

void RecordReveal::follow(const SessionView &view, const Document &document) {
	moved_ = document.identity() != document_ || view.documents.selection.primary != seen_ ||
	         asked_;
	document_ = document.identity();
	seen_ = view.documents.selection.primary;
	asked_ = false;
	if (!moved_) return;
	path_.clear();
	// The selection is the active document's.
	moved_ = view.documents.selection.primary.row != 0 && view.documents.active == document.path();
	if (!moved_) return;
	path_ = document.ancestors(view.documents.selection.primary);
	path_.push_back(view.documents.selection.primary);
}

bool RecordReveal::holds(const NodeAddress &record) const {
	return moved_ && std::find(path_.begin(), path_.end(), record) != path_.end();
}

void RecordReveal::scroll_to(const NodeAddress &record, bool holder) const {
	if (holds(record) && (holder || record == path_.back()) && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
}

} // namespace opennova::editor
