#pragma once

#include <string>

#include <editor/preview/canvas_half.h>

namespace opennova::editor {

class EffectViewport;

// How far a wheel notch dollies an effect's camera (the model canvas's step).
inline constexpr float kEffectWheelDolly = 0.85f;

// The effect viewport's half of a canvas (ADR 0046 DI-14; preview/effect_viewport.h): the picture fills
// the canvas and its camera orbits the spawn point, every change a SetViewport of its camera. A drag of
// the left button orbits, of the middle one (or with Shift) pans; the wheel dollies; a double click or F
// frames the live particles. Nothing it does edits the file: a particle file's text holds no records.
class EffectCanvas final : public CanvasHalf {
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
	CanvasGesture gesture_;
	const EffectViewport *viewport_ = nullptr; // the frame's, from follow
	bool pan_ = false; // the press pans (the middle button, or Shift held)
};

} // namespace opennova::editor
