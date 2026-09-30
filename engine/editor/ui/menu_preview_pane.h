#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mnu_clipboard.h>
#include <editor/preview/menu_canvas.h>
#include <editor/preview/menu_preview_state.h>
#include <editor/session/editor_request.h>
#include <editor/ui/editor_host.h>
#include <editor/ui/viewport_canvas.h>
#include <runtime/menu/menu_frame.h>

namespace opennova::editor {

class MnuDocument;

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
// Ctrl+wheel zooms about the mouse.
class MenuPreviewPane {
public:
	explicit MenuPreviewPane(EditorHost &host);
	void set_viewport(MenuPreviewViewport *viewport) { viewport_ = viewport; }
	// Into the current window, below the Preview window's line naming the screen.
	void draw();
	// After every frame's windows (the workspace's frame bracket): a canvas that did not draw
	// this frame (the model pane shown, the window closed or hidden, nothing to show) ends its
	// drag or nudge, its end raised once for the menu it began in.
	void end_frame();

private:
	// What the pane draws this frame: what its canvas maps, and what the clipboard takes.
	struct Frame {
		MenuCanvasFrame canvas;
		MenuClipboard clipboard;
	};

	void follow_selection_(const MnuDocument &document, const NodeAddress &selected);
	void toolbar_(const Frame &frame);
	void draw_canvas_(const Frame &frame, float height);
	// The clipboard's shortcuts (the arrows and Esc are the canvas's).
	void keys_(const Frame &frame);
	// The Arrange items (the toolbar's menu and the canvas's).
	void arrange_items_(const Frame &frame);
	// Copy, Cut, Paste or Duplicate the selected windows (the keys and the canvas's menu).
	void clipboard_(const Frame &frame, EditorRequestKind kind);

	EditorHost &host_;
	MenuPreviewViewport *viewport_ = nullptr;
	ViewportCanvas canvas_;
	MenuCanvas menu_canvas_;
	CanvasWindowRequests requests_;
	bool snap_ = true;
	NodeId followed_ = 0; // the selected window the held state last followed
	// The mouse over the picture in design units, from the last canvas pass (the toolbar's
	// readout).
	bool mouse_on_picture_ = false;
	int mouse_design_x_ = 0, mouse_design_y_ = 0;
};

} // namespace opennova::editor
