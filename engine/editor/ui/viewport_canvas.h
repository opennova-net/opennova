#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class Workspace;

// A canvas's requests raised as the windows raise theirs (editor_requests.h): a record selected,
// a step's batch, a gesture's end, the viewport's state.
class CanvasWindowRequests final : public CanvasRequests {
public:
	explicit CanvasWindowRequests(Workspace &workspace) : workspace_(workspace) {}
	void request(EditorRequest request) override;

private:
	Workspace &workspace_;
};

// The editor's canvas (ADR 0046 S13 V2, V5), one per viewport view (ui/viewport_view), drawing the
// viewport's picture on it: the surface first, so every press on the canvas is its own and the
// device's item under it never takes the mouse; the kind's hover tip, made only while it shows;
// the device's picture (the Shell's SubViewport texture, drawn as an ImGui item);
// the kind's overlay shapes over it, clipped to the canvas, coloured by their roles; the cursor the
// kind asks for; the keys the kinds act on (the arrows, Esc, F), read while the canvas's window has
// the keyboard; and the zoom and the pan. A picture that fills the canvas (the model's) leaves the
// zoom and the pan to its kind's camera and hands it the wheel, the middle button and the right one. A design
// picture (the menu's 800 x 600) is the canvas's to zoom and scroll: fitted to the canvas at the
// design's aspect, a scale of the design (Ctrl+wheel steps it about the mouse), or the device's
// own size, with a margin the handles on its edges stay on; the middle button, or Space with
// the left, pans it, and a left press on it lasts until the button comes up. Each frame begin()
// lays the canvas out and reads the keys and the pointer (input(), which the kind's canvas takes),
// picture() shows the tip and draws the device's picture, draw() paints the shapes and sets the
// cursor, end() closes it; end_frame() is the workspace's frame bracket.
class ViewportCanvas {
public:
	enum class Zoom : uint8_t { Fill, Fit, Scale, Device };
	// A design picture's zoom steps (Ctrl+wheel, and the menu pane's Zoom list).
	static constexpr float kZoomLevels[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };
	using Device = std::function<void(const ViewportPicture &picture)>;
	using Tip = std::function<std::string()>;

	// A picture that fills the canvas (no design size), or a design picture `design_width` x
	// `design_height`, fitted until the zoom changes.
	explicit ViewportCanvas(int design_width = 0, int design_height = 0);

	Zoom zoom() const { return zoom_; }
	float scale() const { return scale_; } // Zoom::Scale
	// A design picture's zoom: Fit, Scale (`scale` of the design) or Device.
	void set_zoom(Zoom zoom, float scale = 1.0f);

	// The canvas `height` tall across the current window, the device's own size `device_width` x
	// `device_height` for Zoom::Device: its surface, and the frame's keys and pointer. False when
	// it does not show (a scrolled child clipped away): no input then. end() follows either way.
	bool begin(float height, int device_width, int device_height);
	const CanvasInput &input() const { return input_; }
	// After begin(), when it shows: the kind's hover tip on the surface (`tip` made only while it
	// shows), then the device's picture (`device` draws it where the picture lies, at its size, as the
	// current item; the canvas's surface its clip) and its edge.
	void picture(const Device &device, const Tip &tip);
	// After picture(): a line over the picture's top left corner (S13 V6: how far its device's build
	// is while the last picture shows, or why the build failed).
	void badge(const std::string &text);
	// The right button clicked on the canvas this frame (not while it pans). On a picture that fills
	// the canvas, whose right button is its kind's (a look), a click is the button let go having
	// travelled less than a drag does.
	bool right_clicked() const { return right_clicked_; }
	void draw(const OverlayList &shapes, CanvasCursor cursor);
	void end();
	// After every frame's windows: a canvas that did not draw lets go of its pan.
	void end_frame();

private:
	int design_width_ = 0, design_height_ = 0;
	Zoom zoom_ = Zoom::Fill;
	float scale_ = 1.0f;
	bool scroll_pending_ = false; // a zoom about the mouse: the canvas's scroll next frame
	float scroll_x_ = 0.0f, scroll_y_ = 0.0f;
	bool panning_ = false;
	bool child_ = false; // a design picture's scrolling child is open
	bool drawn_ = false; // drew this frame
	bool right_clicked_ = false;
	bool popup_last_frame_ = false; // a popup was open as the canvas began last frame (its Esc is the popup's)
	bool right_held_ = false; // the right button went down on a picture that fills the canvas
	CanvasPoint origin_; // the picture's top-left corner on the screen
	CanvasPoint surface_min_, surface_max_; // the canvas's surface on the screen (the shapes' clip)
	CanvasInput input_;
};

} // namespace opennova::editor
