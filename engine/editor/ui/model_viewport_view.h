#pragma once

#include <memory>

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// A model viewport's view (ADR 0046 S10p3, S11d, S13 V2, V5; ui/viewport_views' model row): the
// model as the game draws it, rebuilt on every edit of what it draws, at the level the game would
// pick at the camera's distance (Auto) or a level held; its CTRL registers held at a value; when
// there is nothing to draw it says why. A toolbar and a clip's timeline around one canvas
// (preview/model_canvas, the model viewport's half), whose picture fills it. Over the picture it
// marks the user points (and their Z axes), the lights (their reach, a spot's axis) and, when
// asked, the part pivots, each where the game puts it on the posed model; a click on a marker
// selects its record, the selected records' markers are ringed. A drag of the selected marker moves
// its record (and every selected marker as far) in the plane that faces the eye (snapped to the grid
// the toolbar picks, Alt for free), a drag of its axis tip turns the axis; each drag is one undo
// step. The left button elsewhere drags to orbit, the middle button (or Shift with the left) pans,
// the wheel dollies, F or a double-click frames the selected marker (else the model): each a
// SetViewport of the camera. The preview clock runs or holds the part animations (Run / Pause:
// Play is the game's); the toolbar wraps whole controls in a narrow window. A clip or an animation
// table plays on its rig's model (the one an item pairs with the table, or one the author picks):
// the selected row's clip runs on the timeline under the picture (run, hold, step a tick, scrub),
// its trigger events marked on the ticks the clip first samples them; a click on one seeks there and
// selects the event in the clip's document.
class ModelViewportView final : public ViewportView {
public:
	ModelViewportView();
	~ModelViewportView() override;

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;
	void draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) override;

private:
	struct Tools;
	std::unique_ptr<Tools> tools_;
};

} // namespace opennova::editor
