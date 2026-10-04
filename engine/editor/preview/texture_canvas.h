#pragma once

#include <string>

#include <editor/preview/canvas_half.h>

namespace opennova::editor {

class TextureViewport;

// The texture viewport's half of a canvas (ADR 0046 S18; preview/texture_viewport.h): the picture fills
// the canvas and the camera is the kind's, every change a SetViewport of its camera. The wheel steps the
// zoom about the pointer (the texel under it stays under it); a drag of the left or the middle button
// pans; a double click or F fits the texture to the picture. The texel under the pointer is ringed past
// a few pixels a texel, and its words are the hover tip (its column and row in the level shown and its
// value, its palette entry where it has one). Nothing it does edits the texture.
class TextureCanvas final : public CanvasHalf {
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
	const TextureViewport *viewport_ = nullptr; // the frame's, from follow
};

} // namespace opennova::editor
