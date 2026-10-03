#include <editor/ui/output_window.h>

#include <iterator>
#include <string>

#include <editor/model/field_text.h>
#include <editor/session/output_log.h>
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

std::vector<std::pair<uint64_t, int64_t>> OutputWindow::rows(const OutputLog &output, const std::set<uint64_t> &open) {
	std::vector<std::pair<uint64_t, int64_t>> out;
	out.reserve(output.size());
	for (size_t i = 0; i < output.size(); ++i) {
		const uint64_t at = output.first_index() + i;
		out.emplace_back(at, -1);
		if (!open.count(at)) continue;
		for (size_t f = 0; f < output.folded(i).size(); ++f) out.emplace_back(at, int64_t(f));
	}
	return out;
}

void OutputWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = workspace_.view();
	const OutputLog &output = v.activity.output;
	const bool empty = output.empty();
	ui_kit::WrapRow row;
	if (ui_kit::tool(row, "Clear", !empty, empty ? "Nothing to clear." : "Empties the output."))
		workspace_.request(request::clear_output());
	if (ui_kit::tool(row, "Copy", !empty, empty ? "Nothing to copy." : "Copies every line, the folded ones too, to the clipboard.")) {
		std::string all;
		for (size_t i = 0; i < output.size(); ++i) {
			all += output[i] + "\n";
			for (const std::string &folded : output.folded(i)) all += "    " + folded + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
	// The last game's whole log, as it wrote it (the one under its line holds as much as Output keeps).
	if (!v.activity.play_log_file.empty() &&
	    ui_kit::tool(row, "Game log file", v.allows(EditorRequestKind::RevealPath),
	                 "Shows the last game's log file in the file browser: " + v.activity.play_log_file))
		workspace_.request(request::reveal_path(v.activity.play_log_file));
	if (empty) {
		lines_seen_ = output.next_index();
		ui_kit::empty_state("Nothing yet.", "What the editor does and what the running game says show here.");
		return;
	}
	// The lines opened that the log no longer holds go.
	for (auto it = open_.begin(); it != open_.end();) it = *it < output.first_index() ? open_.erase(it) : std::next(it);
	if (ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
	                      ImGuiWindowFlags_HorizontalScrollbar)) {
		const std::vector<std::pair<uint64_t, int64_t>> shown = rows(output, open_);
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(shown.size()));
		while (clipper.Step()) {
			for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
				const auto [at, folded] = shown[size_t(r)];
				const size_t held = static_cast<size_t>(at - output.first_index());
				if (folded >= 0) {
					// A folded line, under its own, muted unless it reads as an error or a warning.
					ImGui::Indent();
					const std::string &text = output.folded(held)[size_t(folded)];
					if (game_line_matters(text)) ImGui::TextUnformatted(text.c_str());
					else ImGui::TextDisabled("%s", text.c_str());
					ImGui::Unindent();
					continue;
				}
				const size_t count = output.folded(held).size() + output.folded_dropped(held);
				if (count == 0) {
					output_line(output[held]);
					continue;
				}
				// A line holding others: a click opens it.
				const bool open = open_.count(at) != 0;
				ImGui::SetNextItemOpen(open, ImGuiCond_Always);
				const std::string label = output[held] + "  (" + counted(count, "line") + ")###folded" + std::to_string(at);
				if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth) != open) {
					if (open) open_.erase(at);
					else open_.insert(at);
				}
				ui_kit::tooltip(open ? "Folds its lines away." : "Shows the lines folded under it.");
			}
		}
		// Follow the newest line unless the user scrolled up to read (by its absolute index: at the
		// log's cap the count stands while the lines move on).
		if (output.next_index() != lines_seen_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
			ImGui::SetScrollHereY(1.0f);
		}
		lines_seen_ = output.next_index();
	}
	ImGui::EndChild();
}

} // namespace opennova::editor
