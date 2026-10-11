#include <editor/ui/mission_row_menu.h>

#include <editor/documents/mission_document.h>

namespace opennova::editor {

void draw_mission_row_menu(Workspace &workspace, const Document &document, const NodeAddress &record) {
	const auto *mission = dynamic_cast<const MissionDocument *>(&document);
	if (!mission) return;
	mission_logic_row_items(workspace, *mission, record);
	mission_space_row_items(workspace, *mission, record);
}

// The views' items (lane B, S24) are ui/mission_mark_menu.cpp's.

} // namespace opennova::editor
