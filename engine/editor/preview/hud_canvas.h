#pragma once

#include <string>

#include <editor/preview/canvas_half.h>

namespace opennova::editor {

class HudViewport;

// The HUD viewport's half of a canvas (the plan's DI-20; preview/hud_viewport.h): a design picture of
// the screen the HUD is drawn at, which the canvas fits, zooms and scrolls. The element under the
// pointer is ringed and named in the hover tip (its words, the hudpos.def lines that place it, the
// textures it draws); a click picks it (a SetViewport of the options' `picked`, nothing picked on a
// click of nothing), which rings it apart. Nothing it does edits the layout: the Go to of a picked
// element's line or texture is the view's (window_requests::go_to), as a double click's is.
class HudCanvas final : public CanvasHalf {
public:
	const CanvasGesture &gesture() const override { return gesture_; }
	void follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	void end(CanvasRequests &out) override;
	void end_frame(CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const override;
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

	// The screen point (the resolution's pixels) under the picture's pixel (x, y) of a picture
	// `width` x `height` that shows a screen `screen_width` x `screen_height`.
	static void screen_point(float x, float y, int width, int height, int screen_width, int screen_height, float &sx,
			float &sy);

private:
	CanvasGesture gesture_;
	const HudViewport *viewport_ = nullptr; // the frame's, from follow
};

} // namespace opennova::editor
