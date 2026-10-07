#pragma once

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The HUD viewport's view (the plan's DI-20): its toolbar (the screen it is drawn at, the player's
// stance, weapon, clip, reserve and health, the view through binoculars or night vision, a hit's
// damage vignette, the HUD detail level, the crosshair style), the element picked with a Go to of each
// hudpos.def line that places it and of each texture it draws (window_requests::go_to), then the
// canvas filling the rest (preview/hud_canvas: hover names an element, a click picks it, a double
// click goes to its line; DI-37: a drag moves it, a corner of the picked one sizes it, the arrows nudge
// it, each written to its hudpos.def lines). Every change of the state a SetViewport; the picked
// element's fields are the Inspector's (ui/hud_layout_inspector).
class HudViewportView final : public ViewportView {
public:
	HudViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;
};

} // namespace opennova::editor
