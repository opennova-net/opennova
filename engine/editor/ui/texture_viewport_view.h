#pragma once

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The texture viewport's view (ADR 0046 S18): its toolbar (Fit, 1:1 and the zoom; the channels shown;
// the mip level, where the file holds more than one), then the canvas filling the rest, which owns the
// wheel, the pans and the fit (preview/texture_canvas). Every change a SetViewport.
class TextureViewportView final : public ViewportView {
public:
	TextureViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;
};

} // namespace opennova::editor
