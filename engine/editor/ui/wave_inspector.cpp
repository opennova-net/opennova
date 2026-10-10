#include "wave_inspector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <imgui.h>

#include <editor/documents/wave_document.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

namespace opennova::editor {

namespace {

std::string number_words(const char *format, double value) {
	char text[64];
	std::snprintf(text, sizeof(text), format, value);
	return text;
}

class WaveView final : public DocumentView {
public:
	void draw(Workspace &workspace, const DocumentBase &document) override {
		// A wave holds no records: a reveal sent to it shows nothing more than the tab itself.
		take_events();
		const auto *wave = dynamic_cast<const WaveDocument *>(&document);
		if (!wave) return;
		const lwf::WaveFacts &facts = wave->facts();
		ImGui::TextUnformatted(basename_of(document.path()).c_str());
		ui_kit::tooltip(document.path() + "\nRead as the game's loader reads it. Its edits are whole-wave ones: a trim, a "
		                                  "normalise.");
		if (path_ != document.path()) {
			path_ = document.path();
			from_ = 0.0f;
			to_ = float(facts.seconds);
			peak_ = 1.0f;
		}
		if (!facts.read) {
			ImGui::TextWrapped("The editor reads no wave in this file: %s.", facts.error.c_str());
			return;
		}
		// What it is, and whether the game plays it.
		ImGui::TextWrapped("%s", (lwf::wave_format_words(facts.format) + ", " + number_words("%.3f s", facts.seconds) + ", " +
		                          std::to_string(facts.frames) + " frames; peak " +
		                          number_words("%.0f%%", double(facts.peak) * 100.0) + ", RMS " +
		                          number_words("%.0f%%", double(facts.rms) * 100.0) + ".")
		                                 .c_str());
		if (facts.retail.plays) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextWrapped("The game plays it as it is.");
			ImGui::PopStyleColor();
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.6f, 0.3f, 1.0f));
			ImGui::TextWrapped("The game cannot play it: %s A trim or a normalise writes it as the game plays it.",
			                   facts.retail.why.c_str());
			ImGui::PopStyleColor();
		}
		ui_kit::WrapRow tools;
		if (ui_kit::tool(tools, "Play", !wave->dirty(),
		                 wave->dirty() ? std::string("Save the wave first: the editor plays the file, as the game loads it.")
		                               : std::string("Plays the wave as the game loads it.")))
			workspace.request(request::play_sound(document.path()));
		if (ui_kit::tool(tools, "Stop", true, "Stops the sound the editor plays.")) workspace.request(request::stop_sound());
		// The trim: from and to, seconds.
		const float seconds = float(facts.seconds);
		to_ = std::min(to_, seconds);
		const float field = ImGui::GetFontSize() * 6.0f;
		tools.next(ui_kit::field_width(field, "##from"));
		ImGui::SetNextItemWidth(field);
		ImGui::DragFloat("##from", &from_, 0.001f, 0.0f, seconds, "from %.3f s");
		ui_kit::tooltip("Where the trim starts, seconds from the wave's start.");
		tools.next(ui_kit::field_width(field, "##to"));
		ImGui::SetNextItemWidth(field);
		ImGui::DragFloat("##to", &to_, 0.001f, 0.0f, seconds, "to %.3f s");
		ui_kit::tooltip("Where the trim ends, seconds from the wave's start.");
		if (ui_kit::tool(tools, "Trim", to_ > from_, "Keeps the samples between from and to: one undo step, written as the game "
		                                             "takes a wave (one channel, 8 or 16 bits, fmt and data alone)."))
			workspace.request(request::wave_operation(document.path(), "trim",
			                                          {{"start", number_words("%.6f", double(from_))},
			                                           {"end", number_words("%.6f", double(to_))}}));
		tools.next(ui_kit::field_width(field, "##peak"));
		ImGui::SetNextItemWidth(field);
		ImGui::SliderFloat("##peak", &peak_, 0.05f, 1.0f, "peak %.2f");
		ui_kit::tooltip("The level the loudest sample is scaled to by a normalise, 1 full scale.");
		if (ui_kit::tool(tools, "Normalise", facts.peak > 0.0f,
		                 facts.peak > 0.0f ? std::string("Scales every sample so the loudest is at the peak: one undo step, "
		                                                 "written as the game takes a wave.")
		                                   : std::string("The wave is silent: there is no sample to scale.")))
			workspace.request(request::wave_operation(document.path(), "normalise", {{"peak", number_words("%.4f", double(peak_))}}));
		// The picture, as large as the tab gives it, the trim's span marked.
		const ImVec2 room = ImGui::GetContentRegionAvail();
		const ImVec2 size(std::max(room.x, 64.0f), std::max(room.y, 64.0f));
		const ImVec2 at = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##picture", size);
		ImDrawList *draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(28, 28, 32, 255));
		const float middle = at.y + size.y * 0.5f;
		if (seconds > 0.0f) {
			const float x0 = at.x + size.x * (from_ / seconds), x1 = at.x + size.x * (to_ / seconds);
			draw->AddRectFilled(ImVec2(x0, at.y), ImVec2(x1, at.y + size.y), IM_COL32(60, 60, 80, 255));
		}
		const size_t bins = facts.envelope.size();
		for (size_t i = 0; i < bins; ++i) {
			const float x0 = at.x + size.x * float(i) / float(bins), x1 = at.x + size.x * float(i + 1) / float(bins);
			const float h = facts.envelope[i] * size.y * 0.5f;
			draw->AddRectFilled(ImVec2(x0, middle - h), ImVec2(std::max(x1 - 1.0f, x0 + 1.0f), middle + h),
			                    facts.envelope[i] >= 0.999f ? IM_COL32(230, 90, 70, 255) : IM_COL32(110, 190, 230, 255));
		}
		draw->AddLine(ImVec2(at.x, middle), ImVec2(at.x + size.x, middle), IM_COL32(90, 90, 100, 255));
		if (ImGui::IsItemHovered() && seconds > 0.0f) {
			const float t = std::clamp((ImGui::GetIO().MousePos.x - at.x) / size.x, 0.0f, 1.0f) * seconds;
			ui_kit::tooltip(number_words("%.3f s", double(t)) + ": a click sets from, a right click sets to.");
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) from_ = std::min(t, to_);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) to_ = std::max(t, from_);
		}
	}

private:
	std::string path_;
	float from_ = 0.0f, to_ = 0.0f, peak_ = 1.0f;
};

} // namespace

std::unique_ptr<DocumentView> make_wave_view() { return std::make_unique<WaveView>(); }

} // namespace opennova::editor
