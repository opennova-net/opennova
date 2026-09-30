#pragma once

#include <memory>

namespace opennova::editor {

class Workspace;

// The menu preview, the Preview window's menu pane (ADR 0046 S6c, S9j, S9k1, S9k2, S11d, S13 V2):
// the selected screen of the menu the view previews as the game draws it, refreshed on every
// edit of the menu, its stylesheets and its string tables; when there is nothing to draw it says
// why. A toolbar and the compiler's notes around one canvas (ui/viewport_canvas over
// preview/menu_canvas). The toolbar zooms (fit, a scale, or a device size), snaps drags to the
// grid, shows hidden windows, holds the selected window in a state and arranges the selected
// windows (align, distribute, drawing order). On the canvas a click selects the window the
// game's hit test finds, Shift+click adds one and Ctrl+click adds or drops one, and a drag from
// the screen's background (a root window not selected, or nothing) selects every window its box
// touches. Every selected window is outlined, the primary with eight handles: a drag of a handle
// resizes the primary, a drag of a selected window moves every selected one (one undo step per
// drag), as do the arrow keys; Esc selects the primary's parent; Ctrl+C / X / V / D copy, cut,
// paste and duplicate windows, also from the right-click menu; the middle button or Space pans,
// Ctrl+wheel zooms about the mouse. What it holds (its canvas, the menu's half of it, the
// toolbar's settings) is its own, behind a pointer, so this header names none of the preview's
// or the canvas's types (S13 V4); the device it draws through is the Shell's
// (preview/menu_preview_viewport.h, the workspace's devices).
class MenuPreviewPane {
public:
	explicit MenuPreviewPane(Workspace &workspace);
	~MenuPreviewPane();
	MenuPreviewPane(const MenuPreviewPane &) = delete;
	MenuPreviewPane &operator=(const MenuPreviewPane &) = delete;
	// Into the current window, below the Preview window's line naming the screen.
	void draw();
	// After every frame's windows (the workspace's frame bracket): a canvas that did not draw
	// this frame (the model pane shown, the window closed or hidden, nothing to show) ends its
	// drag or nudge, its end raised once for the menu it began in.
	void end_frame();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::editor
