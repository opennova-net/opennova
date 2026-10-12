#include <editor/ui/terrain_pane_parts.h>

#include <imgui.h>

namespace opennova::editor {

// The heights' parts of the Terrain pane (S25 lane A): not drawn yet in this build.
void draw_sculpt_part(TerrainPaneContext &context) {
	(void)context;
	ImGui::TextDisabled("Sculpting is not ready yet in this build.");
}

void draw_bake_part(TerrainPaneContext &context) { (void)context; }

} // namespace opennova::editor
