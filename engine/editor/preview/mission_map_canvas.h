#pragma once

#include <string>
#include <vector>

#include <editor/documents/mission_links.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_link_shapes.h>
#include <editor/preview/mission_map.h>
#include <editor/preview/terrain_brush_canvas.h>

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
// (pick_mission_map_mark); a box the pins in it and the footprints it meets. S24: with the links option, the
// selected event's, trigger's or action's links (preview/mission_link_shapes; a reach is its circle seen from
// above) over the boxes and under the pins, and a badge on each mark some event names, its hover listing them.
// While a pick waits (the workspace's document `pick`), a click on a pin that fills its slot sets it and ends the
// pick, a click on nothing or Esc ends it, and only its candidates hover, each rung. B7: the 3D view's keys, Delete,
// Ctrl+D and the arrows' nudge by the snap (F frames already).
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
	bool brush_ground(const ViewportContext &context, const CanvasInput &in, double &x, double &y) const override;

private:
	// The camera moved to `camera`, raised as a SetViewport once a frame at most.
	void camera_(const MissionMapCamera &camera, CanvasRequests &out);
	// S24 B7, the 3D view's keys on the map: Delete removes the selection (a stop its marker), Ctrl+D copies it a snap
	// step east, the arrows nudge it a snap step (Shift a tenth) on the map's axes, one gesture while an arrow is held.
	void keys_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out);
	// S24: the primary area's edge or corner within the pick slop of (x, y) on a view (Move: none), with the area as
	// a press finds it.
	MissionHandle handle_under_(const ViewportContext &context, const MissionMapView &view, float x, float y,
			MissionPressed &area) const;

	CanvasGesture gesture_;
	TerrainBrushHalf brush_{ViewportKind::Map}; // S25: the terrain brush, while live, before the map's own grammar
	const MissionMapViewport *viewport_ = nullptr; // the frame's, from follow
	std::vector<MissionMapMark> marks_; // the frame's, at its picture's size
	std::vector<int> selected_; // the frame's selected marks
	int primary_ = -1;
	// S24: the selected logic record's links, how many events name each mark (by mark index), the counts kept
	// while the document stands, and the hovered record's uses.
	MissionLinkScene links_;
	std::vector<size_t> badges_;
	MissionLinkCounts counts_;
	mutable MissionLinkHover hover_;
	// S24 pick mode: the slot a pick waits for and what each mark gives it.
	MissionPickFrame pick_;
	// What a press took: the mark under it (-1: a box from nothing), the records it moves as the press found them and
	// the grabbed one's place among them, where it began in the mission, and the box's corners.
	struct Press {
		int mark = -1;
		// S24: the primary area's edge or corner the press took (Move: none), the area as pressed and where the
		// handle stood (mission x and y).
		MissionHandle handle = MissionHandle::Move;
		MissionPressed area;
		double handle_at[2] = { 0.0, 0.0 };
		std::vector<MissionPressed> taken;
		size_t grabbed = 0;
		double from[2] = { 0.0, 0.0 };
		CanvasPoint box_from;
		bool selected = false; // the grabbed record selected before it moved
	};
	Press press_;
	// A nudge: the records as it began, how far it has gone (metres east and north), and the selection it took.
	std::vector<MissionPressed> nudge_;
	std::vector<NodeAddress> nudged_;
	double nudge_by_[2] = { 0.0, 0.0 };
	// A pan by the right or middle button: the camera as it began and where the pointer was.
	bool panning_ = false;
	MissionMapCamera pan_from_;
	CanvasPoint pan_at_;
};

} // namespace opennova::editor
