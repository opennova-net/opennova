#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/preview/canvas_half.h>
#include <editor/preview/model_preview_camera.h>

namespace opennova::editor {

class ViewportModel;

// How far a wheel notch dollies an orbiting camera (the model canvas's step).
inline constexpr float kOrbitWheelDolly = 0.85f;

// An orbit camera on the wire (a viewport's `camera`, its SetViewport's): {target [x, y, z], yaw, pitch,
// distance}; and the camera a `camera` member sets over `held`: those members and `frame` (the camera on what
// the picture shows), each optional, the pitch kept within kOrbitPitchLimit. False, nothing changed, with why,
// for another member or a value of another type.
io::JsonValue orbit_camera_to_json(const OrbitCamera &camera);
bool read_orbit_camera(const io::JsonValue &json, OrbitCamera &held, bool &frame, std::string &error);

// What an orbit canvas reads of its viewport (a viewport whose picture names nothing a point of it is on: an
// effect's, DI-14; a definition's, DI-21): its orbit camera, that camera looking at what the picture shows on
// a picture `width` x `height` (its angles kept), and the change a SetViewport makes to set the camera.
struct OrbitCanvasHooks {
	const OrbitCamera &(*camera)(const ViewportModel &viewport) = nullptr;
	OrbitCamera (*framed)(const ViewportModel &viewport, int width, int height) = nullptr;
	std::string (*change)(const OrbitCamera &camera) = nullptr;
};

// The half of a canvas over a picture its camera orbits (ADR 0046 DI-14, the effect viewport's; DI-21, the
// definition viewport's): the picture fills the canvas and its camera orbits what it shows, every change a
// SetViewport of its camera. A drag of the left button orbits, of the middle one (or with Shift) pans; the
// wheel dollies; a double click or F frames what the picture shows. Nothing it does edits the document: a
// point of the picture names nothing.
class OrbitCanvas final : public CanvasHalf {
public:
	explicit OrbitCanvas(OrbitCanvasHooks hooks) : hooks_(hooks) {}
	const CanvasGesture &gesture() const override { return gesture_; }
	void follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	void end(CanvasRequests &out) override;
	void end_frame(CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const override;
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

private:
	OrbitCanvasHooks hooks_;
	CanvasGesture gesture_;
	const ViewportModel *viewport_ = nullptr; // the frame's, from follow
	bool pan_ = false; // the press pans (the middle button, or Shift held)
};

} // namespace opennova::editor
