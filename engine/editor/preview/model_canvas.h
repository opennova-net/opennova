#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/model_handle_edit.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class ModelDocument;
class ModelViewport;
class PreviewClock;

// The model viewport's canvas (ADR 0046 S10p3 to S10p5, S13 V2, V5): what the viewport marks over the
// model (preview/model_overlay: the user points and their Z axes, the lights with their reach and a
// spot's axis, the part pivots when asked), what a press on the picture takes (the selected marker's
// place or axis tip, whose drag writes its record through model_handle_edits, every selected
// marker's place moving as far, one batch per step; else a marker a click selects, and a drag that
// orbits, or pans with the middle button or Shift), and the camera's wheel dolly and framing. The
// camera is the viewport's state: an orbit, a pan, a dolly or a framing is a SetViewport request.

// How far from a marker's pixel a press still takes it, and what a wheel notch dollies.
inline constexpr float kModelPickSlop = 8.0f;
inline constexpr float kModelWheelDolly = 0.85f;
// The markers' own colours (a light draws in its own start colour), 0xRRGGBB.
inline constexpr uint32_t kUserPointRgb = 0xFFDC5A;
inline constexpr uint32_t kPivotRgb = 0x6EDCFF;

// What the canvas maps, one frame's worth.
struct ModelCanvasFrame {
	// The model document shown (none: an animation's rig model, which no open document holds).
	const ModelDocument *document = nullptr;
	// The viewport: its camera and level (a camera's move is a SetViewport of its path).
	const ModelViewport *model = nullptr;
	std::vector<ModelOverlay> overlays; // what the picture marks now
	bool current = false; // the picture shows the document's revision
	// The primary record's marker, while the model is the active document (-1: none), and the other
	// selected records' markers, which a drag of the primary's place moves as far (S13 D7).
	ModelOverlayKind selected_kind = ModelOverlayKind::UserPoint;
	int selected = -1;
	std::vector<ModelOverlay> others;
	float snap = 0.0f; // a dragged place snaps to this grid, metres (Alt: free)
	// Edits may be raised (S13 A3: false while an operation holds the documents, as
	// SessionView::allows(EditRecord) says; ViewportContext::editable): else no handle is taken, a
	// press selects or orbits.
	bool editable = true;
	const PreviewClock *clock = nullptr; // the clock the markers are posed at
};

// What a press on the canvas took: on the selected marker (or its axis tip) its handle, whose
// drag moves (or turns) its record, kept where the press took it; else the marker under it (a
// click selects its record) and a drag that orbits, or pans.
struct ModelGrab {
	bool pan = false; // the middle button, or Shift with the left
	int pick = -1; // the marker under the press (-1 none)
	bool handle = false; // on the selected marker or its axis tip
	ModelHandle which = ModelHandle::Place;
	ModelOverlay marker; // the marker as pressed
	std::vector<ModelOverlay> others; // the other selected markers as pressed (a place's drag)
	CanvasPoint offset; // from the pointer to the handle's pixel
};
// The front-most marker within kModelPickSlop of the pointer (-1: none, or not hovered): found
// once a frame, and what the hover ring, the tip and a press read (`under` below).
int model_canvas_under(const ModelCanvasFrame &frame, const CanvasInput &in);
ModelGrab model_canvas_grab(const ModelCanvasFrame &frame, const CanvasInput &in, int under);

// The canvas's gestures on a model viewport (its CanvasHalf), and what it draws and shows. Over a
// frame it is given (a test's), or the one it makes of the viewport at each frame's start
// (ModelViewport::canvas_frame).
class ModelCanvas final : public CanvasHalf {
public:
	const CanvasGesture &gesture() const override { return gesture_; }
	const ModelCanvasFrame &frame() const { return frame_; }

	// CanvasHalf, over the frame made of the viewport (a ModelViewport) at follow.
	void follow(const ViewportModel &viewport, const ViewportContext &context,
			CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &, const CanvasInput &) const override {
		return CanvasCursor::Default;
	}
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

	// The frame's start, while the canvas draws: a gesture begun on another subject ends (another
	// model's document, a reload of it; the model an animation plays on has none).
	void follow(const ModelCanvasFrame &frame, CanvasRequests &out);
	// The frame's pointer and keys, `under` the marker under the pointer: a press, then each
	// sample of its drag (the handle's record planned from the marker as pressed, every selected
	// marker's place moving as far, one undo step; or the camera orbited or panned), then its release
	// (a click selects the marker's record, while the picture is the document's); the wheel dollies;
	// a double-click, or F while the canvas has the keyboard, frames. A camera's move is a
	// SetViewport of the viewport's camera.
	void input(
			const ModelCanvasFrame &frame, const CanvasInput &in, int under, CanvasRequests &out);
	// The gesture ends.
	void end(CanvasRequests &out) override;
	// The frame bracket (CanvasGesture::end_frame).
	void end_frame(CanvasRequests &out) override;
	// The camera looking at the selected marker (a light's reach around it, else a share of the
	// model), else at the whole model (F, a double-click, the toolbar's Frame), on a picture `width`
	// x `height`: a SetViewport of the camera.
	void frame_selected(const ModelCanvasFrame &frame, int width, int height, CanvasRequests &out) const;

	// Over the picture: each marker where the camera puts it, the one under the pointer (`under`)
	// ringed, the selected one ringed with its axis tip's handle.
	OverlayList shapes(const ModelCanvasFrame &frame, const CanvasInput &in, int under) const;
	// The name of the marker under the pointer ("" none, or while dragging).
	std::string hover_tip(const ModelCanvasFrame &frame, int under) const;

private:
	CanvasGesture gesture_;
	ModelCanvasFrame frame_;
	ModelGrab grab_;
};

} // namespace opennova::editor
