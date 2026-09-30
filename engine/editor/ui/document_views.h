#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/session/view/view_events.h>
#include <editor/ui/view_event_mailbox.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

class DocumentBase;

// What fills a document's tab (ADR 0046 S13 V3, decision 11 as S13 amends it).
enum class DocumentViewRole {
	// The view draws the document's records: an outline (a tree, a list of the rows, master and
	// detail), or a view of its own (a stylesheet's lines, a menu's screens and windows).
	Records,
	// A Main-role viewport fills the tab, the records' outline and the Inspector beside it (the
	// mission's 3D view): main_viewport is its hook, which S13 V5's viewport seam fills; no type
	// has one yet.
	MainViewport,
};

// A document's view in its Document tab (ADR 0046 S13 V3; CONTEXT.md "Document view"): made from
// its type's row (make_view) the first time the Document window meets the open document, one per
// open document by its path, and kept with all it holds (its filter and order, what it has open,
// its caches, the events it is sent) while the document is open there; the document read again
// (another instance at the path) is rebound to it, and closed, the view goes with it.
class DocumentView {
public:
	virtual ~DocumentView() = default;

	// Its row's role (DocumentViewRow::role).
	virtual DocumentViewRole role() const { return DocumentViewRole::Records; }
	// The tab's content over `document`, the one open at the view's path (a record document's views
	// read records_of): its toolbar, then what it shows of the document. It takes the RevealRecord
	// events it holds as it draws (the selection shown again).
	virtual void draw(Workspace &workspace, const DocumentBase &document) = 0;
	// The view's own modals (a menu's Remove screen?), which the workspace draws every frame
	// whether the tab shows or not: a modal no frame draws would hold the input.
	virtual void draw_modals(Workspace &workspace) { (void)workspace; }
	// The document at the view's path is another instance (read again): what the view keeps that
	// names the old one's records by identity goes (the reveal, a range's anchor); its filter, its
	// order and what it has open stay.
	virtual void rebind(const DocumentBase &document) { (void)document; }
	// The Main-role viewport that fills the tab where the view's role is MainViewport, drawn in the
	// room the tab gives it: true when it drew one. The hook S13 V5's viewport seam fills (a
	// mission's 3D view, with ImGui overlays); every view draws its records itself until then.
	virtual bool main_viewport(Workspace &workspace, const DocumentBase &document) {
		(void)workspace;
		(void)document;
		return false;
	}

	// A RevealRecord event for its document (EditorWindows::begin_frame through the Document
	// window), held until the view draws; how many it holds.
	void receive(const ViewEvent &event) { events_.post(event); }
	size_t held_events() const { return events_.held(); }

protected:
	// The events held since the view last drew, now the view's: it takes them as it draws.
	std::vector<ViewEvent> take_events() { return events_.take(); }

private:
	ViewEventMailbox<> events_;
};

// A document type's view (ADR 0046 S13 V3): one row per DocumentTypeId past None, in its order,
// in ui/document_views.cpp, which does not build without it (static_asserts): its role and how
// its view is made (an outline in its mode, or a view of its own).
struct DocumentViewRow {
	DocumentTypeId type = DocumentTypeId::None;
	DocumentViewRole role = DocumentViewRole::Records;
	std::unique_ptr<DocumentView> (*make)() = nullptr;
};

// A type's row; null for None and past the last.
const DocumentViewRow *document_view_row(DocumentTypeId type);
// A new view of `document` by its type (the row of the document column of its kind's row); null
// for a document no type opens.
std::unique_ptr<DocumentView> make_view(const DocumentBase &document);

} // namespace opennova::editor
