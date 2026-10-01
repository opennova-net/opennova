#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/value.h>
#include <editor/session/editor_request.h>
#include <editor/session/selection.h>
#include <editor/session/view/viewport_kind.h>

namespace opennova::editor {

class DocumentBase;
class Viewports;

// What the Preview window shows of a viewport kind (ADR 0046 S13 V5; the kinds are
// session/view/viewport_kind.h's, so the view holds no preview header): the document of the kind it
// follows, and for a kind that shows one row of it (ViewportKindRow::part) the row, a menu's
// screen.
struct PreviewTarget {
	std::string path;
	NodeId part = 0;
};

// The gesture open in a document (ADR 0046 S13 V8; CONTEXT.md "Gesture"): the document its batches
// changed and the token they carry (Edit::gesture), from its first batch that changes the document
// until it ends: an EndEdit of that document (let go, or its canvas not drawn), an Undo or a Redo, a
// Save, every edit group ended (Build, Play, the unsaved-changes prompt), its document closed or
// another gesture changing a document. Its edits' validation waits for its end, and so does a
// viewport's picture of the document made again (the last picture kept meanwhile). Token 0: none
// open.
struct OpenGesture {
	std::string path;
	uint64_t token = 0;
	bool open() const { return token != 0; }
	// Open in the document at `document` (its project-relative path).
	bool in(const std::string &document) const { return open() && path == document; }
};

// One target per ViewportKind (a Main-role kind's stays empty: its view is the Document tab's).
struct PreviewTargets {
	std::array<PreviewTarget, kViewportKindCount> targets;
	PreviewTarget &operator[](ViewportKind kind) { return targets[static_cast<size_t>(kind)]; }
	const PreviewTarget &operator[](ViewportKind kind) const {
		return targets[static_cast<size_t>(kind)];
	}
};

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
	// The gesture open now, if any (S13 V8): kept by DocumentSet as the gesture's batches and its
	// end are served, read by the viewports as they follow (no concern moves with it: the Shell's
	// pump reads it each frame).
	OpenGesture gesture;
	// What the Preview window follows of each viewport kind (S13 V5): the last document of a type
	// the kind shows made active (a model, a clip or an animation table for the model's), and for a
	// kind that shows one row of it the row the selection last landed in there (a menu's screen: set
	// when a record of the menu is selected). A target stays while another document is active (the
	// stylesheet the menu's screen draws with), and clears when its document closes or its row goes.
	PreviewTargets previews;
	// The Preview-role kind the Preview window shows (S13 V5; preview/viewport_kinds' preview_kind):
	// the active document's (the kind its type shows in or feeds), else the one it showed before;
	// when that kind has no target, the first kind that has one; kCount when none has.
	ViewportKind preview_shown = ViewportKind::kCount;
	// The session's viewports (preview/viewports.h), each a document's picture with its state, and
	// the preview clock: shared const, the windows reading what a viewport shows and changing it
	// only by a request (SetViewport). Made with the session (null only in a view no session made).
	// The previews (their targets and the kind shown) follow the active document and the selection
	// at every change of the view (preview/viewport_kinds' update_preview_targets, the session's
	// touch).
	std::shared_ptr<const Viewports> viewports;
};

} // namespace opennova::editor
