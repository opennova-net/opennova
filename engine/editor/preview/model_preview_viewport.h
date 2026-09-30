#pragma once

namespace opennova::editor {

class ModelPreviewModel;

// The only seam between the engine-owned model pane and a rendering device (ADR 0046 d11,
// the GameViewport pattern): the shell renders the previewed model through the runtime's
// own ObjectModel into an offscreen viewport and draws it into the pane; engine-only runs
// leave it null. The pane moves the portable half's camera and options, the device
// follows them. Declared apart from the pane (S13 V4), so the shell's device names no window
// header; S13 V5's viewport seam replaces it.
class ModelPreviewViewport {
public:
	virtual ~ModelPreviewViewport() = default;
	// What the device shows and how (the status, the options, the camera).
	virtual ModelPreviewModel &model() = 0;
	// Size the offscreen viewport to the device size and draw its texture as the current
	// ImGui item (it renders on the frames it is drawn).
	virtual void draw(int device_width, int device_height) = 0;
};

} // namespace opennova::editor
