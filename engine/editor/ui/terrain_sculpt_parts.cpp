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
	}
	const TerrainHeightsDocument *heights = open_heights(context);
	if (!heights) {
		const TerrainFamilyFile &file = context.family.plane(TerrainPlane::Heights);
		if (!file.path.empty() && file.refusal.empty())
			ImGui::TextDisabled("%s opens as you sculpt.", basename_of(file.path).c_str());
		return;
	}
	if (heights->blocked()) {
		ui_kit::refusal_text(basename_of(heights->path()) + " is open read only: " +
		                     (heights->issues().empty() ? "its heights do not read." : heights->issues().front().message));
		return;
	}
	const HeightsFacts facts = heights->facts();
	ImGui::Text("Heights %.2f m to %.2f m, %zu tiles", metres(facts.lowest), metres(facts.highest), facts.tiles);
	if (facts.over_rasterizer)
		ui_kit::warning_line(std::to_string(facts.over_rasterizer) + " texels stand past 128 m",
		                     "TrnGen's bake leaves them out of the terrain it draws: its draw-back covers 0 to 128 m, and a "
		                     "height past it keeps its old atlas value.");
	if (facts.steep_blocks)
		ui_kit::warning_line(std::to_string(facts.steep_blocks) + " depth blocks written clamped",
		                     "These blocks of the depth section span more than it holds: the writer clamps them.");
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
	const std::string bakes = "A stroke's bake remakes only the tiles it reached; Save, Play and Build wait for it.";
	if (view.documents.preparing == heights->path()) {
		ImGui::Text("Preparing %s for sculpting...", name.c_str());
		ui_kit::tooltip("Its bake state is made once, in the background: strokes go on meanwhile, their bake after it.");
	} else if (view.documents.baking == heights->path()) {
		ImGui::Text("Baking %s...", name.c_str());
		ui_kit::tooltip(bakes);
	} else if (const size_t due = heights->heights().unbaked.size()) {
		ui_kit::clipped_text(std::to_string(due) + (due == 1 ? " region waits" : " regions wait") + " for the bake.",
		                     "It starts as the stroke ends. " + bakes);
	} else if (heights->heights().baked) {
		ui_kit::quiet_text(name + " baked.", "Every tile as TrnGen's bake makes it of these heights. " + bakes);
	} else {
		ui_kit::quiet_text("Not baked yet.", "The import's terrain stands until the first stroke's bake. " + bakes);
	}
}

} // namespace opennova::editor
