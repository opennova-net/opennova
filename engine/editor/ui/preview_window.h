#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_kinds.h>
#include <editor/ui/workspace.h>
#include <runtime/devtools/imgui_pass.h>

namespace opennova::editor {

class ViewportView;
struct SessionView;

// The Preview-role viewport kind the Preview window shows (S13 V5): the active document's (the kind
// its type shows in or feeds, viewport_kinds.h's preview_kind_of), else the one it showed last
// (`last`; kCount before it showed one). When that kind has nothing to show (its document never
// opened, or closed), the first kind that has a target does; kCount when none has.
ViewportKind preview_kind(const SessionView &view, ViewportKind last);

// The Preview window (ADR 0046 S11d, S13 V5), beside Document: the viewport of the kind
// preview_kind picks over the document its target names (DocumentsView::previews), so a
// stylesheet's or a string table's tab keeps showing the menu screen it feeds and a catalog's tab
// keeps what was shown. A line above names what it shows ("main.mnu - STARTUP", "skinned.3di",
// "walk.bad on skinned.3di"), marked while that file has unsaved changes; with nothing to show it
// says what to open. A viewport view per (document, kind) it shows (ui/viewport_views), kept with
// its state (its canvas's zoom, its snap, its gesture) while the viewport is kept (its document
// open): each draws its viewport's device's picture on a canvas of its own, and a canvas that does
// not draw a frame ends the drag or the nudge in progress on it, its gesture's end raised once for
// the document it began in: when the window shows another viewport, when it has nothing to show,
// and when the window itself does not draw (closed, collapsed, its tab hidden), which end_frame()
// catches. The window never takes the focus, and the Shell's devices follow their viewports whether
// the window shows them or not.
class PreviewWindow : public devtools::Window {
public:
	explicit PreviewWindow(Workspace &workspace);
	~PreviewWindow() override;
	const char *title() const override { return "Preview"; }
	devtools::InitialDockPlacement initial_dock_placement() const override { return devtools::InitialDockPlacement::CenterRight; }
	devtools::MenuGroup menu_group() const override { return devtools::MenuGroup::Workspace; }
	void draw(devtools::ImGuiPass &pass, uint64_t frame_index) override;
	// After each frame's windows, whether this one drew or not (the workspace's frame
	// bracket): a viewport view whose canvas did not draw this frame ends its gesture.
	void end_frame();

	// The kind the window last showed (kCount before it showed one).
	ViewportKind shown() const { return shown_; }
	// The view of the viewport of `kind` over `path`, null until the window shows it.
	ViewportView *view_of(const std::string &path, ViewportKind kind);

private:
	struct Slot {
		std::string path;
		ViewportKind kind = ViewportKind::kCount;
		std::unique_ptr<ViewportView> view;
	};
	void header_(const SessionView &view, ViewportKind kind, const std::string &path);
	ViewportView &view_for_(const std::string &path, ViewportKind kind);
	// The views of viewports no longer kept go.
	void prune_(const SessionView &view);

	Workspace &workspace_;
	std::vector<Slot> views_;
	ViewportKind shown_ = ViewportKind::kCount;
};

} // namespace opennova::editor
