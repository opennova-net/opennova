#pragma once

#include <functional>
#include <memory>
#include <string>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/ui/viewport_canvas.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

class CanvasHalf;
class DocumentBase;
class ViewportModel;
struct ViewportContext;

// A viewport drawn in the room a window gives it (ADR 0046 S13 V5; CONTEXT.md "Viewport"): the
// kind's tools (above: a toolbar; below: a menu's notes, a model's timeline) around one canvas that
// fills the rest, the viewport's device's picture under the kind's overlays, the canvas owning the
// input over it. Every change it makes is a request: a SetViewport of the viewport's state (its
// options, its camera, the clock, the size its device draws at), an EditRecord batch for a drag, a
// SelectRecord for a click. One per (document, kind), made from the kind's row of ui/viewport_views:
// the Preview window's for its targets (a Preview-role viewport), and a Main-role document view's
// (ui/document_views' MainViewport role: the mission's 3D view). The viewport is the view's, read
// const (DocumentsView::viewports); one the session keeps none of yet (its device made at the next
// pump) shows its kind's message.
class ViewportView {
public:
	virtual ~ViewportView();
	ViewportView(const ViewportView &) = delete;
	ViewportView &operator=(const ViewportView &) = delete;

	ViewportKind kind() const { return kind_; }
	// Into the current window's room: the viewport of the view's kind over the document at `path`.
	void draw(Workspace &workspace, const std::string &path);
	// After every frame's windows (the workspace's frame bracket): a canvas that did not draw this
	// frame (another viewport shown, the window closed or hidden, nothing to show) ends its gesture,
	// its end raised once for the document it began in.
	void end_frame(Workspace &workspace);

protected:
	ViewportView(ViewportKind kind);

	// The kind's: everything it draws of a viewport that shows its picture, the canvas (canvas())
	// in the room it leaves for it; and what it says of one that does not (its message, and what the
	// author can do about it: a model's rig chooser).
	virtual void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) = 0;
	virtual void draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path);

	// The canvas `height` tall across the room: the device's picture of the viewport under the
	// kind's shapes, the frame's pointer and keys to the kind's half of it (the canvas's gestures),
	// `inside` drawn once the canvas has read the frame's input (a menu's mouse readout and its
	// right-click menu). A picture that fills the canvas (a model's) sets the size its device draws
	// at to the canvas's (a SetViewport when it differs). The context's size is the picture's.
	void canvas(Workspace &workspace, const ViewportModel &model, ViewportContext &context, float height,
			const std::function<void(const CanvasInput &)> &inside = nullptr);
	// The canvas (its zoom: a design picture's) and its kind's half, made of the viewport (its
	// layout, ViewportModel::make_canvas) at the first frame that draws one.
	ViewportCanvas &canvas_ui() { return *canvas_; }
	CanvasHalf *half() { return half_.get(); }
	// The grid a drag snaps to (the kind's toolbar sets it): the context's.
	float snap = 0.0f;

private:
	ViewportKind kind_;
	std::unique_ptr<ViewportCanvas> canvas_;
	std::unique_ptr<CanvasHalf> half_;
	std::string path_; // the document the last frame drew
};

} // namespace opennova::editor
