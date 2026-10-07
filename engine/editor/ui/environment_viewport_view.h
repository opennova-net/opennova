#pragma once

#include <editor/preview/environment_viewport.h>
#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The environment viewport's view (the deep-integration plan's DI-19b; preview/environment_viewport.h): its
// toolbar (Run or Pause of the preview clock, the time it shows, the day's length, Start, the mission it is
// drawn over and its header's overrides, the layers, Frame), the day's track (24 hours with a mark at each
// keyframe and the segment the clock is in; a click or a drag runs the clock from that hour), the weather a
// script sets (rain and overcast, each a percent over seconds, and Clear) with where each stands, then the
// canvas filling the rest, which orbits, pans and dollies the camera (preview/orbit_canvas). Every change a
// SetViewport.
class EnvironmentViewportView final : public ViewportView {
public:
	EnvironmentViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;

private:
	// The weather the next command issues (the sliders' values before Rain or Overcast is pressed).
	EnvironmentWeatherCommand rain_{ 60, 10 };
	EnvironmentWeatherCommand overcast_{ 100, 10 };
};

} // namespace opennova::editor
