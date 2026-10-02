#pragma once

#include <string>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class ViewportModel;
struct ViewportContext;

// A viewport kind's half of a canvas (ADR 0046 S13 V2, V5): what one canvas drawing a viewport of
// the kind keeps between frames (the one gesture machine and what a press or a nudge took) and plans
// each frame over the viewport, every change a request (CanvasRequests: a record selected, a step's
// batch, a gesture's end, the viewport's state); made by the viewport (ViewportModel::make_canvas),
// driven by whoever draws it (ui/viewport_view, a test), always over a viewport of its kind. The
// frame's start reads the viewport (follow); the rest of the frame reads what it made of it then.
// The menu's (preview/menu_canvas) and the model's (preview/model_canvas).
class CanvasHalf {
public:
	virtual ~CanvasHalf() = default;
	virtual const CanvasGesture &gesture() const = 0;
	// The frame's start, while the canvas draws: what the frame maps made once (the selection on the
	// picture, its markers), and a gesture begun on another subject ended (another document, a
	// reload of it, another screen of a menu).
	virtual void follow(
			const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) = 0;
	// The frame's pointer and keys over the picture: a press, its drag, its release; the arrows,
	// Esc and F while the canvas has the keyboard; the wheel the canvas leaves to the kind.
	virtual void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) = 0;
	// The gesture ends (its end raised once when a step went out).
	virtual void end(CanvasRequests &out) = 0;
	// The frame bracket (CanvasGesture::end_frame): a canvas that did not draw ends its gesture.
	virtual void end_frame(CanvasRequests &out) = 0;
	// What is drawn over the picture, the cursor the pointer shows, and the hover tip ("" none).
	virtual OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const = 0;
	virtual CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const = 0;
	virtual std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const = 0;
};

} // namespace opennova::editor
