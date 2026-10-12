#include <editor/ui/terrain_pane.h>

#include <iterator>
#include <string>

#include <editor/documents/mission_document.h>
#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_follow.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/terrain_pane_parts.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The tools of each tab, in their order.
constexpr TerrainTool kSculpt[] = {TerrainTool::Raise, TerrainTool::Lower, TerrainTool::Smooth, TerrainTool::Flatten, TerrainTool::Set};
constexpr TerrainTool kPaint[] = {TerrainTool::Surface, TerrainTool::Foliage, TerrainTool::Colour, TerrainTool::Detail};
constexpr TerrainTool kTiles[] = {TerrainTool::Stamp, TerrainTool::Tiles};

// The brush's tool set (one set_workspace), None for the tool pressed again.
void set_tool(Workspace &workspace, TerrainTool tool) {
	window_requests::set_workspace(workspace, "terrain_brush", "tool", io::JsonValue::make_string(terrain_tool_token(tool)));
}

template <size_t N>
void draw_tools(Workspace &workspace, const TerrainBrush &brush, const TerrainTool (&tools)[N]) {
	ui_kit::WrapRow row;
	for (const TerrainTool tool : tools) {
		const bool active = brush.tool == tool;
		if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		const bool pressed = ui_kit::tool(row, terrain_tool_label(tool), true,
		                                  active ? std::string("Put the brush down (no tool): the view's own tools again.")
		                                         : std::string("Brush with ") + terrain_tool_label(tool) + ".");
		if (active) ImGui::PopStyleColor();
		if (pressed) set_tool(workspace, active ? TerrainTool::None : tool);
	}
}

// What a stroke of the heights moves under the brush (S25, D2; the terrain query's lock op): the entities within its
// radius of where it meets the ground under the pointer (else of the cursor an MCP client placed) in the view's
// mission, or in the open missions whose ground the view's heights are (as each routes its own strokes): those that
// follow the ground and those height locked.
void draw_lock_line(TerrainPaneContext &context) {
	const SessionView &view = context.workspace.view();
	const std::vector<const MissionDocument *> missions = missions_under_view(view, context.document, context.family);
	if (missions.empty()) {
		ImGui::TextDisabled("No open mission stands on this terrain: a sculpt moves no entity open here.");
		return;
	}
	double x = 0.0, y = 0.0;
	bool at = context.workspace.brush_ground(context.document.path(), x, y);
	if (!at && context.brush.has_cursor) {
		x = context.brush.cursor[0];
		y = context.brush.cursor[1];
		at = true;
	}
	if (!at) {
		ImGui::TextDisabled("Point the brush at the ground: what a sculpt there moves shows here.");
		return;
	}
	size_t follow = 0, locked = 0;
	for (const MissionDocument *mission : missions) {
		const GroundFollowers under = ground_followers(*mission, x, y, double(context.brush.radius));
		follow += under.follow;
		locked += under.locked;
	}
	// Where it counts: the view's own mission, or the open missions on the view's terrain (a sculpt's follow moves every
	// mission on the heights, closed ones and those of another terrain naming the .cpt among them).
	const bool mission = dynamic_cast<const MissionDocument *>(&context.document) != nullptr;
	const char *where = mission ? "in this mission" : "in the open missions on it";
	if (follow + locked == 0) {
		ImGui::TextDisabled("Under the brush %s: no entity.", where);
	} else {
		const std::string follows = std::to_string(follow) + (follow == 1 ? " entity follows" : " entities follow");
		const std::string held = std::to_string(locked) + (locked == 1 ? " is" : " are");
		ImGui::TextWrapped("Under the brush %s: %s the ground, %s height locked.", where, follows.c_str(), held.c_str());
	}
	ui_kit::tooltip("A sculpt moves an entity that follows the ground with the ground under it, in every mission on these "
	                "heights; a height-locked one keeps its height (its Height lock, beside its z).");
}

// One of the brush's numbers as a slider, set once as the drag lets go.
void brush_slider(Workspace &workspace, const char *label, const char *member, float value, float least, float most,
                  const char *format, const char *tip) {
	float held = value;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	ImGui::SliderFloat(label, &held, least, most, format);
	ui_kit::tooltip(tip);
	if (ImGui::IsItemDeactivatedAfterEdit())
		window_requests::set_workspace(workspace, "terrain_brush", member, io::JsonValue::make_number(double(held)));
}

class TerrainPane final : public DocumentPane {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override {
		const SessionView &view = workspace.view();
		const TerrainBrush &brush = view.workspace.terrain_brush;
		TerrainFamily family;
		resolve_terrain_family(view, document, family);
		TerrainPaneContext context{workspace, document, family, brush};
		ImGui::TextUnformatted("Terrain");
		if (!family.error.empty()) ImGui::TextWrapped("%s", family.error.c_str());
		// The files a Save of this view writes beside it that keep unsaved edits, each with its dirty dot: the terrain
		// files its brush edited and the missions that followed their strokes (its Save saves them with the view's
		// document; its Close asks about them).
		{
			std::vector<std::string> scope;
			if (view.documents.active == document.path()) {
				scope = view.documents.active_scope_unsaved;
			} else if (const auto edited = view.documents.edited_from.find(document.path());
			           edited != view.documents.edited_from.end()) {
				for (const std::string &path : edited->second)
					for (const auto &open : view.documents.open)
						if (open->path() == path && open->unsaved()) scope.push_back(path);
			}
			std::string unsaved;
			for (const std::string &path : scope) unsaved += (unsaved.empty() ? "" : ", ") + basename_of(path) + " *";
			if (!unsaved.empty()) {
				ImGui::TextWrapped("Unsaved: %s", unsaved.c_str());
				ui_kit::tooltip("Saving this view (Ctrl+S) saves them with it; closing it asks about them.");
			}
		}
		// Where the edits go and why.
		TerrainPlane plane = TerrainPlane::Heights;
		if (terrain_tool_plane(brush.tool, plane)) {
			const TerrainFamilyFile &file = family.plane(plane);
			const std::string words = !file.refusal.empty() ? file.refusal : !file.words.empty() ? file.words : file.path;
			ImGui::TextWrapped("%s", words.c_str());
		} else if (brush.tool == TerrainTool::None) {
			ImGui::TextDisabled("No tool: pick one to brush in the view.");
		} else if (!family.tiles.words.empty()) {
			ImGui::TextWrapped("Tiles go to %s.", family.tiles.words.c_str());
		}
		// The tab of the brush's tool comes forward as the tool moves to another family (a tab looked at stays). A tab
		// pressed picks its family's first tool where the brush has another family's, or none (Paint: Surface, its
		// legend shown), so a family and its value are two clicks.
		const TerrainToolFamily shown = terrain_tool_family(brush.tool);
		const bool moved = shown != last_ && shown != TerrainToolFamily::None;
		last_ = shown;
		if (ImGui::BeginTabBar("terrain_tools")) {
			// A tab's body scrolls under its tab bar, which stays in sight (a legend or a palette taller than the pane
			// is reached with the wheel, its tabs still a click away).
			const auto tab = [&](TerrainToolFamily each, TerrainTool first, auto draw) {
				const ImGuiTabItemFlags flags = moved && shown == each ? ImGuiTabItemFlags_SetSelected : 0;
				if (ImGui::BeginTabItem(terrain_tool_family_label(each), nullptr, flags)) {
					if (selected_ != TerrainToolFamily::None && selected_ != each && !moved && shown != each)
						set_tool(workspace, first);
					selected_ = each;
					if (ImGui::BeginChild("terrain_tab_body")) draw();
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
			};
			tab(TerrainToolFamily::Sculpt, TerrainTool::Raise, [&] {
				draw_tools(workspace, brush, kSculpt);
				draw_brush_part(context);
				draw_sculpt_part(context);
				draw_lock_line(context);
				draw_bake_part(context);
			});
			tab(TerrainToolFamily::Paint, TerrainTool::Surface, [&] {
				draw_tools(workspace, brush, kPaint);
				draw_brush_part(context);
				draw_paint_part(context);
			});
			tab(TerrainToolFamily::Tiles, TerrainTool::Stamp, [&] {
				draw_tools(workspace, brush, kTiles);
				draw_tiles_part(context);
			});
			ImGui::EndTabBar();
		}
	}

private:
	TerrainToolFamily last_ = TerrainToolFamily::None;
	TerrainToolFamily selected_ = TerrainToolFamily::None; // the tab drawn selected last
};

} // namespace

void draw_brush_part(TerrainPaneContext &context) {
	const TerrainBrush &brush = context.brush;
	brush_slider(context.workspace, "Radius", "radius", brush.radius, 0.0f, kTerrainRadiusMost, "%.1f m",
	             "The brush's radius, metres ([ and ] in the view; 0: one texel, the original editor's click).");
	brush_slider(context.workspace, "Strength", "strength", brush.strength, 0.0f, 1.0f, "%.2f",
	             "How much a dab changes (Shift+[ and Shift+] in the view).");
	brush_slider(context.workspace, "Falloff", "falloff", brush.falloff, 0.0f, 1.0f, "%.2f",
	             "The share of the radius the brush fades over to its rim.");
	brush_slider(context.workspace, "Spacing", "spacing", brush.spacing, kTerrainSpacingLeast, kTerrainSpacingMost, "%.2f",
	             "A dab every spacing x radius along the pointer's path on the ground.");
}

std::unique_ptr<DocumentPane> make_terrain_pane() { return std::make_unique<TerrainPane>(); }

} // namespace opennova::editor
