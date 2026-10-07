#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mnu_clipboard.h>
#include <editor/model/edit.h>
#include <editor/preview/canvas_gesture.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/viewport_overlay.h>
#include <formats/mnu/mnu.h>
#include <runtime/menu/menu_frame.h>

namespace opennova::editor {

class MnuDocument;
struct Node;

// The menu viewport's canvas (ADR 0046 S9k1, S9k2, S13 V2, V5): what a press on the screen's
// picture takes, what a drag or the arrow keys write, what a box selects, and what is drawn over the
// picture, from the viewport's headless compile of the screen (the game's own hit test and rects,
// which the Shell's device draws alike). The rect math is the runtime's; the writes are the layout
// planner's (menu_layout_edit) and the arrange's (menu_arrange). An editor authoring aid, not a port.

// What the canvas maps, one frame's worth: the menu shown and its screen, the selection on it,
// and the viewport's compiled screen, which maps a point to a window only while it is of the
// document's own revision.
struct MenuCanvasFrame {
	const MnuDocument *document = nullptr;
	const Node *screen = nullptr;
	NodeAddress record; // the primary record while the menu is the active document (none: not)
	// Every selected record, the session's, borrowed for the frame (null: none).
	const std::vector<NodeAddress> *selected = nullptr;
	NodeAddress primary; // the window of the screen holding the primary record (none: none)
	std::vector<NodeAddress> windows; // every selected window of the screen, the primary among them
	// Each of `windows`' pre-order index in the compiled screen (-1: none), and the primary's.
	std::vector<int> indexes;
	int primary_index = -1;
	const menu::MenuFrameCompiler *compiler = nullptr;
	const menu::MenuFrameState *state = nullptr;
	bool current = false; // the picture shows the document's revision: only then does it map
	// The viewport is in Try mode (DI-35): the picture's clicks and keys are the game's menu's, so the
	// canvas takes no press, draws nothing over the picture and shows no pointer of its own.
	bool trying = false;
	bool snap = true; // a drag snaps its moved edges to kLayoutGrid (Alt: free)
	// Edits may be raised (S13 A3: false while an operation holds the documents, as
	// SessionView::allows(EditRecord) says; ViewportContext::editable): else a press selects (a click,
	// a marquee) and moves or resizes nothing, and the arrows and an arrange write nothing.
	bool editable = true;
	// The compiler's notes on the screen as it stands (the viewport's, null: none).
	const std::vector<menu::MenuFrameNote> *notes = nullptr;
};

// The frame's selection, the session's while the menu is the active document (none while it
// is not): the primary record `primary`, every selected record (`selected`, which must outlive the
// frame), the window of the screen holding the primary, every selected window of the screen, and
// where each sits in the screen's pre-order, each found once a frame.
void menu_canvas_select(MenuCanvasFrame &frame, const NodeAddress &primary,
		const std::vector<NodeAddress> &selected);
// What Copy, Cut, Duplicate and Paste take of the frame's selection: the menu clipboard's one
// rule (documents/mnu_clipboard), the tree's; `full` while the session's clipboard holds records.
MenuClipboard menu_canvas_clipboard(const MenuCanvasFrame &frame, bool full);

// The primary window's eight handles: squares kMenuHandleSize across that take a press
// kMenuHandleSlop past their edge; inside the window a handle reaches no further than a quarter
// of it across, so the middle of a window a few pixels wide still moves it. Corners first: where
// handles overlap (a small window), a corner takes the press.
inline constexpr float kMenuHandleSize = 6.0f;
inline constexpr float kMenuHandleSlop = 4.0f;
inline constexpr LayoutHandle kMenuHandles[] = { LayoutHandle::TopLeft, LayoutHandle::TopRight,
	LayoutHandle::BottomLeft, LayoutHandle::BottomRight, LayoutHandle::Left, LayoutHandle::Right,
	LayoutHandle::Top, LayoutHandle::Bottom };

// A design-space rect on the picture, scaled `scale_x` x `scale_y`: its corners `a` (top left)
// and `b` (bottom right) on the pixels the game draws it on (menu::menu_scaled_edge).
void menu_picture_rect(
		const mnu::RectEdges &rect, float scale_x, float scale_y, CanvasPoint &a, CanvasPoint &b);
// Where a handle of the rect a..b sits.
CanvasPoint menu_handle_point(LayoutHandle handle, CanvasPoint a, CanvasPoint b);
// The handle a press at `at` takes on the rect a..b; false for none.
bool menu_handle_at(CanvasPoint at, CanvasPoint a, CanvasPoint b, LayoutHandle &out);
// What the pointer shows over a handle (Move: over the window itself).
CanvasCursor menu_handle_cursor(LayoutHandle handle);

// What a left press on the canvas took. With Shift or Ctrl the selection changes on release
// and nothing moves (a press on the screen's background a marquee). Otherwise: on a handle of
// the primary window a resize of it; inside the primary or another selected window (its rect,
// whatever window the hit test finds there) a move of every selected one; on the screen's
// background (nothing, or a root window not selected) a marquee; elsewhere the window the
// game's hit test found, which a drag moves alone. A stale picture maps nothing.
struct MenuPress {
	bool resize = false; // on a handle of the primary window
	bool marquee = false; // on the screen's background: a drag selects what its box touches
	int pick = -1; // the widget the game's hit test found there (-1 none)
	CanvasJoin join = CanvasJoin::Replace;
	float scale_x = 1.0f, scale_y = 1.0f; // the picture's scale at the press
	CanvasPoint from, to; // the marquee's box, design units
	NodeAddress window; // what a drag moves or resizes (none: a click only)
	LayoutHandle handle = LayoutHandle::Move;
	LayoutPress layout; // where the drag began: the windows it takes
	int dx = 0, dy = 0, grid = -1; // the last step planned
};
MenuPress menu_canvas_press(const MenuCanvasFrame &frame, const CanvasInput &in);

// The selected window whose rect holds the picture point `at`: the primary when it does, else
// the front-most one (the last in the compiled screen's pre-order, the order the runtime draws
// in); none when no selected window's rect holds it.
NodeAddress menu_selected_window_at(
		const MenuCanvasFrame &frame, CanvasPoint at, float scale_x, float scale_y);
// The window the game's hit test finds under the pointer (none: none, or a stale picture).
NodeAddress menu_window_at(const MenuCanvasFrame &frame, const CanvasInput &in);
// Every shown window of the screen but its root windows (the background a marquee starts on)
// whose rect the box `from`..`to` (design units) touches, in the screen's order.
std::vector<NodeAddress> menu_marquee_windows(
		const MenuCanvasFrame &frame, CanvasPoint from, CanvasPoint to);

// Esc: what holds the primary record selected (the primary window's owner: its parent window,
// or the screen). False when the selection is not a record of this screen.
bool menu_canvas_escape(const MenuCanvasFrame &frame, CanvasRequests &out);
// One arrange of the selected windows (align, distribute, drawing order), one batch; nothing on
// a stale picture, on a frame that may raise no edit (`editable` false) or when arrange_edits
// refuses.
void menu_canvas_arrange(const MenuCanvasFrame &frame, ArrangeOp op, CanvasRequests &out);

// The canvas's gestures on a menu viewport (its CanvasHalf): a press, the drag it becomes and its
// release; the arrow keys (1 unit, 8 with Shift) moving the selected windows while one is held; and
// what the canvas draws and shows while they last. Over a frame it is given (a test's), or the one
// it makes of the viewport at each frame's start (MenuViewport::canvas_frame).
class MenuCanvas final : public CanvasHalf {
public:
	const CanvasGesture &gesture() const override { return gesture_; }
	// The frame it made of the viewport at the frame's start.
	const MenuCanvasFrame &frame() const { return frame_; }

	// CanvasHalf, over the frame made of the viewport (a MenuViewport) at follow.
	void follow(const ViewportModel &viewport, const ViewportContext &context,
			CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const override;
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

	// The frame's start, while the pane draws its canvas: a gesture begun on another subject
	// ends (another menu, a reload of it, another of its screens), and a nudge ends when the
	// selected windows change.
	void follow(const MenuCanvasFrame &frame, CanvasRequests &out);
	// The frame's keys and pointer. The keys, while the canvas has the keyboard and no press is
	// down: Esc selects what holds the primary; an arrow moves the selected windows 1 unit (8
	// with Shift), one gesture while one is held, ended when none is or the keyboard goes
	// elsewhere. The pointer: a press, then each sample of its drag (a move of the windows it
	// took, planned from where they began and by the pointer's travel on the screen, or the
	// marquee's box), then its release when the left button comes up (a click selects the window
	// the hit test found, joining as Shift or Ctrl say; a marquee selects what its box touches,
	// the screen when nothing).
	void input(const MenuCanvasFrame &frame, const CanvasInput &in, CanvasRequests &out);
	// The gesture ends.
	void end(CanvasRequests &out) override;
	// The frame bracket (CanvasGesture::end_frame).
	void end_frame(CanvasRequests &out) override;

	// Over the picture: a mark on every noted window, the window under the pointer, the other
	// selected windows, the marquee's box, and the primary window with its eight handles.
	OverlayList shapes(const MenuCanvasFrame &frame, const CanvasInput &in) const;
	// What the pointer shows: the handle it holds or is over, a move inside a selected window.
	CanvasCursor cursor(const MenuCanvasFrame &frame, const CanvasInput &in) const;
	// The window under the pointer: its name, type, rects and notes ("" none, or while pressing).
	std::string hover_tip(const MenuCanvasFrame &frame, const CanvasInput &in) const;

private:
	void keys_(const MenuCanvasFrame &frame, const CanvasInput &in, CanvasRequests &out);
	// The selected windows moved (dx, dy) more, a nudge begun when none is open.
	void nudge_by_(const MenuCanvasFrame &frame, int dx, int dy, CanvasRequests &out);
	void move_(const MenuCanvasFrame &frame, const CanvasInput &in, CanvasRequests &out);
	void release_(const MenuCanvasFrame &frame, CanvasRequests &out);

	CanvasGesture gesture_;
	MenuCanvasFrame frame_;
	MenuPress press_;
	// The nudge: the windows it moves, where they began, and how far in all.
	std::vector<NodeAddress> nudged_;
	LayoutPress nudge_;
	int nudge_dx_ = 0, nudge_dy_ = 0;
};

} // namespace opennova::editor
