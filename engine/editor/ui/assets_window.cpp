#include <editor/ui/assets_window.h>

#include <editor/assets/asset_kind.h>

#include <imgui.h>

namespace opennova::editor {

void AssetsWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = host_.view();
	if (!v.project_open) {
		ImGui::TextDisabled("No project open.");
		return;
	}
	if (ImGui::Button("Refresh")) host_.request(make_request(EditorRequestKind::Rescan));
	ImGui::SameLine();
	if (ImGui::Button("Show in folder")) host_.request(make_request(EditorRequestKind::RevealPath, v.project_root));
	ImGui::SameLine();
	ImGui::TextDisabled("%zu file(s)", v.scan.entries.size());
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
	                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("files", 3, flags)) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
	ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 2.0f);
	ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableHeadersRow();
	for (const AssetEntry &entry : v.scan.entries) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(entry.relative_path.c_str());
		if (ImGui::IsItemHovered() && entry.logical_name != entry.relative_path) {
			ImGui::SetTooltip("The game sees it as %s", entry.logical_name.c_str());
		}
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(asset_kind_label(entry.kind));
		ImGui::TableNextColumn();
		if (entry.size_bytes < 1024) {
			ImGui::Text("%llu B", static_cast<unsigned long long>(entry.size_bytes));
		} else {
			ImGui::Text("%.1f KB", static_cast<double>(entry.size_bytes) / 1024.0);
		}
	}
	ImGui::EndTable();
}

} // namespace opennova::editor
