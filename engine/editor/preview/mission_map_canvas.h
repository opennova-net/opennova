#pragma once

#include <string>
#include <vector>

#include <editor/preview/canvas_half.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_map.h>

namespace opennova::editor {

// The 2D map's half of a canvas (ADR 0046 S23 C; preview/mission_map.h): the mission's records as pins over the
// device's picture of the commander map, picked and moved as the 3D view's marks are. A click selects the pin under it
// (Shift adds, Ctrl toggles; a click of nothing clears the selection), a box dragged from nothing selects what it
// holds (one SelectRecord), a drag of a pin moves it and, when it is selected, the selection with it: the 3D view's
// move (mission_move_edits from the records as the press found them, stick over the device's ground), each sample one
// batch under one gesture, so a drag is one undo step. The right or the middle button pans, the wheel zooms by the
// CMAP's step about the pointer, F frames the selection (else everything); each a SetViewport of the camera. Drawn
// (over the device's picture, which draws the models' wireframes): each path's line through its stops (the player's
// route in the light blue the game's map colours team 1 with, its stops numbered; a red team's route in the salmon),
// each area's box, each entity with no wireframe its pin by its pool ringed in its team's map colour, the hovered and
// the selected rings (a wireframe's: its footprint outlined), the labels (the hovered and the selected, every shown
// mark's with the labels option), the box being dragged. A press takes a pin, else the model under it seen from above
// (pick_mission_map_mark); a box the pins in it and the footprints it meets.
class MissionMapCanvas final : public CanvasHalf {
public:
	const CanvasGesture &gesture() const override { return gesture_; }
	void follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	void end(CanvasRequests &out) override;
	void end_frame(CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const override;
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

private:
	// The camera moved to `camera`, raised as a SetViewport once a frame at most.
	void camera_(const MissionMapCamera &camera, CanvasRequests &out);

	CanvasGesture gesture_;
	const MissionMapViewport *viewport_ = nullptr; // the frame's, from follow
	std::vector<MissionMapMark> marks_; // the frame's, at its picture's size
	std::vector<int> selected_; // the frame's selected marks
	int primary_ = -1;
	// What a press took: the mark under it (-1: a box from nothing), the records it moves as the press found them and
	// the grabbed one's place among them, where it began in the mission, and the box's corners.
	struct Press {
		int mark = -1;
		std::vector<MissionPressed> taken;
		size_t grabbed = 0;
		double from[2] = { 0.0, 0.0 };
		CanvasPoint box_from;
		bool selected = false; // the grabbed record selected before it moved
	};
	Press press_;
	// A pan by the right or middle button: the camera as it began and where the pointer was.
	bool panning_ = false;
	MissionMapCamera pan_from_;
	CanvasPoint pan_at_;
};

} // namespace opennova::editor
