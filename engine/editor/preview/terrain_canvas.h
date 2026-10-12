#pragma once

#include <string>

#include <editor/preview/canvas_half.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/terrain_brush_canvas.h>

namespace opennova::editor {

// The terrain viewport's canvas (ADR 0046 S25, the shared seams; it replaces DI-30b's orbit canvas): while the
// terrain's Terrain pane is hidden, or its brush's tool is None, the orbit camera's alone (a drag of the left button
// orbits, of the middle one or with Shift pans, the wheel dollies, F or a double click frames); while the brush is live,
// the brush takes the left button and its keys (TerrainBrushHalf) and the camera keeps the middle button, Alt and the
// wheel.
class TerrainCanvas final : public CanvasHalf {
public:
	explicit TerrainCanvas(OrbitCanvasHooks hooks) : orbit_(hooks) {}
	const CanvasGesture &gesture() const override { return brush_.live() ? gesture_ : orbit_.gesture(); }
	const TerrainBrushHalf &brush() const { return brush_; }
	void follow(const ViewportModel &viewport, const ViewportContext &context, CanvasRequests &out) override;
	void input(const ViewportContext &context, const CanvasInput &in, CanvasRequests &out) override;
	void end(CanvasRequests &out) override;
	void end_frame(CanvasRequests &out) override;
	OverlayList shapes(const ViewportContext &context, const CanvasInput &in) const override;
	CanvasCursor cursor(const ViewportContext &context, const CanvasInput &in) const override;
	std::string hover_tip(const ViewportContext &context, const CanvasInput &in) const override;

private:
	OrbitCanvas orbit_;
	TerrainBrushHalf brush_;
	CanvasGesture gesture_; // the brush's strokes'
	CanvasSubject subject_;
};

} // namespace opennova::editor
