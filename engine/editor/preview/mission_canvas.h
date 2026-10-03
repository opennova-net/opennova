#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/graph/display_names.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/mission_handle_edit.h>
#include <editor/preview/mission_hint.h>
#include <editor/preview/mission_label_picks.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class Document;
class MissionViewport;
class ViewportDevice;

// The mission viewport's canvas (ADR 0046 S14, S15): what the viewport marks over the mission
// (preview/mission_overlay), what a press on the picture takes, and the camera's gestures. Every
// change is a request: a record selected (a click, Shift adding, Ctrl toggling; a marquee's records
// in one SelectRecord), a batch a sample of a drag (the mark under the press moved on the ground with
// every selected record, one not selected selected alone first; the primary's height and yaw
// handles, several selected (areas with them) turning about their group's centre, the turn the
// pointer's bearing about it; an area's edges; Ctrl held, from the press or as it goes, free of the
// snap), one undo step a gesture; an Alt-drag of a mark copies what it would move, the copies
// placed where it is let go (one `duplicate` command, one undo step); the camera's state (a look with
// the right button, flown with W A S D Q E while it is held; a pan with the middle button; an orbit
// with Alt on nothing; the wheel's dolly toward the ground under the pointer; F and a double click
// framing), a look, a fly and a wheel in one frame one SetViewport. The arrows nudge the selection a
// snap step along the file's axis nearest the camera's right and forward (Shift a tenth of it), PgUp
// and PgDn its height; Ctrl+D duplicates it a step to the right of the camera; Delete removes the
// selected records; Esc selects nothing, and while a press is down cancels it (nothing its release
// would raise, what its drag wrote put back). Its tools (the viewport's options' MissionTool, which the
// toolbar and the wire set): Place (each click places the picked item), Path (each click adds the
// picked path's next stop), Area (a box dragged on the ground makes an area trigger), each an
// EditInViewport drop the viewport plans.

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
	std::string not_editable; // why not
	float snap = 0.0f; // a move, a lift and an edge snap to this, metres (Ctrl: free)
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
		Area, // the Area tool: a box on the ground
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
	// An Alt-drag: the drag copies what it takes (nothing is written until it is let go), and how far
	// the copies go so far, metres east and north.
	bool copy = false;
	double copy_by[2] = { 0.0, 0.0 };
	CanvasPoint from, to; // a marquee's or an area box's corners, the picture's pixels
	CanvasPoint offset; // from the pointer to the handle's pixel
	PreviewVec3 through; // where the handle stood as pressed (a lift's plane)
	double ground[3] = { 0.0, 0.0, 0.0 }; // where the press met the ground (a move's, an edge's)
	bool grounded = false;
};
// The front-most mark within the pick slop of the pointer (-1: none, or not hovered).
int mission_canvas_under(const MissionCanvasFrame &frame, const CanvasInput &in);
// The primary's handle within the pick slop of the pointer (its height, its yaw, an area's edges),
// while the mission takes edits: false for none.
bool mission_canvas_handle_under(const MissionCanvasFrame &frame, const CanvasInput &in, MissionHandle &out);
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
	// What the line under the picture says now (mission_hint.h): what the tool does, what a click or a
	// drag would do where the pointer is.
	std::string hint(const ViewportContext &context, const CanvasInput &in) const;

	// The camera on the selected marks, else on everything (F, a double click, the toolbar's Frame).
	void frame_selected(int width, int height, CanvasRequests &out) const;
	// The tool (ADR 0046 S14, S15), the viewport's options' as the frame began (mission_options.h): Place
	// with an item picked, a click on the picture placing one of it there (an EditInViewport drop of
	// the item, which the viewport plans) instead of selecting; Path with a path picked, a click adding
	// its next stop there; Area, a box dragged on the ground making an area trigger. A press under a
	// tool takes no mark and draws no marquee; Alt still orbits and the middle button pans. Place with
	// no item picked, or Path with no path, places nothing.
	MissionTool tool() const { return tool_; }
	// The item a click places (0: none), and its name as the catalogs define it; the path a click adds
	// a stop to (0: none).
	int64_t place() const { return tool_ == MissionTool::Place ? item_ : 0; }
	const std::string &place_name() const { return item_name_; }
	int path() const { return tool_ == MissionTool::Path ? path_ : 0; }
	// The Turn snap (the toolbar's): a turned entity's heading snaps to this, degrees (0: whole
	// degrees; Ctrl turns freely); negative, the frame's own (15 while the snap is on, else 0).
	void set_turn(float degrees) { turn_ = degrees; }
	// The way one step goes to the camera's right on the file's nearest axis (Ctrl+D's offset, the
	// right arrow's nudge), metres east and north.
	void step_right(double &east, double &north) const;

private:
	void keys_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out);
	// One sample of a handle's drag: its batch planned from the records as pressed.
	void drag_(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out);
	void release_(CanvasRequests &out);
	// Esc while a press is down: nothing its release would raise, what its drag wrote put back.
	void cancel_(CanvasRequests &out);
	// The selection moved a step east, north and up (the arrows, PgUp and PgDn): a nudge's batch,
	// planned from the records as the nudge began.
	void nudge_by_(double east, double north, double up, CanvasRequests &out);
	// The step a nudge goes: the snap's (a metre when free), a tenth of it with Shift; Ctrl+D's copy goes
	// the whole step (Ctrl+Shift+D is no duplicate).
	float step_(const CanvasInput &in) const;

	CanvasGesture gesture_;
	MissionCanvasFrame frame_;
	MissionGrab grab_;
	bool looking_ = false;
	// A nudge: the records as it began, and how far it has gone.
	std::vector<MissionPressed> nudge_;
	std::vector<NodeAddress> nudged_;
	double nudge_east_ = 0.0, nudge_north_ = 0.0, nudge_up_ = 0.0;
	// The marks' titles by the project's names (the labels, the hint), kept while the document and the
	// graph stand, and while a drag of a handle writes (the polish: it moves marks, which no title reads);
	// the labels' last layout, laid out again only when what it read moved.
	mutable DisplayNameCache titles_;
	mutable MissionLabelLayout labels_;
	MissionTool tool_ = MissionTool::Select;
	int64_t item_ = 0;
	std::string item_name_;
	int path_ = 0;
	float turn_ = -1.0f;
};

} // namespace opennova::editor
