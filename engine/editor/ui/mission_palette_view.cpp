#include <editor/ui/mission_palette_view.h>

#include <string>

#include <imgui.h>

#include <editor/graph/asset_graph.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

std::string file_name(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

std::string MissionPaletteView::name_of(int64_t item) const {
	for (const MissionPaletteItem &each : palette_.items)
		if (each.item == item) return each.name;
	return std::string();
}

int64_t MissionPaletteView::draw(const AssetGraph *graph, uint64_t generation, const std::vector<int64_t> &recent,
		int64_t picked, const std::string &search, std::string *typed) {
	filter_.follow(search);
	if (ui_kit::filter_box("##palette_filter", filter_.text, sizeof(filter_.text), "Search items", 0.0f,
			"Find an item by its name, its id or its model's file.", false) && typed)
		*typed = filter_.sent();
	if (!graph) {
		ui_kit::empty_state("No item catalog yet.", "The project's items.def lists what can be placed.");
		return 0;
	}
	// Made again only when what it reads moved.
	const std::string filter = filter_.sent();
	if (!made_ || generation_ != generation || made_filter_ != filter || made_recent_ != recent) {
		palette_ = mission_palette(*graph, filter, recent);
		made_ = true;
		generation_ = generation;
		made_filter_ = filter;
		made_recent_ = recent;
	}
	if (palette_.count == 0) {
		ui_kit::empty_state("No items to place.", "Import the game's items.def (or add one) to place soldiers, vehicles "
												  "and buildings.");
		return 0;
	}
	if (palette_.items.empty()) {
		ui_kit::empty_state("No item matches the search.");
		return 0;
	}
	int64_t chosen = 0;
	if (ImGui::BeginChild("palette_items", ImVec2(0.0f, 0.0f))) {
		const float muted_gap = ImGui::GetStyle().ItemInnerSpacing.x;
		for (const MissionPaletteSection &section : palette_.sections) {
			ImGui::PushID(int(section.group));
			const std::string header = std::string(mission_palette_group_words(section.group)) + " (" +
					std::to_string(section.items.size()) + ")###group";
			const bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
			if (section.group != MissionPaletteGroup::Recent && !section.items.empty())
				ui_kit::tooltip(std::string("Placed among the mission's ") +
						mission_pool_words(palette_.items[section.items.front()].pool) + ".");
			if (open) {
				// Rows clipped to what shows: a group may hold hundreds.
				ImGuiListClipper clipper;
				clipper.Begin(int(section.items.size()));
				while (clipper.Step()) {
					for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
						const MissionPaletteItem &item = palette_.items[section.items[size_t(row)]];
						ImGui::PushID(row);
						const std::string name = item.name.empty() ? "Item " + std::to_string(item.item) : item.name;
						const float room = ImGui::GetContentRegionAvail().x;
						const std::string shown = ui_kit::fit(name, room);
						if (ImGui::Selectable((shown + "###item").c_str(), picked == item.item)) chosen = item.item;
						if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
							const std::string id = std::to_string(item.item);
							ImGui::SetDragDropPayload(kItemDragPayload, id.c_str(), id.size() + 1);
							ImGui::TextUnformatted(name.c_str());
							ImGui::EndDragDropSource();
						}
						ui_kit::tooltip_lazy([&] {
							return name + "\nItem " + std::to_string(item.item) + ", " +
									(item.type < 0 ? std::string("TYPE unknown") : "TYPE " + std::to_string(item.type)) +
									", placed among the " + mission_pool_words(item.pool) + ".\n" +
									(item.model.empty() ? std::string("Its graphic loads no model of the project.")
														: "Model: " + item.model) +
									"\nClick to place it at each click on the picture, or drag it there.";
						});
						// The model's file, muted, where the row has room after the name.
						if (!item.model.empty()) {
							const float used = ImGui::CalcTextSize(shown.c_str()).x;
							const std::string model = file_name(item.model);
							if (room - used > ImGui::CalcTextSize(model.c_str()).x + muted_gap * 2.0f) {
								ImGui::SameLine(room - ImGui::CalcTextSize(model.c_str()).x);
								ImGui::TextDisabled("%s", model.c_str());
							}
						}
						ImGui::PopID();
					}
				}
			}
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	return chosen;
}

} // namespace opennova::editor
