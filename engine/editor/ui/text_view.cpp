#include "text_view.h"

#include <string>
#include <utility>

#include <base/io/cp1252.h>
#include <editor/model/text_document.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

int rank(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Error: return 2;
	case DiagnosticSeverity::Warning: return 1;
	case DiagnosticSeverity::Info: break;
	}
	return 0;
}

} // namespace

void TextView::rebind(const DocumentBase &document) {
	(void)document;
	marked_ = 0;
	scroll_ = false;
	markers_valid_ = false;
}

void TextView::follow_markers(const SessionView &view, const DocumentBase &document) {
	findings_.follow(view);
	const RevisionKey key = FindingsIndex::cache_key(view);
	if (markers_valid_ && key == markers_key_ && markers_document_ == document.identity() &&
			markers_load_ == document.load_generation() && markers_revision_ == document.revision())
		return;
	markers_valid_ = true;
	markers_key_ = key;
	markers_document_ = document.identity();
	markers_load_ = document.load_generation();
	markers_revision_ = document.revision();
	++markers_made_;
	markers_.clear();
	for (const size_t i : findings_.of_file(document.path())) {
		const Diagnostic &d = view.findings.diagnostics[i];
		if (!d.line) continue;
		Marker &marker = markers_[d.line];
		const bool first = marker.tip.empty();
		if (first || rank(d.severity) > rank(marker.severity)) marker.severity = d.severity;
		marker.tip += (first ? "" : "\n") + d.message;
	}
}

void TextView::draw(Workspace &workspace, const DocumentBase &base) {
	const TextDocument *document = text_of(base);
	if (!document) {
		take_events();
		return ui_kit::empty_state("This file holds no text to show.");
	}
	draw_toolbar(workspace, *document);
	draw_lines(workspace, *document);
}

void TextView::draw_toolbar(Workspace &workspace, const TextDocument &document) {
	// A text held read only is a file its text form cannot carry as it is (a shipped music script's
	// message handler), nothing to correct: each issue says what.
	draw_document_toolbar(workspace, document,
			"Read only: the editor shows this file's text and cannot write the file back as it is.");
	for (const SourceIssue &issue : document.issues())
		if (issue.blocks) ImGui::TextWrapped("%s", issue.message.c_str());
}

void TextView::draw_lines(Workspace &workspace, const TextDocument &text) {
	// A RevealText taken: its line marked and scrolled to (a RevealRecord names no record here).
	for (const ViewEvent &event : take_events()) {
		size_t line = 0, column = 0;
		if (event.kind == ViewEventKind::RevealText && TextDocument::read_locator(event.locator, line, column)) {
			marked_ = line;
			scroll_ = true;
		}
	}
	const TextDocument *document = &text;
	const SessionView &view = workspace.view();
	follow_markers(view, *document);
	const std::string lines = std::to_string(document->line_count()) +
			" lines, read only here: an edit is a span the editor MCP sends.";
	ui_kit::empty_state(lines.c_str());
	if (!ImGui::BeginChild("text", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
		ImGui::EndChild();
		return;
	}
	const size_t count = document->line_count();
	// A row is a frame high (its line aligned to the frame's padding, the marker's box) and advances
	// by the spacing besides: the clipper's step.
	const float row_height = ImGui::GetFrameHeight();
	const float row_step = ImGui::GetFrameHeightWithSpacing();
	const int digits = int(std::to_string(count).size());
	if (scroll_ && marked_ >= 1 && marked_ <= count) {
		ImGui::SetScrollY(float(marked_ - 1) * row_step - ImGui::GetWindowHeight() * 0.4f);
		scroll_ = false;
	}
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(count), row_step);
	lines_drawn_ = 0;
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
			const size_t number = size_t(i) + 1;
			++lines_drawn_;
			ImGui::PushID(i);
			// The marked line (a reveal's) lit across the row.
			if (number == marked_) {
				const ImVec2 at = ImGui::GetCursorScreenPos();
				const float width = ImGui::GetContentRegionAvail().x + ImGui::GetScrollX();
				ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + width, at.y + row_height),
						ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
			}
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%*zu", digits, number);
			ImGui::SameLine();
			const auto marker = markers_.find(number);
			if (marker != markers_.end()) {
				ui_kit::severity_marker(marker->second.severity);
				ui_kit::tooltip(marker->second.tip);
			} else {
				ImGui::Dummy(ImVec2(row_height, row_height));
			}
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			const std::string text = cp1252_to_utf8(document->line(number));
			ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
			ImGui::PopID();
		}
	ImGui::EndChild();
}

} // namespace opennova::editor
