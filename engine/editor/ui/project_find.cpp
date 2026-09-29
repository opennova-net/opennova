#include "project_find.h"

#include <editor/ui/editor_requests.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstring>

#include <imgui.h>

namespace opennova::editor {
namespace {

constexpr const char *kTitle = "Find in project";
constexpr size_t kShownMax = 200; // the results listed; more ask for a longer text

// What a result is: a file, or a symbol's kind.
std::string what_of(const GraphSearchHit &hit) {
	if (!hit.symbol) return "file";
	return std::string(reference_row(hit.symbol->kind).label) + (hit.symbol->inert ? ", unreachable" : "");
}

} // namespace

const std::vector<ProjectFind::Usage> &ProjectFind::usages(const SessionView &view, size_t hit) {
	auto found = usages_.find(hit);
	if (found != usages_.end()) return found->second;
	std::vector<Usage> out;
	const GraphSearchHit &result = hits_[hit];
	for (const GraphEdge *edge : result.symbol ? view.graph->users_of(*result.symbol) : view.graph->usages_of(result.file)) {
		Usage use;
		use.line = edge->source + ": " + (edge->record.empty() ? "" : edge->record + " - ") + edge_field_title(view, *edge);
		use.tip = use.line + "\n" + edge->field + " = " + edge->value;
		use.target = usage_target(*edge, view);
		out.push_back(std::move(use));
	}
	return usages_.emplace(hit, std::move(out)).first->second;
}

void ProjectFind::open() {
	ask_ = true;
}

void ProjectFind::draw(EditorHost &host) {
	const SessionView &view = host.view();
	if (ask_) {
		ask_ = false;
		ImGui::OpenPopup(kTitle);
	}
	const float em = ImGui::GetFontSize();
	ImGui::SetNextWindowSize(ImVec2(em * 40.0f, em * 30.0f), ImGuiCond_Appearing);
	bool open = true;
	if (!ImGui::BeginPopupModal(kTitle, &open)) return;
	if (!view.project_open || !view.graph) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	// Escape closes the modal, the text typed kept: the text box, which has the keyboard, would
	// put back the text it had when it took it.
	const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	char typed[sizeof(text_)];
	std::memcpy(typed, text_, sizeof(typed));
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##text", "A file or a name the files define", text_, sizeof(text_));
	if (escape) {
		std::memcpy(text_, typed, sizeof(typed));
		ImGui::CloseCurrentPopup();
	}
	if (view_ != &view || revision_ != view.revision || searched_ != text_) {
		view_ = &view;
		revision_ = view.revision;
		searched_ = text_;
		hits_ = view.graph->search(searched_);
		usages_.clear();
	}
	// Somewhere to go: the modal closes as it goes.
	const auto go = [&](const ReferenceTarget &target) {
		window_requests::go_to(host, target);
		ImGui::CloseCurrentPopup();
	};
	if (!text_[0]) ui_kit::empty_state("Type part of a file's name or of a name a file defines.");
	else if (hits_.empty()) ui_kit::empty_state("No file or name holds it.");
	else if (hits_.size() > kShownMax)
		ImGui::TextDisabled("%zu results; the first %zu listed. Type more to narrow them.", hits_.size(), kShownMax);
	ImGui::BeginChild("results", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
	for (size_t i = 0; i < hits_.size() && i < kShownMax; ++i) {
		const GraphSearchHit &hit = hits_[i];
		ImGui::PushID(static_cast<int>(i));
		// The result: its name, what it is and where, its uses; opened, the uses.
		const std::string uses = std::to_string(hit.usages) + (hit.usages == 1 ? " use" : " uses");
		const std::string title = hit.name + "  (" + what_of(hit) + ", " + hit.file + ")  " + uses;
		const float go_width = ui_kit::button_width("Go to") + ImGui::GetStyle().ItemSpacing.x;
		const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		const bool expanded = ImGui::TreeNodeEx("result", hit.usages ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_Leaf, "%s",
		                                        ui_kit::fit(title, ImGui::GetContentRegionAvail().x - go_width -
		                                                                   ImGui::GetTreeNodeToLabelSpacing())
		                                                .c_str());
		std::string tip = hit.name + "\n" + what_of(hit) + " in " + hit.file + "\n" + uses;
		if (hit.symbol && hit.symbol->inert) tip += "\nNo lookup of the game finds it: " + hit.symbol->inert_reason + ".";
		ui_kit::tooltip(tip);
		ImGui::SameLine(right - ui_kit::button_width("Go to"));
		if (ImGui::SmallButton("Go to"))
			go(hit.symbol ? symbol_target(*hit.symbol, view) : file_target(hit.file, view));
		ui_kit::tooltip(hit.symbol ? "Open the record that defines it." : "Open the file, or show it in Files.");
		if (expanded) {
			// Its uses, made once while the hits stand, the rows out of sight not drawn.
			const std::vector<Usage> &uses_of = usages(view, i);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(uses_of.size()));
			while (clipper.Step())
				for (int u = clipper.DisplayStart; u < clipper.DisplayEnd; ++u) {
					const Usage &use = uses_of[size_t(u)];
					ImGui::PushID(u);
					if (ImGui::Selectable((ui_kit::fit(use.line, ImGui::GetContentRegionAvail().x) + "###use").c_str()))
						go(use.target);
					ui_kit::tooltip(use.tip);
					ImGui::PopID();
				}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
	if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

} // namespace opennova::editor
