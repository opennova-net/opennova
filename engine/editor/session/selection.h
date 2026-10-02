#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/change_set.h>
#include <editor/model/value.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class Document;

// Whether `document` has the record `address` names, of the kind it says: a row, or a record a row
// holds.
bool has_record(const Document &document, const NodeAddress &address);

// The records selected in one document (ADR 0046 S13 D7): any records of any of its rows (a marquee
// over a mission's entities, windows of several screens of a menu), one of them the primary (the
// Inspector's form, the preview's handles and the place a new or pasted record goes follow it; an
// arrange aligns the others to it), and a serial that takes a value no selection had before with
// every change and every selection put back (restore), so whoever keeps what it made of a
// selection compares one number. The view's selection is the active document's (DocumentsView);
// an open document keeps its own while another is active (DocumentSet).
struct Selection {
	std::string document; // the path of the document the records are in ("" = none)
	NodeAddress primary; // one of `records`; empty when none is selected
	// Each selected record once, in the order selected; changed through the methods below, which
	// keep holds()'s index of it.
	std::vector<NodeAddress> records;
	uint64_t serial = 0;

	bool empty() const { return records.empty(); }
	// Whether `address` is selected: every list and tree of records marks a row selected by it, so
	// a row is marked exactly when Copy, Cut, Duplicate and Remove take it. A binary search of the
	// records in address order (the outline asks it for every line it draws).
	bool holds(const NodeAddress &address) const;
	// `address` alone in `document` (nothing selected for an empty address).
	void select_only(const std::string &document, const NodeAddress &address);
	// SelectRecord's: the records named (`others`, then `primary`) in `document`, by `mode`.
	// Replace, or any mode in another document, makes them the selection; Add joins each not
	// selected; Toggle joins each not selected and leaves each that is. The primary is `primary`
	// when it is named and selected; else, after a Replace, the first named; after an Add the last
	// named; after a Toggle the primary it had while it stays, else the last record still selected.
	// Nothing named selects nothing.
	void select(const std::string &document, const NodeAddress &primary,
			const std::vector<NodeAddress> &others, SelectMode mode);
	// After an edit that made records in `document` (last_added_records): they are the selection,
	// one that another of them holds left out (a window, not the ACTIONs made with it), the first
	// the primary.
	void select_added(const Document &document);
	// `address`, one of the records, the primary (nothing when it is not selected).
	void make_primary(const NodeAddress &address);
	// `kept` put back (a document made active again, a selection kept through a reload): its
	// document, primary and records, under a serial no selection had.
	void restore(const Selection &kept);
	// After an edit, an undo or a redo of `document`: the selected records it no longer has drop
	// out; a primary that is gone gives way to `owner` (the owner it had before the edit) when that
	// is still there, else to the last record still selected. `changes` (the document's changes
	// since the state the selection was repaired against, DocumentBase::changes_since) says where
	// to look: a record of a removed row drops out unasked, one of a changed row is asked for, one
	// of any other row stays unasked; null (the document could not say) asks for every record.
	// Returns how many records it asked the document for.
	size_t repair(const Document &document, const ChangeSet *changes, const NodeAddress &owner);

private:
	// The records in address order again, and a serial no selection had.
	void changed();
	std::vector<NodeAddress> sorted_; // `records` in address order: holds()'s index
};

} // namespace opennova::editor
