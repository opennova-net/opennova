#include <editor/ui/problems_window.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

ImVec4 severity_color(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
	case DiagnosticSeverity::Warning: return ImVec4(0.95f, 0.80f, 0.40f, 1.0f);
	case DiagnosticSeverity::Error: return ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
	}
	return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

} // namespace

void ProblemsWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = host_.view();
	if (v.diagnostics.empty()) {
		ImGui::TextDisabled(v.project_open ? "No problems." : "No project open.");
		return;
	}
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
	                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable("problems", 3, flags)) return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch, 4.0f);
	ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 1.5f);
	ImGui::TableHeadersRow();
	// Errors first, then warnings, then notes; each group in its reported order.
	int problem_index = 0;
	for (const DiagnosticSeverity severity :
	     {DiagnosticSeverity::Error, DiagnosticSeverity::Warning, DiagnosticSeverity::Info}) {
		for (const Diagnostic &d : v.diagnostics) {
			if (d.severity != severity) continue;
			ImGui::PushID(problem_index++);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextColored(severity_color(d.severity), "%s", diagnostic_severity_label(d.severity));
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", d.message.c_str());
			if (ImGui::IsItemHovered() && !d.code.empty()) ImGui::SetTooltip("%s", d.code.c_str());
			ImGui::TableNextColumn();
			if (ImGui::Selectable((d.asset + (d.line ? ":" + std::to_string(d.line) : "")).c_str())) {
				auto request = make_request(EditorRequestKind::OpenDocument, d.asset, d.record);
				request.catalog_edit.address = {d.row_id, d.record_kind, d.child_id};
				host_.request(std::move(request));
			}
            ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

} // namespace opennova::editor
