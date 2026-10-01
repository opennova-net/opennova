#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/session/view/view_events.h>
#include <editor/ui/outline_model.h>
#include <editor/ui/view_event_mailbox.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

class DocumentBase;

// What fills a document's tab (ADR 0046 S13 V3, decision 11 as S13 amends it).
enum class DocumentViewRole {
	// The view draws the document's records: an outline (a tree, a list of the rows, master and
	// detail), or a view of its own (a stylesheet's lines, a menu's screens and windows).
	Records,
	// A Main-role viewport fills the tab, the records' outline (the row's OutlineSpec) and the
	// Inspector beside it (the mission's 3D view; ui/main_viewport_view): main_viewport is its hook,
	// drawing the viewport of the Main-role kind that shows the type (S13 V5); no type has one yet.
	MainViewport,
	// The view draws the document's text (S13 D9: its lines, read only, its findings in the
	// gutter, ui/text_view), until S13 V10's script device (a Godot CodeEdit) fills the tab.
	Text,
};

// A document's view in its Document tab (ADR 0046 S13 V3; CONTEXT.md "Document view"): made from
// its type's row (make_view) the first time the Document window draws the document's tab as the
// active document's or sends it a RevealRecord, one per open document by its path, and kept with
// all it holds (its filter and order, what it has open, its caches, the events it is sent) while
// the document is open there; the document read again (another instance at the path, or the same
// one loaded again) is rebound to it, and closed, the view goes with it. A renamed file's
// document is another instance at another path (the session closes it and opens the new path):
// its view is a new one.
class DocumentView {
public:
	virtual ~DocumentView() = default;

	// The tab's content over `document`, the one open at the view's path (a record document's views
	// read records_of): its toolbar, then what it shows of the document. It takes the RevealRecord
	// events it holds as it draws (the selection shown again).
	virtual void draw(Workspace &workspace, const DocumentBase &document) = 0;
	// The view's own modals (a menu's Remove screen?), which the workspace draws every frame
	// whether the tab shows or not: a modal no frame draws would hold the input.
	virtual void draw_modals(Workspace &workspace) { (void)workspace; }
	// The document at the view's path was read again (another instance, or the same one loaded
	// again): what the view keeps that names the old records by identity goes (the reveal, a
	// range's anchor, the cell being edited); its filter, its order and what it has open stay. The
	// events it held went before (drop_events).
	virtual void rebind(const DocumentBase &document) { (void)document; }
	// The Main-role viewport that fills the tab where the view's row's role is MainViewport, drawn
	// in the room the tab gives it: true when it drew one (ui/main_viewport_view, S13 V5: the
	// viewport's view, its canvas filling the room, with ImGui overlays); a view of records has none.
	virtual bool main_viewport(Workspace &workspace, const DocumentBase &document) {
		(void)workspace;
		(void)document;
		return false;
	}
	// After every frame's windows (the workspace's frame bracket): a view with a viewport whose canvas
	// did not draw this frame (its tab hidden, the window closed) ends that canvas's gesture.
	virtual void end_frame(Workspace &workspace) { (void)workspace; }
	// An outline's model (its row's OutlineSpec): its lines, its filter, its order and what is open,
	// which its filter box, its sort and its arrows read and write; null for a view of its own.
	virtual OutlineModel *outline() { return nullptr; }

	// A RevealRecord or RevealText event for its document (EditorWindows::begin_frame through the
	// Document window), held until the view draws; how many it holds.
	void receive(const ViewEvent &event) { events_.post(event); }
	size_t held_events() const { return events_.held(); }
	// The events held for a document read again go: they name its old records.
	void drop_events() { events_.take(); }

protected:
	// The events held since the view last drew, now the view's: it takes them as it draws.
	std::vector<ViewEvent> take_events() { return events_.take(); }

private:
	ViewEventMailbox<> events_;
};

// A document type's view (ADR 0046 S13 V3): one row per DocumentTypeId past None, in its order,
// in ui/document_views.cpp, which does not build without it (static_asserts): its role, and the
// view it makes: an outline in the mode its OutlineSpec gives (ui/outline_view; for a MainViewport
// row, the outline beside the viewport, ui/main_viewport_view), or a view of its own, which its
// make makes (one of the two).
struct DocumentViewRow {
	DocumentTypeId type = DocumentTypeId::None;
	DocumentViewRole role = DocumentViewRole::Records;
	const OutlineSpec *outline = nullptr;
	std::unique_ptr<DocumentView> (*make)() = nullptr;
};

// A type's row; null for None and past the last.
const DocumentViewRow *document_view_row(DocumentTypeId type);
// A new view of `document` by its type (the row of the document column of its kind's row); null
// for a document no type opens.
std::unique_ptr<DocumentView> make_view(const DocumentBase &document);

} // namespace opennova::editor
