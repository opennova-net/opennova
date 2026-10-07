#pragma once

#include <string>
#include <vector>

#include <editor/preview/canvas_half.h>
#include <editor/preview/hud_layout_edit.h>

namespace opennova::editor {

class HudViewport;
struct HudPreviewElement;

// The picked element's corner handles: squares kHudHandleSize across, taking a press kHudHandleSlop past
// their edge (the menu's handles' size, preview/menu_canvas.h).
inline constexpr float kHudHandleSize = 6.0f;
inline constexpr float kHudHandleSlop = 4.0f;
inline constexpr HudHandle kHudCorners[] = { HudHandle::TopLeft, HudHandle::TopRight, HudHandle::BottomLeft,
                                             HudHandle::BottomRight };

// The HUD viewport's half of a canvas (the plan's DI-20, DI-37; preview/hud_viewport.h): a design picture of
// the screen the HUD is drawn at, which the canvas fits, zooms and scrolls. The element under the pointer is
// ringed and named in the hover tip (its words, the hudpos.def lines that place it, the textures it draws);
// a click picks it (a SetViewport of the options' `picked`, nothing picked on a click of nothing), which
// rings it apart with a handle at each corner where the game reads a size for it. A drag of an element
// moves it, a drag of a corner of the picked one resizes it, the arrows nudge the picked one a design unit
// (Shift: 8): each step the values its lines take (preview/hud_layout_edit), landing on the 1024 x 768
// design grid whatever the screen shown, sent as an EditRecord of the text carrying the gesture's token,
// so a drag is one undo step and the picture follows it as it goes. The Go to of a picked element's line or
// texture is the view's (window_requests::go_to), as a double click's is.
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
	// The corner of the picked element's box (min, max on the picture) a press at `at` takes (false: none).
	static bool corner_at(CanvasPoint at, CanvasPoint min, CanvasPoint max, HudHandle &out);

private:
	// One step of the drag or the nudge: the values from where it began, sent when they changed.
	void step_(const ViewportContext &context, float dx, float dy, CanvasRequests &out);

	CanvasGesture gesture_;
	const HudViewport *viewport_ = nullptr; // the frame's, from follow
	// What the press took: the element under it and the handle (a corner of the picked one, else a move),
	// where it began (its values, the picture's design units per pixel then), and the step last sent.
	struct Press {
		bool element = false;
		HudHandle handle = HudHandle::Move;
		HudDragStart start;
		float units_x = 1.0f;
		float units_y = 1.0f;
		std::vector<HudValueChange> sent;
	};
	Press press_;
	// A nudge's way so far, in design units.
	int nudge_x_ = 0;
	int nudge_y_ = 0;
};

} // namespace opennova::editor
