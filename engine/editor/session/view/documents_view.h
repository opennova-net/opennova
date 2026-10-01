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

// A gesture open in a document (ADR 0046 S13 V8; CONTEXT.md "Gesture"): the document its batches
// changed, the token they carry (Edit::gesture) and when its last batch came (the session's
// milliseconds, steady_clock_ms), from its first batch that changes the document until it ends: any
// other request on that document (its EndEdit, an Undo, a Redo or a Save of it, a select, another
// batch: a request whose row names the document, its own batches alone excepted), a Save All or a
// close that writes or drops the document, every edit group ended (Build, Play, a project opened,
// the imports and the renames), another gesture's batch in the document, or no batch of it for the
// session's gesture deadline (a client that stopped mid-drag). Its edits' validation waits for its
// end, and so does the picture a viewport whose kind holds for a gesture makes again of the document
// (the model's scene: the device keeps the last one meanwhile).
struct OpenGesture {
	std::string path;
	uint64_t token = 0;
	int64_t last_ms = 0;
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
	// The gestures open now, one per document at most (S13 V8): kept by DocumentSet as the gestures'
	// batches and their ends are served and the polls pass their deadline, read by the viewports as
	// they follow (no concern moves with them: the Shell's pump reads them each frame). gesture_in:
	// the one open in the document at `path` (its project-relative path), null for none.
	std::vector<OpenGesture> gestures;
	const OpenGesture *gesture_in(const std::string &path) const {
		for (const OpenGesture &gesture : gestures)
			if (gesture.path == path) return &gesture;
		return nullptr;
	}
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
