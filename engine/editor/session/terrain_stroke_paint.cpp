#include <editor/session/terrain_stroke_planes.h>

namespace opennova::editor {

// The maps' planner (S25 lane B: the surface, foliage, colour and blend maps painted): not yet.
bool plan_paint_stroke(TerrainStrokeContext &context, TerrainStrokePlan &plan, std::string &error) {
	(void)plan;
	error = std::string("Painting ") + terrain_plane_words(context.plane) + " is not ready yet in this build.";
	return false;
}

} // namespace opennova::editor
