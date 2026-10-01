#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <editor/assets/asset_kinds.h>

namespace opennova::editor {

class ViewportModel;

// The kinds of viewport (ADR 0046 S13 V5; CONTEXT.md "Viewport"): a document's picture as the
// game would draw it, drawn by a device of the Shell's and by a canvas that owns its input, with
// a state of its own (its options, a camera) that changes only through a SetViewport request. A
// kind is one value here and one row in each of three tables, none of which builds without it:
// viewport_kinds.cpp (what it is to the session), ui/viewport_views.cpp (the view a window draws
// it with) and godot/src/authoring/viewport_devices.cpp (the Shell's device for it).
enum class ViewportKind : uint8_t {
	Menu, // a menu's screen as the game draws it (the Shell's MenuFrame)
	Model, // a model, or a clip or an animation table played on its rig's model (ObjectModel)
	kCount,
};

inline constexpr size_t kViewportKindCount = static_cast<size_t>(ViewportKind::kCount);

// Where a viewport of the kind is drawn: in the Preview window beside the Document tab (the
// menu's and the model's), or as the Document tab's main view, with ImGui overlays, the
// document's outline and the Inspector beside it (ui/document_views' MainViewport role: the
// mission's 3D view, no kind yet).
enum class ViewportRole : uint8_t { Preview, Main };

// A document type a viewport kind reads, and how: `shows`, a document of the type is what a
// viewport of the kind draws (its target: a menu, a model, a clip, a table); else the type only
// feeds it, what its documents hold showing in the picture, so the Preview window keeps showing the
// kind while one is active (a menu's stylesheets and string tables).
struct ViewportFeed {
	DocumentTypeId type = DocumentTypeId::None;
	bool shows = false;
};

// One row per ViewportKind, in its order (viewport_kinds.cpp, static_asserted as the request and
// the asset kinds' tables are): its token on the wire; where it is drawn; whether it shows the
// document as the game would read it were it saved now (its bytes written and read back, so a
// document that cannot be written shows nothing: the menu's and the model's) or the document's
// rows as they stand (the mission's, to come); whether it shows one row of its document, the
// selection's (a menu's screen: its target moves only when a row of the document is selected and
// goes with that row); the document types it shows and those that feed it; and what makes a
// viewport of it over the document at a path, its state at the kind's defaults.
struct ViewportKindRow {
	ViewportKind kind = ViewportKind::kCount;
	const char *token = "";
	ViewportRole role = ViewportRole::Preview;
	bool as_saved = true;
	bool part = false;
	const ViewportFeed *feeds = nullptr;
	size_t feed_count = 0;
	std::unique_ptr<ViewportModel> (*make)(const std::string &path) = nullptr;
};

// A kind's row; Menu's for a value past the last kind.
const ViewportKindRow &viewport_kind_row(ViewportKind kind);
// The kind a wire token names ("menu", "model"); false for none.
bool viewport_kind_from_token(const std::string &token, ViewportKind &out);
inline const char *viewport_kind_token(ViewportKind kind) { return viewport_kind_row(kind).token; }
// Whether a viewport of `kind` shows a document of `type` (its feeds' `shows`).
bool viewport_kind_shows(ViewportKind kind, DocumentTypeId type);
// The Preview-role kind a document of `type` shows in or feeds (the Preview window shows it while
// such a document is active: a menu, a stylesheet or a string table the menu's, a model, a clip or
// a table the model's); kCount for none.
ViewportKind preview_kind_of(DocumentTypeId type);
// The Main-role kind that shows a document of `type` (its Document tab's main view); kCount for
// none (no kind plays the Main role yet).
ViewportKind main_viewport_kind(DocumentTypeId type);

} // namespace opennova::editor
