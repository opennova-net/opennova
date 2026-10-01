#pragma once

#include <string>

#include <editor/ui/ui_kit.h>
#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The script device's view (ADR 0046 S13 V10; ViewportKind::Script's row of ui/viewport_views;
// CONTEXT.md "Script device"): no canvas, since the device is a Godot Control (a CodeEdit) that owns
// the input in its rect (decision 11's allowed exception for script text). It reserves the rest of
// the window's room for the device, an item over all of it so a press there is never the window's
// (no window moves under a drag that selects text), hands the device the rect each frame it is
// shown (ViewportDevice::draw: the place in the OS window's pixels and the clip), and lets the
// pointer through to the control over that rect (Dear ImGui wants none of it the next frame while
// the pointer is over it or a press there lasts, whatever item holds the keyboard: a text field's
// first press out of it is the control's). It places nothing, and says so (placed), while the
// device is not made yet (the next pump makes it), while a popup, a modal or a window floating over
// the tab lies over the rect (a Control placed there would hide it), or while the window is in
// another OS window than the main one: the script view then draws the document's lines instead
// (ui/text_view), and the device hides on its next tick. A window or a modal begun after the tab in
// the frame is known only once begun: end_frame asks again with every window begun, and hides the
// device the same frame (its draw of a picture with no room), the lines coming the frame after.
class ScriptViewportView final : public ViewportView {
public:
	ScriptViewportView();
	// Whether the last frame that drew it placed the device, and left it placed through the frame's
	// end.
	bool placed() const { return placed_; }
	void end_frame(Workspace &workspace) override;

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;
	void draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) override;

private:
	bool placed_ = false;
	// Where the device was placed this frame (the frame count it was), the document it shows and the
	// rect it covers, for end_frame's second look.
	int placed_frame_ = -1;
	std::string path_;
	ui_kit::Cover cover_;
};

} // namespace opennova::editor
