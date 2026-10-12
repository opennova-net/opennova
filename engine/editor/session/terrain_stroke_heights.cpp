#include <editor/session/terrain_stroke_planes.h>

namespace opennova::editor {

// The heights' planner (S25 lane A: the heights document's stroke, documents/terrain_heights_brush): not yet.
bool plan_heights_stroke(TerrainStrokeContext &context, TerrainStrokePlan &plan, std::string &error) {
	(void)context;
	(void)plan;
	error = "Sculpting the heights is not ready yet in this build.";
	return false;
}

} // namespace opennova::editor
