#include "correct_line_dialog.h"

#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>

#include <string>

#include <imgui.h>

namespace opennova::editor {

void CorrectLineDialog::draw(Workspace &workspace) {
	const SessionView &view = workspace.view();
	const WorkspaceView::CorrectLine &held = view.workspace.correct_line;
	text_.follow(held.text);
	const auto close = [&workspace] {
		window_requests::set_workspace(workspace, "correct_line", "path", io::JsonValue::make_string(""));
	};
	if (!popup_.begin("Correct the line", !held.path.empty() && view.project.open &&
	                                          modal_may_show(view, HeldModal::CorrectLine),
	                  true, ImGuiWindowFlags_AlwaysAutoResize, false, view.workspace.opened)) {
		if (popup_.dismissed()) close();
		return;
	}
	const float em = ImGui::GetFontSize();
	const std::string place = held.path + ", line " + std::to_string(held.line);
	ui_kit::quiet_text(place, "Clear the text to take the line out.");
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(em * 40.0f);
	const bool enter = ImGui::InputText("##line", text_.text, sizeof(text_.text), ImGuiInputTextFlags_EnterReturnsTrue);
	if (ImGui::IsItemEdited() && text_.sent() != held.text)
		window_requests::set_workspace(workspace, "correct_line", "text", io::JsonValue::make_string(text_.sent()));
	const std::string text = text_.sent();
	const bool changed = text != held.was;
	// It writes a project file: while an operation holds the files (a build packing them), it waits.
	const bool allowed = view.allows(EditorRequestKind::CorrectLine);
	ImGui::BeginDisabled(!changed || !allowed);
	const bool apply = ImGui::Button(text.empty() ? "Take it out" : "Apply");
	ImGui::EndDisabled();
	ui_kit::tooltip(!allowed ? "It writes the file: it waits for the running operation."
	                         : "Writes the line in the file on disk and reads the file again. Undo does not reach it.");
	if ((apply || enter) && changed && allowed) {
		// The session closes the dialog as it takes the correction (correct_line alone, over the wire); Apply closes it
		// too.
		workspace.request(request::correct_line(held.path, held.line, text, held.was));
		close();
		popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		close();
		popup_.close();
	}
	ImGui::EndPopup();
}

} // namespace opennova::editor
