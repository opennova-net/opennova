#include <editor/ui/mission_palette_view.h>

#include <algorithm>
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

namespace {

// The time a frame gives the pictures (the texture thumbnails' share of a poll).
constexpr int64_t kPicturesFrameUs = 2000;

// `picture` drawn into the square at `at`, `side` pixels, its lines fitted with a margin, in the text's colour; a
// muted square where it has none yet (or the model does not read).
void draw_picture(const MissionPalettePicture *picture, ImVec2 at, float side) {
	ImDrawList *draw = ImGui::GetWindowDrawList();
	const ImU32 frame = ImGui::GetColorU32(ImGuiCol_Border);
	draw->AddRect(at, ImVec2(at.x + side, at.y + side), frame);
	if (!picture || !picture->read || picture->lines.empty()) return;
	const float margin = 3.0f;
	const float w = std::max(picture->hi[0] - picture->lo[0], 1e-3f), h = std::max(picture->hi[1] - picture->lo[1], 1e-3f);
	const float scale = (side - 2.0f * margin) / std::max(w, h);
	// Centred, up the screen's up.
	const float ox = at.x + margin + ((side - 2.0f * margin) - w * scale) * 0.5f;
	const float oy = at.y + side - margin - ((side - 2.0f * margin) - h * scale) * 0.5f;
	const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
	const std::vector<float> &lines = picture->lines;
	for (size_t i = 0; i + 3 < lines.size(); i += 4)
		draw->AddLine(ImVec2(ox + (lines[i] - picture->lo[0]) * scale, oy - (lines[i + 1] - picture->lo[1]) * scale),
				ImVec2(ox + (lines[i + 2] - picture->lo[0]) * scale, oy - (lines[i + 3] - picture->lo[1]) * scale), colour);
}

} // namespace

int64_t MissionPaletteView::draw(const SessionView &view, const AssetGraph *graph, uint64_t generation,
		const std::vector<int64_t> &recent, int64_t picked, const std::string &search, std::string *typed) {
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
						// The row two lines high: the model's picture, then the name.
						const float side = ImGui::GetTextLineHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
						const ImVec2 at = ImGui::GetCursorScreenPos();
						const float room = ImGui::GetContentRegionAvail().x - side - muted_gap;
						const std::string shown = ui_kit::fit(name, room);
						if (ImGui::Selectable("###item", picked == item.item, 0, ImVec2(0.0f, side))) chosen = item.item;
						draw_picture(pictures_.get(view, item.model).get(), at, side);
						const ImVec2 text_at(at.x + side + muted_gap, at.y + (side - ImGui::GetTextLineHeight()) * 0.5f);
						ImGui::GetWindowDrawList()->AddText(text_at, ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
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
							const float width = ImGui::CalcTextSize(model.c_str()).x;
							if (room - used > width + muted_gap * 2.0f)
								ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + side + muted_gap + room - width, text_at.y),
										ImGui::GetColorU32(ImGuiCol_TextDisabled), model.c_str());
						}
						ImGui::PopID();
					}
				}
			}
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	// The pictures the rows asked for, a few a frame.
	pictures_.step(view, kPicturesFrameUs);
	return chosen;
}

} // namespace opennova::editor
