#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// A catalog's part heading the Inspector (the plan's DI-20): over a weapon of weapon.def, Show on the
// HUD, which goes to the project's HUD layout (window_requests::go_to: hudpos.def opened, its HUD the
// Preview window's) with the weapon held there (a SetViewport of the HUD viewport's `weapon`), so its
// name, ammo count, clip and round art and silhouette show as the game's HUD draws them. Takes nothing
// of the generic form; false (nothing drawn) over any other record.
bool draw_catalog_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
		InspectorTaken &taken);

} // namespace opennova::editor
