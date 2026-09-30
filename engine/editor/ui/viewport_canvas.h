#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/viewport_overlay.h>
#include <editor/ui/editor_host.h>

namespace opennova::editor {

// A canvas's requests raised as the windows raise theirs (editor_requests.h): a record selected,
// a step's batch, a gesture's end.
class CanvasWindowRequests final : public CanvasRequests {
public:
	explicit CanvasWindowRequests(EditorHost &host) : host_(host) {}
	void select(const std::string &path, const NodeAddress &record, CanvasJoin join) override;
	void edits(const std::string &path, std::vector<Edit> batch) override;
	void end_edit(const std::string &path) override;

private:
	EditorHost &host_;
};

// The editor's canvas (ADR 0046 S13 V2), one for the Preview window's menu pane and one for its
// model pane, each drawing its picture on it: the surface first, so every press on the canvas is
// its own and the device's item under it never takes the mouse; the kind's hover tip, made only
// while it shows; the device's picture (the shell's SubViewport texture, drawn as an ImGui item);
// the kind's overlay shapes over it, clipped to the canvas, coloured by their roles; the cursor the
// kind asks for; and the zoom and the pan. A picture that fills the canvas (the model's) leaves the
// zoom and the pan to its kind's camera and hands it the wheel and the middle button. A design
// picture (the menu's 800 x 600) is the canvas's to zoom and scroll: fitted to the canvas at the
// design's aspect, a scale of the design (Ctrl+wheel steps it about the mouse), or the device's
// own size, with a margin the handles on its edges stay on; the middle button, or Space with
// the left, pans it. Each frame begin() lays it out and reads the pointer over the picture
// (input(), which the kind's canvas takes), draw() paints the shapes and sets the cursor, end()
// closes it; end_frame() is the workspace's frame bracket.
class ViewportCanvas {
public:
	enum class Zoom : uint8_t { Fill, Fit, Scale, Device };
	// A design picture's zoom steps (Ctrl+wheel, and the menu pane's Zoom list).
	static constexpr float kZoomLevels[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };
	using Device = std::function<void(int width, int height)>;
	using Tip = std::function<std::string(const CanvasInput &input)>;

	// A picture that fills the canvas (no design size), or a design picture `design_width` x
	// `design_height`, fitted until the zoom changes.
	explicit ViewportCanvas(int design_width = 0, int design_height = 0);

	Zoom zoom() const { return zoom_; }
	float scale() const { return scale_; } // Zoom::Scale
	// A design picture's zoom: Fit, Scale (`scale` of the design) or Device.
	void set_zoom(Zoom zoom, float scale = 1.0f);

	// The canvas `height` tall across the current window, the device's own size `device_width` x
	// `device_height` for Zoom::Device; `device` draws the picture at a size as the current item,
	// `tip` makes the hover tip. False when it does not show (a scrolled child clipped away): no
	// input then. end() follows either way.
	bool begin(float height, int device_width, int device_height, const Device &device,
			const Tip &tip);
	const CanvasInput &input() const { return input_; }
	// The right button clicked on the canvas this frame (not while it pans).
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
	CanvasPoint origin_; // the picture's top-left corner on the screen
	CanvasPoint surface_min_, surface_max_; // the canvas's surface on the screen (the shapes' clip)
	CanvasInput input_;
};

} // namespace opennova::editor
