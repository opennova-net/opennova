#pragma once

#include <editor/preview/model_canvas.h>
#include <editor/preview/model_preview_state.h>
#include <editor/ui/workspace.h>
#include <editor/ui/viewport_canvas.h>

namespace opennova::editor {

namespace ui_kit {
class WrapRow;
}

// The only seam between the engine-owned model pane and a rendering device (ADR 0046 d11,
// the GameViewport pattern): the shell renders the previewed model through the runtime's
// own ObjectModel into an offscreen viewport and draws it into the pane; engine-only runs
// leave it null. The pane moves the portable half's camera and options, the device
// follows them.
class ModelPreviewViewport {
public:
	virtual ~ModelPreviewViewport() = default;
	// What the device shows and how (the status, the options, the camera).
	virtual ModelPreviewModel &model() = 0;
	// Size the offscreen viewport to the device size and draw its texture as the current
	// ImGui item (it renders on the frames it is drawn).
	virtual void draw(int device_width, int device_height) = 0;
};

// The model preview, the Preview window's model pane (ADR 0046 S10p3, S11d, S13 V2): the
// model the view previews as the game draws it, rebuilt on every edit of what it draws, at the
// level the game would pick at the camera's distance (Auto) or a level held; its CTRL registers
// held at a value; when there is nothing to draw it says why. A toolbar and a clip's timeline
// around one canvas (ui/viewport_canvas over preview/model_canvas). Over the picture it marks
// the user points (and their Z axes), the lights (their reach, a spot's axis) and, when
// asked, the part pivots, each where the game puts it on the posed model; a click on a
// marker selects its record, the selected record's marker is ringed. A drag of the
// selected marker moves its record in the plane that faces the eye (snapped to the grid
// the toolbar picks, Alt for free), a drag of its axis tip turns the axis; each drag is
// one undo step. The left button elsewhere drags to orbit, the middle button (or Shift
// with the left) pans, the wheel dollies, F or a double-click frames the selected marker
// (else the model). The clock runs or holds the part animations (Run / Pause: Play is the
// game's); the toolbar wraps whole controls in a narrow window. A clip or an animation
// table plays on its rig's model (the one an item pairs with the table, or one the author
// picks): the selected row's clip runs on the timeline under the picture (run, hold, step
// a tick, scrub), its trigger events marked on the ticks the clip first samples them; a
// click on one seeks there and selects the event in the clip's document.
class ModelPreviewPane {
public:
	explicit ModelPreviewPane(Workspace &workspace) : workspace_(workspace), requests_(workspace) {}
	// Into the current window, below the Preview window's line naming the model.
	void draw();
	// After every frame's windows (the workspace's frame bracket): a canvas that did not draw
	// this frame (the menu pane shown, the window closed or hidden, nothing to show) ends its
	// drag, a marker's end raised once for the model it began in.
	void end_frame() {
		model_canvas_.end_frame(requests_);
		canvas_.end_frame();
	}

private:
	void toolbar_(ModelPreviewModel &model, const ModelCanvasFrame &frame);
	void registers_(ModelPreviewModel &model);
	void draw_canvas_(const ModelCanvasFrame &frame, float available_height);
	void rig_chooser_(ui_kit::WrapRow &row, ModelPreviewModel &model);
	void timeline_(ModelPreviewModel &model);

	// The Shell's model device (the workspace's devices), null for none.
	ModelPreviewViewport *viewport() const { return workspace_.devices().model; }

	Workspace &workspace_;
	ViewportCanvas canvas_;
	ModelCanvas model_canvas_;
	CanvasWindowRequests requests_;
	int snap_ = 2; // kModelHandleSnaps: 1/16 m
};

} // namespace opennova::editor
