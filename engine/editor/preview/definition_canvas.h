#pragma once

#include <string>

#include <editor/preview/canvas_half.h>
#include <editor/preview/model_preview_camera.h>

namespace opennova::editor {

class DefinitionViewport;

// How near the pointer takes an eye, picture pixels (the model canvas's marker slop).
inline constexpr float kDefinitionEyeSlop = 8.0f;

// The half of the definition viewport's canvas (DI-21, DI-22): the picture fills the canvas and its camera orbits
// what it shows (a drag of the left button orbits, of the middle one or with Shift pans, the wheel dollies, a double
// click or F frames), every camera change a SetViewport. A weapon seen in first person from the orbit shows its two
// eyes (DefinitionViewport::eyes): where the game's camera stands before the view model at the hip (weapon.def `pos`)
// and aimed (`tpos`), each with the way it looks, joined by the path the scope eases along. A drag of an eye moves it
// in the plane through it that faces the camera, each step an EditRecord of the record's view record (one undo
// step), and Alt held moves it along the way it looks.
class DefinitionCanvas final : public CanvasHalf {
public:
	const CanvasGesture &gesture() const override { return gesture_; }
	void follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	void end(CanvasRequests &out) override;
	void end_frame(CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const override;
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

private:
	// The eye under the picture point (x, y): its index in DefinitionViewport::eyes, else -1.
	int eye_under(const ViewportContext &context, float x, float y) const;

	CanvasGesture gesture_;
	const DefinitionViewport *viewport_ = nullptr; // the frame's, from follow
	bool pan_ = false; // the press pans (the middle button, or Shift held)
	int grabbed_ = -1; // the eye the press took (-1 none: the press moves the camera)
	CanvasPoint offset_; // the eye's pixel less the press's, kept through the drag
	PreviewVec3 along_; // Alt: the press's eye place, the drag sliding it along its look
	float along_y_ = 0.0f;
};

} // namespace opennova::editor
