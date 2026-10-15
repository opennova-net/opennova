#pragma once

#include <cstdint>
#include <vector>

#include <editor/model/document.h>

namespace opennova::editor {

struct SessionView;

// Where a Document tab's view shows the selection when it moves there (a Go to, a find's hit, a
// Problems row, a record picked in another window) or a request asks again to show its field (a
// RevealRecord view event, which the view takes from its mailbox as it draws): the view opens the
// records and the collections that hold it (an outline: OutlineModel::reveal over path()) and
// scrolls its item into view, once, and only when the item is out of view (a click in the list,
// whose item shows, moves nothing). A view keeps one and asks it about each record it draws.
class RecordReveal {
public:
	// A RevealRecord event for the view's document, taken as the view draws: the follow() after it
	// shows the selection again, where it was already too.
	void ask() { asked_ = true; }
	// Once a frame, before the view draws its records: whether the selection moved (in another
	// document too) or was asked for again since the view last drew, and what holds it.
	void follow(const SessionView &view, const Document &document);
	// This frame reveals the selection, and the records that hold it, the row first, then the
	// selection itself (none while it does not).
	bool moved() const { return moved_; }
	const std::vector<NodeAddress> &path() const { return path_; }
	// After the item of `record` drew: scrolled into view (centred) when it is the selection being
	// revealed and out of view. A list of the rows alone passes `holder`: its row holding the
	// selection is shown too.
	void scroll_to(const NodeAddress &record, bool holder = false) const;

private:
	bool holds(const NodeAddress &record) const; // the record is on path_, the selection included

	uint64_t document_ = 0;       // the document the view last drew
	NodeAddress seen_;            // the selection it last drew
	bool asked_ = false;          // a RevealRecord event came since it last drew
	bool moved_ = false;          // this frame reveals the selection
	std::vector<NodeAddress> path_; // the records holding it, the row first, then the selection
};

} // namespace opennova::editor
