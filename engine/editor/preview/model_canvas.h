#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/preview/canvas_gesture.h>
#include <editor/preview/model_handle_edit.h>
#include <editor/preview/model_overlay.h>
#include <editor/preview/viewport_overlay.h>

namespace opennova::editor {

class ModelDocument;
class ModelPreviewModel;

// The model preview's canvas (ADR 0046 S10p3 to S10p5, S13 V2): what the preview marks over
// the model (preview/model_overlay: the user points and their Z axes, the lights with their
// reach and a spot's axis, the part pivots when asked), what a press on the picture takes (the
// selected marker's place or axis tip, whose drag writes its record through model_handle_edits;
// else a marker a click selects, and a drag that orbits, or pans with the middle button or
// Shift), and the camera's wheel dolly and framing.

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
	ModelPreviewModel *model = nullptr; // the preview's portable half: its camera, level and clock
	std::vector<ModelOverlay> overlays; // what the picture marks now
	bool current = false; // the picture shows the document's revision
	// The selected record's marker, while the model is the active document (-1: none).
	ModelOverlayKind selected_kind = ModelOverlayKind::UserPoint;
	int selected = -1;
	float snap = 0.0f; // a dragged place snaps to this grid, metres (Alt: free)
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
	CanvasPoint offset; // from the pointer to the handle's pixel
};
ModelGrab model_canvas_grab(const ModelCanvasFrame &frame, const CanvasInput &in);
// The front-most marker within kModelPickSlop of the pointer (-1: none, or not hovered).
int model_canvas_under(const ModelCanvasFrame &frame, const CanvasInput &in);

// The canvas's gestures on the model pane, and what it draws and shows.
class ModelCanvas {
public:
	const CanvasGesture &gesture() const { return gesture_; }

	// The frame's start, while the pane draws its canvas: a gesture begun in another document
	// ends.
	void follow(const ModelCanvasFrame &frame, CanvasRequests &out);
	// The frame's pointer: a press, then each sample of its drag (the handle's record planned
	// from the marker as pressed, one undo step; or the camera orbited or panned), then its
	// release (a click selects the marker's record, while the picture is the document's); the
	// wheel dollies, a double-click frames.
	void input(const ModelCanvasFrame &frame, const CanvasInput &in, CanvasRequests &out);
	// The gesture ends.
	void end(CanvasRequests &out);
	// The frame bracket (CanvasGesture::end_frame).
	void end_frame(CanvasRequests &out);
	// The camera looks at the selected marker (a light's reach around it, else a share of the
	// model), else at the whole model (F, a double-click, the toolbar's Frame).
	void frame_selected(const ModelCanvasFrame &frame) const;

	// Over the picture: each marker where the camera puts it, the one under the pointer ringed,
	// the selected one ringed with its axis tip's handle.
	OverlayList shapes(const ModelCanvasFrame &frame, const CanvasInput &in) const;
	// The marker under the pointer's name ("" none, or while dragging).
	std::string hover_tip(const ModelCanvasFrame &frame, const CanvasInput &in) const;

private:
	CanvasGesture gesture_;
	ModelGrab grab_;
};

} // namespace opennova::editor
