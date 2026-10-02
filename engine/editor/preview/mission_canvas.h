#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class Document;
class MissionViewport;
class ViewportDevice;

// The mission viewport's canvas (ADR 0046 S14): what the viewport marks over the mission
// (preview/mission_overlay), what a press on the picture takes, and the camera's gestures. Every
// change is a request: a record selected (a click, Shift adding, Ctrl toggling; a marquee's records
// in one SelectRecord), a batch a sample of a drag (the mark under the press moved on the ground with
// every selected record, one not selected selected alone first; the primary's height and yaw
// handles; an area's edges), one undo step a gesture; the camera's state (a look with the right
// button, flown with W A S D Q E while it is held; a pan with the middle button; an orbit with Alt;
// the wheel's dolly toward the ground under the pointer; F and a double click framing), a look, a fly
// and a wheel in one frame one SetViewport. The arrows nudge the selection a snap step along the
// file's axis nearest the camera's right and forward, PgUp and PgDn its height; Delete removes the
// selected records; Esc selects nothing.

// What the canvas maps, one frame's worth.
struct MissionCanvasFrame {
	const Document *document = nullptr; // the mission document shown (null: none)
	const MissionViewport *viewport = nullptr;
	std::vector<MissionMark> marks; // the picture's marks now
	int primary = -1; // the primary record's mark, while the mission is the active document
	std::vector<int> selected; // the other selected records' marks
	std::vector<NodeAddress> records; // every selected record of the document (Delete's, a path's line)
	bool current = false; // the picture shows the document as it is now
	bool editable = true; // edits may be raised (ViewportContext::editable)
	float snap = 0.0f; // a move, a lift and an edge snap to this, metres (Alt: free)
	float turn_snap = 0.0f; // a turn snaps to this, degrees
	const ViewportDevice *device = nullptr; // the ground
};

// What a press on the canvas took.
struct MissionGrab {
	enum class What : uint8_t {
		None, // a mark a click selects (joined by the keys), nothing a drag moves
		Pan, // the middle button
		Orbit, // Alt on nothing
		Marquee, // on nothing: a box
		Handle, // a mark's handle: a drag writes its record
	};
	What what = What::None;
	MissionHandle handle = MissionHandle::Move;
	int pick = -1; // the mark under the press (-1 none)
	CanvasJoin join = CanvasJoin::Replace;
	// The records the drag takes, as pressed, and the grabbed one's place among them; whether the
	// grabbed one was not selected (selected alone as the drag begins).
	std::vector<MissionPressed> pressed;
	size_t grabbed = 0;
	bool select_first = false;
	CanvasPoint from, to; // a marquee's corners, the picture's pixels
	CanvasPoint offset; // from the pointer to the handle's pixel
	PreviewVec3 through; // where the handle stood as pressed (a lift's plane)
	double ground[3] = { 0.0, 0.0, 0.0 }; // where the press met the ground (a move's, an edge's)
	bool grounded = false;
};
// The front-most mark within the pick slop of the pointer (-1: none, or not hovered).
int mission_canvas_under(const MissionCanvasFrame &frame, const CanvasInput &in);
MissionGrab mission_canvas_grab(const MissionCanvasFrame &frame, const ViewportContext &context,
		const CanvasInput &in, int under);

class MissionCanvas final : public CanvasHalf {
public:
	const CanvasGesture &gesture() const override { return gesture_; }
	const MissionCanvasFrame &frame() const { return frame_; }
	const MissionGrab &grab() const { return grab_; }
	// The right button is held on the picture: a look.
	bool looking() const { return looking_; }

	void follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	void end(CanvasRequests &out) override;
	void end_frame(CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const override;
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

	// The camera on the selected marks, else on everything (F, a double click, the toolbar's Frame).
	void frame_selected(int width, int height, CanvasRequests &out) const;

private:
	void keys_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out);
	// One sample of a handle's drag: its batch planned from the records as pressed.
	void drag_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out);
	void release_(CanvasRequests &out);
	// The selection moved a step east, north and up (the arrows, PgUp and PgDn): a nudge's batch,
	// planned from the records as the nudge began.
	void nudge_by_(double east, double north, double up, CanvasRequests &out);

	CanvasGesture gesture_;
	MissionCanvasFrame frame_;
	MissionGrab grab_;
	bool looking_ = false;
	// A nudge: the records as it began, and how far it has gone.
	std::vector<MissionPressed> nudge_;
	std::vector<NodeAddress> nudged_;
	double nudge_east_ = 0.0, nudge_north_ = 0.0, nudge_up_ = 0.0;
};

} // namespace opennova::editor
