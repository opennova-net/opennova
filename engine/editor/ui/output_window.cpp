#include <editor/ui/output_window.h>

#include <imgui.h>

namespace opennova::editor {

void OutputWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = host_.view();
	if (v.play_state != PlayState::Stopped && !v.play_command_line.empty()) {
		ImGui::TextDisabled("%s", v.play_command_line.c_str());
	}
	if (ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
	                      ImGuiWindowFlags_HorizontalScrollbar)) {
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(v.output.size()));
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				ImGui::TextUnformatted(v.output[static_cast<size_t>(i)].c_str());
			}
		}
		// Follow the newest line unless the user scrolled up to read.
		if (v.output.size() != lines_seen_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
			ImGui::SetScrollHereY(1.0f);
		}
		lines_seen_ = v.output.size();
	}
	ImGui::EndChild();
}

} // namespace opennova::editor
