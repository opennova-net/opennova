#include <editor/preview/terrain_view_commands.h>

namespace opennova::editor {

// The tile tools' planners' stand-ins (S25): lane B's preview/tile_tools defines the planners and deletes this file.
bool plan_tile_command(const TerrainViewContext &view, const ViewportCommand &command, CanvasRequests &out,
                       std::string &error) {
	(void)view;
	(void)out;
	error = "The tile tools are not ready yet in this build (" + command.name + ").";
	return false;
}

bool plan_tile_drop(const TerrainViewContext &view, const ViewportDrop &drop, CanvasRequests &out, std::string &error) {
	(void)view;
	(void)drop;
	(void)out;
	error = "Stamping tiles is not ready yet in this build.";
	return false;
}

bool plan_tile_drag(const TerrainViewContext &view, const ViewportDrag &drag, CanvasRequests &out, std::string &error) {
	(void)view;
	(void)drag;
	(void)out;
	error = "Moving tiles is not ready yet in this build.";
	return false;
}

bool plan_tile_select(const TerrainViewContext &view, const ViewportCommand &click, CanvasRequests &out, std::string &error) {
	(void)view;
	(void)click;
	(void)out;
	error = "Selecting tiles is not ready yet in this build.";
	return false;
}

} // namespace opennova::editor
