#include <editor/ui/terrain_pane.h>

#include <iterator>
#include <string>

#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/session/terrain_family.h>
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
		// The terrain files this view's brush edited that keep unsaved edits, each with its dirty dot (its Save saves
		// them with the view's document; its Close asks about them).
		if (const auto edited = view.documents.edited_from.find(document.path()); edited != view.documents.edited_from.end()) {
			std::string unsaved;
			for (const std::string &path : edited->second)
				for (const auto &open : view.documents.open)
					if (open->path() == path && open->unsaved()) unsaved += (unsaved.empty() ? "" : ", ") + basename_of(path) + " *";
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
		// The tab of the brush's tool comes forward as the tool moves to another family (a tab looked at stays).
		const TerrainToolFamily shown = terrain_tool_family(brush.tool);
		const bool moved = shown != last_ && shown != TerrainToolFamily::None;
		last_ = shown;
		if (ImGui::BeginTabBar("terrain_tools")) {
			const auto tab = [&](TerrainToolFamily each, auto draw) {
				const ImGuiTabItemFlags flags = moved && shown == each ? ImGuiTabItemFlags_SetSelected : 0;
				if (ImGui::BeginTabItem(terrain_tool_family_label(each), nullptr, flags)) {
					draw();
					ImGui::EndTabItem();
				}
			};
			tab(TerrainToolFamily::Sculpt, [&] {
				draw_tools(workspace, brush, kSculpt);
				draw_brush_part(context);
				draw_sculpt_part(context);
				draw_bake_part(context);
			});
			tab(TerrainToolFamily::Paint, [&] {
				draw_tools(workspace, brush, kPaint);
				draw_brush_part(context);
				draw_paint_part(context);
			});
			tab(TerrainToolFamily::Tiles, [&] {
				draw_tools(workspace, brush, kTiles);
				draw_tiles_part(context);
			});
			ImGui::EndTabBar();
		}
	}

private:
	TerrainToolFamily last_ = TerrainToolFamily::None;
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
