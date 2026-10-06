#pragma once

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The effect viewport's view (ADR 0046 DI-14; preview/effect_viewport.h): its toolbar (the effect shown
// among the file's, Run or Pause of the preview clock, Replay, Loop, the ground grid, the mission wind,
// Frame) and a readout of the playing cycle, a line saying where the game resolves the effect when that
// is not this file's block and which graphics the project lacks, then the canvas filling the rest, which
// orbits, pans and dollies the camera (preview/effect_canvas). Every change a SetViewport.
class EffectViewportView final : public ViewportView {
public:
	EffectViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;
	void draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) override;
};

} // namespace opennova::editor
