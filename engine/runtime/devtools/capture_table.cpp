#include <runtime/devtools/capture_table.h>

#include <runtime/devtools/ui_kit.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

std::string capture_row_text(const CaptureCount &count) {
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%s: held %d / total %llu", count.name != nullptr ? count.name : "?",
			count.held, static_cast<unsigned long long>(count.total));
	return buf;
}

bool capture_mask_table(const char *id, uint32_t &mask, uint32_t all, const CaptureCount *counts,
		const float (*colors)[3], int count) {
	bool changed = false;
	ImGui::PushID(id);
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "All", true, "Draw every kind in the Game view.", true)) {
		mask = all;
		changed = true;
	}
	if (ui_kit::tool(row, "None", true, "Draw none of them.", true)) {
		mask = 0;
		changed = true;
	}
	if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
		const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
		for (int i = 0; i < count; ++i) {
			const CaptureCount &c = counts[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(i);
			bool on = (mask & (1u << i)) != 0;
			if (ImGui::Checkbox("##draw", &on)) {
				mask = on ? (mask | (1u << i)) : (mask & ~(1u << i));
				changed = true;
			}
			ImGui::SameLine();
			ImGui::ColorButton("##swatch", ImVec4(colors[i][0], colors[i][1], colors[i][2], 1.0f),
					ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, swatch_size);
			ImGui::SameLine();
			ImGui::TextUnformatted(c.name != nullptr ? c.name : "?");
			ImGui::TableNextColumn();
			ImGui::Text("held %d / total %llu", c.held, static_cast<unsigned long long>(c.total));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::PopID();
	return changed;
}

}  // namespace opennova::devtools
