#include <editor/ui/output_window.h>

#include <string>

#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

// A line the session wrote about a finding ("error: ...", "warning: ..."), in the
// severity's colour; any other line in the text's.
void output_line(const std::string &line) {
	const bool error = line.rfind("error: ", 0) == 0;
	const bool warning = line.rfind("warning: ", 0) == 0;
	if (!error && !warning) return ImGui::TextUnformatted(line.c_str());
	ImGui::PushStyleColor(ImGuiCol_Text,
	                      ui_kit::severity_color(error ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning));
	ImGui::TextUnformatted(line.c_str());
	ImGui::PopStyleColor();
}

} // namespace

void OutputWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = workspace_.view();
	const bool empty = v.activity.output.empty();
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Clear", !empty, empty ? "Nothing to clear." : "Empties the output."))
		workspace_.request(request::clear_output());
	if (ui_kit::tool(row, "Copy", !empty, empty ? "Nothing to copy." : "Copies every line to the clipboard.")) {
		std::string all;
		for (const std::string &line : v.activity.output) all += line + "\n";
		ImGui::SetClipboardText(all.c_str());
	}
	if (v.activity.play_state != PlayState::Stopped && !v.activity.play_command_line.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ui_kit::clipped_text(v.activity.play_command_line);
		ImGui::PopStyleColor();
	}
	if (empty) {
		lines_seen_ = v.activity.output.next_index();
		ui_kit::empty_state("Nothing yet.", "What the editor does and what the running game says show here.");
		return;
	}
	if (ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
	                      ImGuiWindowFlags_HorizontalScrollbar)) {
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(v.activity.output.size()));
		while (clipper.Step()) {
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) output_line(v.activity.output[static_cast<size_t>(i)]);
		}
		// Follow the newest line unless the user scrolled up to read (by its absolute index: at the
		// log's cap the count stands while the lines move on).
		if (v.activity.output.next_index() != lines_seen_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
			ImGui::SetScrollHereY(1.0f);
		}
		lines_seen_ = v.activity.output.next_index();
	}
	ImGui::EndChild();
}

} // namespace opennova::editor
