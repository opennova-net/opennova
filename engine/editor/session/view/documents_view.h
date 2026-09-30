#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/model/value.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

class Document;
class DocumentBase;

// The open documents as the view shows them (ADR 0046 S13 V4; the Documents, DocumentSet,
// ActiveDocument and Selection concerns): each document, the active one, the selection in it,
// the clipboard, and what the previews follow.
struct DocumentsView {
	// The open documents, each as the base: a window that reads rows asks records_of.
	std::vector<std::shared_ptr<const DocumentBase>> open;
	std::string active; // the active document's path ("" = none)
	// The selection in the active document: the primary record (the inspector's, the one
	// a new record goes beside) and every selected record, the primary among them, all
	// inside one row. Repaired after every edit, undo and redo: a record that is gone
	// drops out, a removed primary gives way to its owner, a new record is selected.
	NodeAddress selection;
	std::vector<NodeAddress> selected;
	// What Copy and Cut put on the clipboard: the payload of the document type that made
	// it (Document::copy), which Paste hands back to the same type.
	std::string clipboard;
	// What the menu preview shows: the selected screen (row) of the last menu document a
	// selection landed in. It stays while another document is active (the stylesheet the
	// screen draws with), and clears when that menu closes or the screen is gone.
	struct MenuPreviewTarget {
		std::string path;
		NodeId screen = 0;
	};
	// What the model preview shows (S10p2, S10p6): the last model, clip or animation table
	// document made active (a clip or a table plays on its rig's model). It stays while
	// another document is active, and clears when that document closes.
	struct ModelPreviewTarget {
		std::string path;
	};
	struct Previews {
		MenuPreviewTarget menu;
		ModelPreviewTarget model;
	};
	Previews previews;

	// The selection is `address` alone (none for an empty address).
	void select_only(const NodeAddress &address);
	// SelectRecord in `path` (which becomes the active document): see SelectMode.
	void select(const std::string &path, const NodeAddress &address, SelectMode mode);
	// After an edit that added records to `document`: they are the selection (the first
	// the primary; one another of them holds is left out) and the document is the active
	// one.
	void select_added(const Document &document);
	// After an edit, an undo or a redo of the active `document`: the selected records it no
	// longer has drop out; a primary that is gone gives way to `owner` (the owner the
	// primary had before the edit) when it is still there, else to the last record
	// still selected.
	void repair_selection(const Document &document, const NodeAddress &owner);
	// Follow the active document and the selection (every view change calls it).
	void update_previews();
};

// Whether a selection's records (a view's `selected`) hold `address`: every list and tree of
// records marks a row selected by it. The primary is one of them whenever there is one
// (select_only, select, select_added and repair_selection keep it there), so a row is marked
// exactly when Copy, Cut, Duplicate and Remove take it.
bool holds(const std::vector<NodeAddress> &selected, const NodeAddress &address);

} // namespace opennova::editor
