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
		const HeightsState &state = heights->heights();
		// The file's name, what it is and how it is sculpted in its tooltip; then one numbers line and the warnings.
		ImGui::TextUnformatted(basename_of(document.path()).c_str());
		ui_kit::tooltip(document.path() + "\nThe terrain's heights, " + heights_form_words(state.form) +
		                ", 1024 x 1024, a height a world unit across. " +
		                (state.form == HeightsForm::Cpt
		                         ? "Sculpted in place: each stroke's ground is baked again by TrnGen's bake over the stretch "
		                           "it reaches, and every other tile keeps its bytes."
		                         : "An imported terrain's heightmap: made into the terrain's .cpt by its import as you save.") +
		                "\nSculpt them in a mission's view or the terrain's: Terrain on the toolbar, or Ctrl+E.");
		if (heights->plane_count() < 2) {
			ui_kit::refusal_text("The editor reads no heights in this file.");
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
		ui_kit::clipped_text("Heights from " + units(facts_.lowest) + " to " + units(facts_.highest) + " units; " +
		                     std::to_string(facts_.tiles) + " tiles.");
		if (facts_.over_rasterizer)
			ui_kit::warning_line(std::to_string(facts_.over_rasterizer) + " heights stand past 128 units",
			                     "TrnGen's bake draws no ground back there.");
		if (facts_.steep_blocks)
			ui_kit::warning_line(std::to_string(facts_.steep_blocks) + " stretches written clamped",
			                     "Stretches of 256 heights rise past what the .cpt holds (32,766 raw).");
		if (facts_.unbaked)
			ui_kit::quiet_text(std::to_string(facts_.unbaked) + " regions wait for their bake",
			                   "The ground there is drawn from the sculpted heights until it lands.");
	}

private:
	uint64_t identity_ = 0, generation_ = 0, revision_ = ~uint64_t(0);
	HeightsFacts facts_;
};

} // namespace

std::unique_ptr<DocumentView> make_terrain_heights_view() { return std::make_unique<TerrainHeightsView>(); }

} // namespace opennova::editor
