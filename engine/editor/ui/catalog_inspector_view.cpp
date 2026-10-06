#include <editor/ui/catalog_inspector_view.h>

#include <string>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

// The HUD layout the game loads by its name [orig: HUD_InitOverlaySystem @ 0x5A4620 (hudpos.def @
// 0x5A4931)]: the project's file of that name ("" none).
std::string hud_layout_of(const SessionView &view) {
	if (!view.project.scan) return std::string();
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == AssetKind::HudPosDefs && strutil::iequals(entry.logical_name, "hudpos.def"))
			return entry.relative_path;
	return std::string();
}

} // namespace

bool draw_catalog_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
		InspectorTaken &) {
	if (document.kind() != AssetKind::WeaponDefs || !record.row || record.child) return false;
	const std::string weapon = document.record_name(record);
	if (weapon.empty()) return false;
	const std::string layout = hud_layout_of(workspace.view());
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Show on the HUD", !layout.empty(),
	                 layout.empty() ? std::string("The project has no hudpos.def: the HUD it lays out shows there.")
	                                : "Open " + layout + "'s HUD preview with " + weapon +
	                                          " held: its name, ammo count, clip and round art and icon as the game "
	                                          "draws them.")) {
		ReferenceTarget target;
		target.file = layout;
		target.editable = true;
		window_requests::go_to(workspace, target);
		io::JsonValue options = io::JsonValue::make_object();
		options.set("weapon", io::json_string(weapon));
		workspace.request(request::set_viewport(layout, viewport_change(ViewportKind::Hud, "options", std::move(options))));
	}
	return true;
}

} // namespace opennova::editor
