#pragma once

#include <memory>

#include <editor/preview/viewport_kinds.h>

namespace opennova::editor {

class ViewportView;

// A viewport kind's view (ADR 0046 S13 V5): one row per ViewportKind, in its order, in
// ui/viewport_views.cpp, which does not build without it (static_asserts, as the document views'
// table): what makes the view a window draws a viewport of the kind with (its toolbar and what it
// draws beside its canvas: ui/menu_viewport_view, ui/model_viewport_view; the script device's rect,
// no canvas: ui/script_viewport_view, S13 V10).
struct ViewportViewRow {
	ViewportKind kind = ViewportKind::kCount;
	std::unique_ptr<ViewportView> (*make)() = nullptr;
};

// A kind's row; null past the last kind.
const ViewportViewRow *viewport_view_row(ViewportKind kind);
// A new view of a viewport of `kind` (null past the last kind).
std::unique_ptr<ViewportView> make_viewport_view(ViewportKind kind);

} // namespace opennova::editor
