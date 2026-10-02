#pragma once

#include <memory>

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// A menu viewport's view (ADR 0046 S6c, S9j, S9k1, S9k2, S11d, S13 V2, V5; ui/viewport_views' menu
// row): the screen of the menu as the game draws it, refreshed on every edit of the menu, its
// stylesheets and its string tables; when there is nothing to draw it says why. A toolbar and the
// compiler's notes around one canvas (preview/menu_canvas, the menu viewport's half). The toolbar
// zooms (fit, a scale, or the device's size, which it sets), snaps drags to the grid, shows hidden
// windows, holds the selected window in a state and arranges the selected windows (align,
// distribute, drawing order); what it changes of the viewport is a SetViewport. On the canvas a
// click selects the window the game's hit test finds, Shift+click adds one and Ctrl+click adds or
// drops one, and a drag from the screen's background (a root window not selected, or nothing)
// selects every window its box touches, in one selection. Every selected window is outlined, the
// primary with eight handles: a drag of a handle resizes the primary, a drag of a selected window
// moves every selected one (one batch per step, one undo step per drag), as do the arrow keys; Esc
// selects the primary's parent; Ctrl+C / X / V / D copy, cut, paste and duplicate windows, also
// from the right-click menu; the middle button or Space pans, Ctrl+wheel zooms about the mouse.
class MenuViewportView final : public ViewportView {
public:
	MenuViewportView();
	~MenuViewportView() override;

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;

private:
	struct Tools;
	std::unique_ptr<Tools> tools_;
};

} // namespace opennova::editor
