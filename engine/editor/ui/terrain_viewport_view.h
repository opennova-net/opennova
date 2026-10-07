#pragma once

#include <string>

#include <editor/preview/terrain_viewport.h>
#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The terrain viewport's view (the deep-integration plan's DI-30b; preview/terrain_viewport.h): its toolbar (the
// mission whose environment, tile set and tiles it is drawn under, or the engine's own; Show (the foliage, the
// water); Over the terrain (DI-29's overlays); Frame and Top), the canvas filling the rest (orbit, pan, dolly),
// the overlay's legend over the picture's corner, and under the picture the ground under the pointer in the
// game's words (DI-07), then a line while the picture lacks a file. Every change a SetViewport.
class TerrainViewportView final : public ViewportView {
public:
	TerrainViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;

private:
	// The ground under the pointer as the canvas last read it (asked again only when the pointer, the picture or
	// the camera moved): its line and its surface class (-1 none).
	struct Asked {
		bool valid = false;
		float x = 0.0f, y = 0.0f;
		int width = 0, height = 0;
		float camera[6] = {};
		bool surface = false;
		uint64_t reads = 0;
	};
	Asked asked_;
	std::string ground_;
	int ground_surface_ = -1;
};

} // namespace opennova::editor
