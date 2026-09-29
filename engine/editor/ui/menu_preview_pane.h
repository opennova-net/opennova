#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/value.h>
#include <editor/preview/menu_arrange.h>
#include <editor/preview/menu_layout_edit.h>
#include <editor/preview/menu_preview_state.h>
#include <editor/session/editor_request.h>
#include <editor/ui/editor_host.h>
#include <runtime/menu/menu_frame.h>

namespace opennova::editor {

class MnuDocument;
struct Node;

// The only seam between the engine-owned menu pane and a rendering device (ADR 0046 d11,
// the GameViewport pattern): the shell renders the previewed screen through the runtime's
// own MenuFrame into an offscreen viewport and draws it into the pane; engine-only runs
// leave it null. Picking, outlines and drags read the configured compiler, so they are
// the game's own hit test and rects.
class MenuPreviewViewport {
public:
	virtual ~MenuPreviewViewport() = default;
	// What the device shows, and why not (`detail`: the status's detail).
	virtual MenuPreviewStatus status(std::string *detail) const = 0;
	// The files the screen names that the project does not have, each once.
	virtual const std::vector<std::string> &missing() const = 0;
	// The files the screen names that the project has but that did not load (a font or
	// string table that does not parse, a texture that does not decode), each once.
	virtual const std::vector<std::string> &unreadable() const = 0;
	// Size the offscreen viewport to the device size and draw its texture as the current
	// ImGui item (it renders on the frames it is drawn).
	virtual void draw(int device_width, int device_height) = 0;
	// How it draws the screen (every window shown, one window held in a state): they apply
	// after every configure, and a change configures again.
	virtual void set_options(const MenuPreviewOptions &options) = 0;
	virtual const MenuPreviewOptions &options() const = 0;
	// The configured compiler and its frame state; null unless ready.
	virtual const menu::MenuFrameCompiler *compiler() const = 0;
	virtual const menu::MenuFrameState *frame_state() const = 0;
	// The document revision the picture shows: an index maps to a record only while it is
	// the document's own.
	virtual uint64_t shown_revision() const = 0;
};

// The menu preview, the Preview window's menu pane (ADR 0046 S6c, S9j, S9k1, S9k2, S11d):
// the selected screen of the menu the view previews as the game draws it, refreshed on
// every edit of the menu, its stylesheets and its string tables; when there is nothing to
// draw it says why. The toolbar zooms (fit, a scale, or a device size), snaps drags to the
// grid, shows hidden windows, holds the selected window in a state and arranges the
// selected windows (align, distribute, drawing order). On the canvas a click selects the
// window the game's hit test finds, Shift+click adds one and Ctrl+click adds or drops one,
// and a drag from the screen's background (a root window not selected, or nothing)
// selects every window its box touches. Every selected window is outlined, the primary
// with eight handles: a drag of a handle resizes the primary, a drag of a selected window
// moves every selected one (one undo step per drag), as do the arrow keys; Esc selects the
// primary's parent; Ctrl+C / X / V / D copy, cut, paste and duplicate windows, also from
// the right-click menu; the middle button or Space pans, Ctrl+wheel zooms about the mouse.
class MenuPreviewPane {
public:
	explicit MenuPreviewPane(EditorHost &host) : host_(host) {}
	void set_viewport(MenuPreviewViewport *viewport) { viewport_ = viewport; }
	// Into the current window, below the Preview window's line naming the screen.
	void draw();
	// The Preview window shows the other pane: a drag or a nudge in progress ends, its
	// gesture's end raised.
	void end_gestures();

private:
	// How the picture is sized: fitted to the window (the game's 4:3), a scale of the
	// 800x600 design, or the options' device size (each axis scaled on its own).
	enum class Zoom : uint8_t { Fit, Scale, Device };
	// The left button down on the canvas, and the drag it becomes.
	struct Press {
		bool active = false;
		bool resize = false;   // on a handle of the primary window
		bool marquee = false;  // on the screen's background: a drag selects what its box touches
		bool dragging = false; // moved past the threshold
		bool sent = false;     // a step of the drag went out
		float x = 0.0f, y = 0.0f;   // where, in screen pixels
		float sx = 1.0f, sy = 1.0f; // the device scale at the press
		float from_x = 0.0f, from_y = 0.0f, to_x = 0.0f, to_y = 0.0f; // the marquee, in design units
		int pick = -1;              // the widget the game's hit test found there
		// Shift: Add, Ctrl: Toggle (the selection changes on release, nothing moves).
		SelectMode join = SelectMode::Replace;
		std::string path;
		NodeAddress window;  // what a drag moves or resizes (none: a click only)
		LayoutPress layout;  // where the drag began: the windows it takes (the selected ones, or the one picked)
		LayoutHandle handle = LayoutHandle::Move;
		uint64_t gesture = 0;
		int dx = 0, dy = 0, grid = -1; // the last step planned
	};
	// Arrow keys on the selected windows: one gesture while any is held.
	struct Nudge {
		uint64_t gesture = 0;
		bool sent = false;
		std::string path;
		std::vector<NodeAddress> windows;
		LayoutPress press;
		int dx = 0, dy = 0;
	};
	// What the canvas pass needs of this frame.
	struct Frame {
		const MnuDocument *document = nullptr;
		const Node *screen = nullptr;
		NodeAddress selected;             // the primary window of the screen (none: none)
		std::vector<NodeAddress> windows; // every selected window of the screen, the primary among them
		bool only_windows = false;        // every selected record is a window of the screen (Copy, Cut, Duplicate)
		const menu::MenuFrameCompiler *compiler = nullptr;
		const menu::MenuFrameState *state = nullptr;
		bool current = false; // the picture shows the document's revision
		std::vector<menu::MenuFrameNote> notes;
	};

	void follow_selection_(const MnuDocument &document, const NodeAddress &selected);
	void toolbar_(const Frame &frame);
	void canvas_(const Frame &frame, float height);
	void keys_(const Frame &frame);
	// The Arrange items (the toolbar's menu and the canvas's), and one of them run.
	void arrange_items_(const Frame &frame);
	void arrange_(const Frame &frame, ArrangeOp op);
	// Copy, Cut, Paste or Duplicate the selected windows (the keys and the canvas's menu).
	void clipboard_(const Frame &frame, EditorRequestKind kind);
	// The selected window a press at a screen point moves: the primary when its rect holds
	// the point, else the front-most selected window whose rect does (none: none does).
	NodeAddress selected_window_at_(const Frame &frame, float x, float y, float origin_x, float origin_y, float sx,
	                                float sy) const;
	void press_on_canvas_(const Frame &frame, float mouse_x, float mouse_y, float origin_x, float origin_y, float sx,
	                      float sy);
	void drag_step_(const Frame &frame, float mouse_x, float mouse_y, float origin_x, float origin_y, bool free);
	void release_(const Frame &frame);
	// The windows the marquee's box touches, selected.
	void marquee_select_(const Frame &frame);
	void end_press_();
	void end_nudge_();

	EditorHost &host_;
	MenuPreviewViewport *viewport_ = nullptr;
	Zoom zoom_ = Zoom::Fit;
	float scale_ = 1.0f; // Zoom::Scale
	bool snap_ = true;
	bool scroll_pending_ = false; // a zoom about the mouse: the canvas's scroll next frame
	float scroll_x_ = 0.0f, scroll_y_ = 0.0f;
	bool panning_ = false;
	Press press_;
	Nudge nudge_;
	NodeId followed_ = 0; // the selected window the held state last followed
	// The mouse over the picture in design units, from the last canvas pass (the toolbar's
	// readout).
	bool mouse_on_picture_ = false;
	int mouse_design_x_ = 0, mouse_design_y_ = 0;
};

} // namespace opennova::editor
