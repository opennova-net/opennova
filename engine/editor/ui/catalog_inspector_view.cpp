#include <editor/ui/catalog_inspector_view.h>

#include <string>

#include <base/gameprofile/required_resources.h>
#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/model_placement.h>
#include <editor/preview/viewport_model.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The HUD layout the game loads by its name (the required resource of the role hudpos_def) [orig:
// HUD_InitOverlaySystem @ 0x5A4620 (hudpos.def @ 0x5A4931)]: the project's file of that name ("" none).
std::string hud_layout_of(const SessionView &view) {
	const gameprofile::RequiredResource *layout = gameprofile::gameprofile_required_resource_by_role("hudpos_def");
	if (!view.project.scan || !layout) return std::string();
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.kind == AssetKind::HudPosDefs && strutil::iequals(entry.logical_name, layout->name))
			return entry.relative_path;
	return std::string();
}

} // namespace

bool places_in_mission(const Document &document, const NodeAddress &record) {
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	const Node *row = catalog && record.row && !record.child ? catalog->row(record.row) : nullptr;
	return row && static_cast<const CatalogRow &>(*row).record_kind() == def::DefRecordKind::Item;
}

void draw_place_in_mission(Workspace &workspace, const Document &document, const NodeAddress &record, bool menu) {
	const SessionView &view = workspace.view();
	const std::string mission = place_in_mission_target(view);
	const bool can = !mission.empty() && view.allows(EditorRequestKind::EditInViewport);
	const std::string name = document.record_name(record);
	const std::string tip = can ? "Arm the Place tool of " + basename_of(mission) + " with " + name +
	                                      ": each click on its picture places one on the ground."
	                            : std::string("Open the mission to place it in (with several open, make that one active "
	                                          "first).");
	bool pressed = false;
	if (menu) {
		pressed = ImGui::MenuItem("Place in mission", nullptr, false, can) && can;
		ui_kit::tooltip(tip);
	} else {
		ui_kit::WrapRow row;
		pressed = ui_kit::tool(row, "Place in mission", can, tip);
	}
	if (!pressed) return;
	ViewportCommand command;
	command.name = "place_in_mission";
	command.kind = ViewportKind::Definition;
	command.ids = {record.row};
	workspace.request(request::edit_in_viewport(document.path(), std::move(command)));
}

void draw_catalog_row_menu(Workspace &workspace, const Document &document, const NodeAddress &record) {
	if (places_in_mission(document, record)) draw_place_in_mission(workspace, document, record, true);
}

bool draw_catalog_inspector(Workspace &workspace, const Document &document, const NodeAddress &record,
		InspectorTaken &) {
	if (places_in_mission(document, record)) {
		draw_place_in_mission(workspace, document, record, false);
		return true;
	}
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
