#include <editor/ui/terrain_pane_parts.h>

#include <string>

#include <editor/documents/terrain_heights_document.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// The open heights document the view's brush sculpts (null: not open, none resolves).
const TerrainHeightsDocument *open_heights(const TerrainPaneContext &context) {
	const std::string &path = context.family.plane(TerrainPlane::Heights).path;
	if (path.empty()) return nullptr;
	for (const auto &open : context.workspace.view().documents.open)
		if (open && open->path() == path) return dynamic_cast<const TerrainHeightsDocument *>(open.get());
	return nullptr;
}

double metres(uint16_t raw) { return double(raw) / 256.0; }

} // namespace

// The sculpt tools' values and the heights they change (S25 lane A): Set's height, the heights' range, and what
// the game and TrnGen's bake make of the heights as they stand (the draw-back's 128-unit limit, the depth
// section's steep blocks).
void draw_sculpt_part(TerrainPaneContext &context) {
	const TerrainBrush &brush = context.brush;
	if (brush.tool == TerrainTool::Set) {
		float held = brush.height;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		ImGui::SliderFloat("Height", &held, 0.0f, 255.99f, "%.2f m");
		ui_kit::tooltip("The height Set lays (strength 1: an exact plateau), metres over the terrain's zero.");
		if (ImGui::IsItemDeactivatedAfterEdit())
			window_requests::set_workspace(context.workspace, "terrain_brush", "height", io::JsonValue::make_number(double(held)));
	} else if (brush.tool == TerrainTool::Flatten) {
		ImGui::TextDisabled("Flatten levels towards the height under the stroke's first dab.");
	}
	const TerrainHeightsDocument *heights = open_heights(context);
	if (!heights) {
		const TerrainFamilyFile &file = context.family.plane(TerrainPlane::Heights);
		if (!file.path.empty() && file.refusal.empty())
			ImGui::TextDisabled("%s opens as you sculpt.", basename_of(file.path).c_str());
		return;
	}
	if (heights->blocked()) {
		ImGui::TextWrapped("%s is open read only: %s", basename_of(heights->path()).c_str(),
		                   heights->issues().empty() ? "its heights do not read." : heights->issues().front().message.c_str());
		return;
	}
	const HeightsFacts facts = heights->facts();
	ImGui::Text("Heights %.2f m to %.2f m, %zu tiles", metres(facts.lowest), metres(facts.highest), facts.tiles);
	if (facts.over_rasterizer) {
		ImGui::TextWrapped("%zu texels stand past 128 m: TrnGen's bake leaves them out of the terrain it draws.",
		                   facts.over_rasterizer);
		ui_kit::tooltip("The bake's draw-back covers 0 to 128 m; a height past it keeps its old atlas value.");
	}
	if (facts.steep_blocks) {
		ImGui::TextWrapped("%d blocks of the depth section span more than it holds: the writer clamps them.",
		                   facts.steep_blocks);
	}
}

// The heights file's bake (S25 lane A): running, the regions it waits on, or done.
void draw_bake_part(TerrainPaneContext &context) {
	const SessionView &view = context.workspace.view();
	const TerrainHeightsDocument *heights = open_heights(context);
	if (!heights) {
		// Prepared before the file opens (Sculpt picked over the terrain).
		const std::string &path = context.family.plane(TerrainPlane::Heights).path;
		if (!path.empty() && view.documents.preparing == path)
			ImGui::Text("Preparing %s for sculpting...", basename_of(path).c_str());
		return;
	}
	if (heights->blocked()) return;
	const std::string name = basename_of(heights->path());
	if (view.documents.preparing == heights->path()) {
		ImGui::Text("Preparing %s for sculpting...", name.c_str());
		ui_kit::tooltip("Its bake state is made once, in the background: strokes go on meanwhile, their bake after it.");
	} else if (view.documents.baking == heights->path()) {
		ImGui::Text("Baking %s...", name.c_str());
	} else if (const size_t due = heights->heights().unbaked.size()) {
		ImGui::TextWrapped("%zu %s for the bake (it starts as the stroke ends).", due,
		                   due == 1 ? "region waits" : "regions wait");
	} else if (heights->heights().baked) {
		ImGui::TextDisabled("%s baked: every tile as TrnGen's bake makes it of these heights.", name.c_str());
	} else {
		ImGui::TextDisabled("Not baked yet: the import's terrain stands until the first stroke's bake.");
	}
	ui_kit::tooltip("A stroke's bake remakes only the tiles it reached; Save, Play and Build wait for it.");
}

} // namespace opennova::editor
