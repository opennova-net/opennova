#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/assets/asset_kinds.h>
#include <editor/session/view/viewport_kind.h>

namespace opennova::editor {

class ViewportModel;
struct DocumentsView;

// The viewport kinds' table (ADR 0046 S13 V5; CONTEXT.md "Viewport"): what each kind of
// session/view/viewport_kind.h is to the session, a document's picture as the game would draw it,
// drawn by a device of the Shell's and by a canvas that owns its input, with a state of its own (its
// options, a camera, its device's size) that a SetViewport request changes, and that its follow
// derives from its document (viewport_model.h).

// A document type a viewport kind reads, and how: `shows`, a document of the type is what a
// viewport of the kind draws (its target: a menu, a model, a clip, a table); else the type only
// feeds it, what its documents hold showing in the picture, so the Preview window keeps showing the
// kind while one is active (a menu's stylesheets and string tables).
struct ViewportFeed {
	DocumentTypeId type = DocumentTypeId::None;
	bool shows = false;
};

// One row per ViewportKind, in its order (viewport_kinds.cpp, static_asserted as the request and
// the asset kinds' tables are): where it is drawn; whether it shows the document as the game would
// read it were it saved now (its bytes written and read back, so a document that cannot be written
// shows nothing: the menu's and the model's) or the document as it stands (a text's in the script
// device, S13 V10; the mission's rows, to come); whether it shows one row of its document, the
// selection's (a menu's screen: its target
// moves only when a row of the document is selected and goes with that row); the document types it
// shows and those that feed it; and what makes a viewport of it over the document at a path, its
// state at the kind's defaults. A type is shown by one Main-role kind at most and fed by one
// Preview-role kind at most (static_asserted).
struct ViewportKindRow {
	ViewportKind kind = ViewportKind::kCount;
	ViewportRole role = ViewportRole::Preview;
	bool as_saved = true;
	bool part = false;
	const ViewportFeed *feeds = nullptr;
	size_t feed_count = 0;
	std::unique_ptr<ViewportModel> (*make)(const std::string &path) = nullptr;
};

// A kind's row; Menu's for a value past the last kind.
const ViewportKindRow &viewport_kind_row(ViewportKind kind);
// Whether a viewport of `kind` shows a document of `type` (its feeds' `shows`).
bool viewport_kind_shows(ViewportKind kind, DocumentTypeId type);
// The Preview-role kind a document of `type` shows in or feeds (the Preview window shows it while
// such a document is active: a menu, a stylesheet or a string table the menu's, a model, a clip or
// a table the model's); kCount for none.
ViewportKind preview_kind_of(DocumentTypeId type);
// The Main-role kind that shows a document of `type` (its Document tab's main view: a text type's
// script device, S13 V10); kCount for none.
ViewportKind main_viewport_kind(DocumentTypeId type);
// The kind a document of `type` is read and changed through when none is named (S13 V7: the
// viewport query, a SetViewport, an edit in a viewport): the Preview-role kind that shows it (the
// one the Preview window shows while the document is active), else its Main-role kind; kCount for a
// type no kind shows (a stylesheet feeds the menu's, but shows in none).
ViewportKind default_viewport_kind(DocumentTypeId type);
// The types a viewport shows, in words, from the kinds' table ("a menu, a model, an animation or an
// animation map"): what a refusal of a document that shows in none names.
std::string viewport_shown_types();

// The Preview-role kind the Preview window shows over `documents`' targets: the active document's
// (the kind its type shows in or feeds, preview_kind_of), else `last` (the one it showed; kCount
// before it showed one). When that kind has no target (its document never opened, or closed), the
// first kind that has one; kCount when none has.
ViewportKind preview_kind(const DocumentsView &documents, ViewportKind last);
// What the Preview window follows after a change of the view (the session's touch, before its
// viewports are tracked): of each Preview-role kind, the active document when the kind shows its
// type, a kind that shows a row of it the row the selection lands in, keeping the one it had while
// none is selected; a target whose document closed, or whose row went, cleared; a Main-role kind's
// empty. Then the kind it shows (DocumentsView::preview_shown, preview_kind over the one before).
void update_preview_targets(DocumentsView &documents);

} // namespace opennova::editor
