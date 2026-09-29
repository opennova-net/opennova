#pragma once

#include <cstdint>
#include <vector>

#include <editor/model/document.h>

namespace opennova::editor {

struct SessionView;

// Where a Document tab's view shows the selection when it moves there (a Go to, a find's hit, a
// Problems row, a record picked in another window) or a request asks again to show its field
// (the view's reveal_serial): the view opens the records and the collections that hold it and
// scrolls its item into view, once, and only when the item is out of view (a click in the list,
// whose item shows, moves nothing). A view keeps one and asks it about each record it draws.
class RecordReveal {
public:
	// Once a frame, before the view draws its records: whether the selection moved (in another
	// document too) or was asked for again since the view last drew, and what holds it.
	void follow(const SessionView &view, const Document &document);
	// Before the tree node of `record` (a row or a nested record) draws: it opens when it holds
	// the selection being revealed.
	void open_record(const NodeAddress &record) const;
	// Before the node of `owner`'s collection of records of `kind` draws: it opens when it holds
	// the selection being revealed.
	void open_collection(const NodeAddress &owner, NodeKind kind) const;
	// The index among a clipped list's `ids` that must draw (the selection, or the record there
	// holding it) while it is revealed; -1 for none.
	int index_in(const std::vector<NodeId> &ids) const;
	// After the item of `record` drew: scrolled into view (centred) when it is the selection being
	// revealed and out of view. A list of the rows alone passes `holder`: its row holding the
	// selection is shown too.
	void scroll_to(const NodeAddress &record, bool holder = false) const;

private:
	bool holds(const NodeAddress &record) const; // the record is on path_, the selection included

	uint64_t document_ = 0;       // the document the view last drew
	NodeAddress seen_;            // the selection it last drew
	uint64_t serial_ = 0;         // the view's reveal_serial it last drew
	bool moved_ = false;          // this frame reveals the selection
	std::vector<NodeAddress> path_; // the records holding it, the row first, then the selection
};

} // namespace opennova::editor
