#pragma once

#include <memory>

namespace opennova::editor {

class Workspace;

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
// click on one seeks there and selects the event in the clip's document. What it holds (its
// canvas, the model's half of it, the snap) is its own, behind a pointer, so this header names
// none of the preview's or the canvas's types (S13 V4); the device it draws through is the
// Shell's (preview/model_preview_viewport.h, the workspace's devices).
class ModelPreviewPane {
public:
	explicit ModelPreviewPane(Workspace &workspace);
	~ModelPreviewPane();
	ModelPreviewPane(const ModelPreviewPane &) = delete;
	ModelPreviewPane &operator=(const ModelPreviewPane &) = delete;
	// Into the current window, below the Preview window's line naming the model.
	void draw();
	// After every frame's windows (the workspace's frame bracket): a canvas that did not draw
	// this frame (the menu pane shown, the window closed or hidden, nothing to show) ends its
	// drag, a marker's end raised once for the model it began in.
	void end_frame();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::editor
