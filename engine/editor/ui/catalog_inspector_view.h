#pragma once

#include <editor/model/document.h>
#include <editor/ui/document_views.h>
#include <editor/ui/workspace.h>

namespace opennova::editor {

// A catalog's part heading the Inspector (the plan's DI-20): over a weapon of weapon.def, Show on the
// HUD, which goes to the project's HUD layout (window_requests::go_to: hudpos.def opened, its HUD the
// Preview window's) with the weapon held there (a SetViewport of the HUD viewport's `weapon`), so its
// name, ammo count, clip and round art and silhouette show as the game's HUD draws them; over an item record
// (DI-18), Place in mission (draw_place_in_mission). Takes nothing of the generic form; false (nothing drawn)
// over any other record.
bool draw_catalog_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
		InspectorTaken &taken);

// An item record's Place in mission (DI-18, DI-12's model twin): whether `record` of `document` is one a
// mission places (a catalog row the parser makes an item of), and the request that arms the Place tool of the
// mission last active with it: the definition viewport's place_in_mission command over that row, an
// EditInViewport, so the wire's is the same request. The tool over the Inspector and the catalog line's menu
// item (`menu`) say where it arms Place, or why it cannot.
bool places_in_mission(const Document &document, const NodeAddress &record);
void draw_place_in_mission(Workspace &workspace, const Document &document, const NodeAddress &record, bool menu);
// The catalog's line menu items (OutlineSpec::row_menu): Place in mission over an item record.
void draw_catalog_row_menu(Workspace &workspace, const Document &document, const NodeAddress &record);

} // namespace opennova::editor
