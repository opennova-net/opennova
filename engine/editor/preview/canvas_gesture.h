#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <editor/session/editor_request.h>

namespace opennova::editor {

// The pixels a press moves from where it began before it is a drag; less is a click.
inline constexpr float kDragThreshold = 3.0f;

// A point on a canvas: in the picture's pixels from its top-left corner, or on the screen.
struct CanvasPoint {
	float x = 0.0f;
	float y = 0.0f;
};

// The modifier keys held: a press joins the selection by them, Alt places freely.
struct CanvasKeys {
	bool shift = false;
	bool ctrl = false;
	bool alt = false;
};

// How a click or a box joins the selection: Ctrl adds or drops a record, Shift adds it, a
// plain press replaces the selection.
enum class CanvasJoin : uint8_t { Replace, Add, Toggle };
CanvasJoin canvas_join(const CanvasKeys &keys);
// The SelectRecord mode a join is.
SelectMode select_mode(CanvasJoin join);

// The keys a canvas acts on in one frame. `focused`: the canvas's window has the keyboard and no
// text field takes it (the rest reads nothing otherwise). The arrows pressed this frame, their key
// repeat included, -1, 0 or 1 on each axis, and whether one is held down; Esc and F pressed. The
// keys a camera flies by, held, -1, 0 or 1 on each of its axes (A and D: left and right; Q and E:
// down and up; S and W: back and forward), Shift held with them (fast); Delete pressed; PgUp and
// PgDn pressed, their key repeat included (1 up, -1 down).
struct CanvasKeyboard {
	bool focused = false;
	int arrow_x = 0;
	int arrow_y = 0;
	bool arrow_held = false;
	bool escape = false;
	bool frame = false; // F
	int move_x = 0;
	int move_y = 0;
	int move_z = 0;
	bool fast = false;
	bool remove = false; // Delete
	int page = 0;
};

// One frame of the pointer and the keyboard over a canvas's picture: the editor's canvas reads it
// from Dear ImGui (ui/viewport_canvas), a test writes it.
struct CanvasInput {
	// The picture's size, device pixels.
	int width = 0;
	int height = 0;
	CanvasPoint mouse; // the pointer, in the picture's pixels
	// The pointer on the screen: a press's travel is measured there, so a zoom, a scroll or a refit
	// while the button is down is no travel.
	CanvasPoint screen;
	CanvasPoint delta; // how far it moved since the last frame
	bool hovered = false; // over the canvas, nothing in front of it
	// The canvas pans its picture itself: a design picture's middle button, or Space.
	bool panning = false;
	// A button went down on the canvas this frame (the left one, or the middle one), and
	// whether it is still down.
	bool pressed = false;
	bool middle = false;
	bool down = false;
	// The right button went down on a picture that fills its canvas this frame, and whether it is
	// still down (a camera's look; a design picture's right button is its menu's alone).
	bool right_pressed = false;
	bool right_down = false;
	bool double_clicked = false; // the left button clicked twice
	float wheel = 0.0f; // the wheel's notches the canvas leaves to its kind
	float dt = 0.0f; // the frame's time, seconds (what a held key moves a camera by)
	CanvasKeys keys;
	CanvasKeyboard keyboard;
};

// What a canvas shows, which a gesture on it edits: a document (its path, and which instance of it:
// a reload is another) and the part of it drawn (a menu's screen row; 0 the whole document). A
// gesture begun on one subject ends when the canvas shows another.
struct CanvasSubject {
	std::string path;
	uint64_t identity = 0;
	NodeId part = 0;
};
inline bool operator==(const CanvasSubject &a, const CanvasSubject &b) {
	return a.identity == b.identity && a.part == b.part && a.path == b.path;
}
inline bool operator!=(const CanvasSubject &a, const CanvasSubject &b) {
	return !(a == b);
}

// What a canvas asks of the session (ADR 0046 S13 V2, V5): every change a request, raised by the
// editor's windows as a window request (ui/viewport_canvas), recorded by a test: a record selected,
// joining the selection as Shift or Ctrl say (SelectRecord, a marquee's records with it); a batch of
// the document over any rows, a step of a gesture (its edits carrying the gesture's token) or an
// arrange (EditRecord); the gesture's end (EndEdit), its steps one undo step; the viewport's state
// (SetViewport: a camera orbited, panned, dollied or framed).
class CanvasRequests {
public:
	virtual ~CanvasRequests() = default;
	virtual void request(EditorRequest request) = 0;
};

// The one gesture machine of a canvas (ADR 0046 S13 V2): a button pressed on the picture,
// which becomes a drag once the pointer travels kDragThreshold on the screen from where it
// began, or an arrow key held (a nudge). The steps it sends carry one token (next_edit_gesture),
// so a gesture is one undo step, and when it ends (let go, lost, the canvas not drawn, another
// subject shown) its end is raised once, for the document it began in, when a step went out;
// nothing after. One gesture at a time: a press ends a nudge, a nudge waits for the button.
class CanvasGesture {
public:
	enum class Mode : uint8_t { None, Press, Nudge };

	Mode mode() const { return mode_; }
	bool pressed() const { return mode_ == Mode::Press; }
	bool nudging() const { return mode_ == Mode::Nudge; }
	// The press moved past the threshold.
	bool dragging() const { return dragging_; }
	// Where the press began, on the screen, and the pointer's travel since, to the screen point
	// `at`.
	CanvasPoint from() const { return from_; }
	CanvasPoint travel(CanvasPoint at) const {
		return CanvasPoint{ at.x - from_.x, at.y - from_.y };
	}
	// What the gesture began on, and its document's path ("" none).
	const CanvasSubject &subject() const { return subject_; }
	const std::string &path() const { return subject_.path; }

	// A button pressed at the screen point `at` over `subject`; the gesture that was open ends.
	void press(const CanvasSubject &subject, CanvasPoint at, CanvasRequests &out);
	// The pointer at the screen point `at` while the button is down: true on the sample that makes
	// the press a drag.
	bool move(CanvasPoint at);
	// An arrow key held over `subject`: a nudge; the gesture that was open ends.
	void nudge(const CanvasSubject &subject, CanvasRequests &out);
	// The token the gesture's steps carry, made at the first ask.
	uint64_t token();
	// A step went out: the gesture's end is raised when it ends.
	void sent() { sent_ = true; }
	// The button let go: true for a click (a press that never became a drag). The gesture ends.
	bool release(CanvasRequests &out);
	// The gesture ends: its end raised once, for its document, when a step went out.
	void end(CanvasRequests &out);

	// The frame bracket. frame(): the canvas draws this frame, showing `subject` (a gesture begun
	// on another subject ends: another document, a reload of it, another screen of a menu).
	// end_frame(), after every frame's windows: a canvas that did not draw (hidden, closed, the
	// other pane shown, nothing to show) ends its gesture.
	void frame(const CanvasSubject &subject, CanvasRequests &out);
	void end_frame(CanvasRequests &out);

private:
	Mode mode_ = Mode::None;
	bool dragging_ = false;
	bool sent_ = false;
	bool drawn_ = false;
	CanvasPoint from_;
	uint64_t token_ = 0;
	CanvasSubject subject_;
};

} // namespace opennova::editor
