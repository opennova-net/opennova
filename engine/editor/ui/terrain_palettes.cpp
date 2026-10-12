#include <editor/ui/terrain_pane_parts.h>

#include <imgui.h>

namespace opennova::editor {

// The maps' and the tiles' parts of the Terrain pane (S25 lane B): not drawn yet in this build.
void draw_paint_part(TerrainPaneContext &context) {
	(void)context;
	ImGui::TextDisabled("Painting the maps is not ready yet in this build.");
}

void draw_tiles_part(TerrainPaneContext &context) {
	(void)context;
	ImGui::TextDisabled("The tile tools are not ready yet in this build.");
}

} // namespace opennova::editor
