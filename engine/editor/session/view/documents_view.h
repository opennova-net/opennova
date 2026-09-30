#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/model/value.h>
#include <editor/session/editor_request.h>
#include <editor/session/selection.h>

namespace opennova::editor {

class DocumentBase;

// The open documents as the view shows them (ADR 0046 S13 V4; the Documents, DocumentSet,
// ActiveDocument and Selection concerns): each document, the active one, the selection in it,
// the clipboard, and what the previews follow.
struct DocumentsView {
	// The open documents, each as the base: a window that reads rows asks records_of.
	std::vector<std::shared_ptr<const DocumentBase>> open;
	std::string active; // the active document's path ("" = none)
	// The selection in the active document (S13 D7: its records over any of its rows, the primary
	// among them; selection.document is `active`). Repaired after every edit, undo and redo: a
	// record that is gone drops out, a removed primary gives way to its owner, what an edit made is
	// selected.
	Selection selection;
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

	// Follow the active document and the selection (every view change calls it).
	void update_previews();
};

} // namespace opennova::editor
