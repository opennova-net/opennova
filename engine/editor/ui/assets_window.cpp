#include <editor/ui/assets_window.h>

#include <editor/assets/asset_kind.h>
#include <algorithm>
#include <filesystem>

#include <imgui.h>

namespace opennova::editor {

void AssetsWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = host_.view();
	if (!v.project_open) {
		ImGui::TextDisabled("No project open.");
		return;
	}
	draw_import();
	if (ImGui::Button("Import...")) {
		EditorRequest pick = make_request(EditorRequestKind::PickFile);
		pick.purpose = PickPurpose::ImportFiles;
		host_.request(pick);
	}
	ImGui::SameLine();
	if (ImGui::Button("Refresh")) host_.request(make_request(EditorRequestKind::Rescan));
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
		if (ImGui::Selectable(entry.relative_path.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
			ImGui::IsMouseDoubleClicked(0) && is_catalog_kind(entry.kind))
			host_.request(make_request(EditorRequestKind::OpenDocument, entry.relative_path));
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

void AssetsWindow::draw_import() {
	const auto &v = host_.view();
	if (v.import_open && !ImGui::IsPopupOpen("Import files")) {
		selected_.clear();
		for (const auto &source : v.import_sources) selected_.push_back(source.entry.empty());
		filter_[0] = '\0';
		replace_existing_ = false;
		ImGui::OpenPopup("Import files");
	}
	ImGui::SetNextWindowSize(ImVec2(760, 540), ImGuiCond_FirstUseEver);
	if (!ImGui::BeginPopupModal("Import files")) return;
	if (!v.import_open) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
	ImGui::TextUnformatted("Choose files to copy into the project.");
	ImGui::InputText("Filter", filter_, sizeof(filter_));
	std::vector<size_t> visible;
	const std::string filter = normalized_logical_name(filter_);
	for (size_t i = 0; i < v.import_sources.size(); ++i)
		if (normalized_logical_name(v.import_sources[i].name()).find(filter) != std::string::npos)
			visible.push_back(i);
	if (ImGui::Button("Select shown")) for (size_t i : visible) selected_[i] = true;
	ImGui::SameLine();
	if (ImGui::Button("Clear selection")) std::fill(selected_.begin(), selected_.end(), false);
	if (ImGui::BeginTable("import_choices", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg,
	                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 3))) {
		ImGui::TableSetupColumn("File");
		ImGui::TableSetupColumn("Source");
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(visible.size()));
		while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
			const size_t index = visible[static_cast<size_t>(row)];
			const auto &source = v.import_sources[index];
			ImGui::PushID(static_cast<int>(index));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			bool selected = selected_[index];
			if (ImGui::Checkbox(source.name().c_str(), &selected)) selected_[index] = selected;
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(std::filesystem::path(source.path).filename().string().c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", source.path.c_str());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::Checkbox("Replace existing files", &replace_existing_);
	const auto count = std::count(selected_.begin(), selected_.end(), true);
	ImGui::Text("%zu selected", static_cast<size_t>(count));
	ImGui::SameLine();
	ImGui::BeginDisabled(count == 0);
	if (ImGui::Button("Import selected")) {
		EditorRequest request = make_request(EditorRequestKind::ImportFiles);
		request.flag = replace_existing_;
		for (size_t i = 0; i < selected_.size(); ++i)
			if (selected_[i]) request.imports.push_back(v.import_sources[i]);
		host_.request(std::move(request));
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		host_.request(make_request(EditorRequestKind::CancelImport));
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

} // namespace opennova::editor
