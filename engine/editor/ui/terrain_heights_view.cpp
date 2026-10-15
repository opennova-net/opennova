#include "terrain_heights_view.h"

#include <cstdio>
#include <string>

#include <imgui.h>

#include <editor/documents/terrain_heights_document.h>
#include <editor/project/project_files.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

std::string units(uint16_t raw) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f", raw / 256.0);
	return text;
}

class TerrainHeightsView final : public DocumentView {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override {
		(void)workspace;
		// The heights hold no records: a reveal sent to them shows nothing more than the tab itself.
		take_events();
		const auto *heights = dynamic_cast<const TerrainHeightsDocument *>(&document);
		if (!heights) return;
		ImGui::TextUnformatted(basename_of(document.path()).c_str());
		ui_kit::tooltip(document.path() + "\nThe terrain's heights, " + heights_form_words(heights->heights().form) +
		                ", 1024 x 1024, a height a world unit across.");
		if (heights->plane_count() < 2) {
			ImGui::TextWrapped("The editor reads no heights in this file.");
			return;
		}
		// The facts walk the million heights: made again only when the document moves.
		if (identity_ != document.identity() || generation_ != document.load_generation() ||
		    revision_ != document.revision()) {
			identity_ = document.identity();
			generation_ = document.load_generation();
			revision_ = document.revision();
			facts_ = heights->facts();
		}
		const HeightsState &state = heights->heights();
		if (state.form == HeightsForm::Cpt)
			ImGui::TextWrapped("Sculpted in place: each stroke's ground is baked again by TrnGen's bake over the stretch it "
			                   "reaches, and every other tile of the file keeps its bytes.");
		else
			ImGui::TextWrapped("An imported terrain's heightmap (%s): sculpted here, made into the terrain's .cpt by its "
			                   "import as you save.",
			                   heights_form_words(state.form));
		ImGui::TextWrapped("Heights from %s to %s units; %zu tiles.", units(facts_.lowest).c_str(),
		                   units(facts_.highest).c_str(), facts_.tiles);
		if (facts_.over_rasterizer)
			ImGui::TextWrapped("%zu heights stand past 128 units, where TrnGen's bake draws no ground back.",
			                   facts_.over_rasterizer);
		if (facts_.steep_blocks)
			ImGui::TextWrapped("%d stretches of 256 heights rise past what the .cpt holds (32,766 raw): written clamped.",
			                   facts_.steep_blocks);
		if (facts_.unbaked)
			ImGui::TextWrapped("%zu regions wait for their bake: the ground there is drawn from the sculpted heights "
			                   "until it lands.",
			                   facts_.unbaked);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("Sculpt them in a mission's view or the terrain's: Terrain on the toolbar, or Ctrl+E.");
		ImGui::PopStyleColor();
	}

private:
	uint64_t identity_ = 0, generation_ = 0, revision_ = ~uint64_t(0);
	HeightsFacts facts_;
};

} // namespace

std::unique_ptr<DocumentView> make_terrain_heights_view() { return std::make_unique<TerrainHeightsView>(); }

} // namespace opennova::editor
