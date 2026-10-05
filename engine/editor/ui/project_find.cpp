#include "project_find.h"

#include <editor/graph/display_names.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
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

// What the hits and their uses read of the view: the graph (its files and symbols, their uses,
// where each leads: the graph moves with the files' paths and kinds too).
RevisionKey cache_key(const SessionView &view) {
	return revision_key(view.revisions, {ViewConcern::Graph});
}

} // namespace

const std::vector<ProjectFind::Usage> &ProjectFind::usages(const SessionView &view, size_t hit) {
	auto found = usages_.find(hit);
	if (found != usages_.end()) return found->second;
	std::vector<Usage> out;
	const GraphSearchHit &result = hits_[hit];
	for (const GraphEdge *edge : result.symbol ? view.findings.graph->users_of(*result.symbol) : view.findings.graph->usages_of(result.file)) {
		Usage use;
		// Its place in words, its file open or not (the plain-words lane: a record by its type's words).
		const AssetEntry *source = view.project.scan->at_path(edge->source);
		const std::string place = edge_place_words(*edge, source ? source->kind : AssetKind::Unknown);
		use.line = edge->source + (place.empty() ? std::string() : ": " + place);
		use.tip = use.line + "\n" + edge->field + " = " + edge->value;
		use.target = usage_target(*view.project.scan, *edge);
		out.push_back(std::move(use));
	}
	return usages_.emplace(hit, std::move(out)).first->second;
}

void ProjectFind::open(Workspace &workspace) {
	window_requests::set_workspace(workspace, "project_find", "open", io::JsonValue::make_bool(true));
}

void ProjectFind::draw(Workspace &workspace) {
	const SessionView &view = workspace.view();
	text_.follow(view.workspace.project_find.text);
	const auto close = [&] {
		window_requests::set_workspace(workspace, "project_find", "open", io::JsonValue::make_bool(false));
	};
	const float em = ImGui::GetFontSize();
	ImGui::SetNextWindowSize(ImVec2(em * 40.0f, em * 30.0f), ImGuiCond_Appearing);
	// Held open, it shows when no dialog before it in the session's order is held (shown_modal).
	const bool held = view.workspace.project_find.open && view.project.open && view.findings.graph &&
	                  modal_may_show(view, HeldModal::ProjectFind);
	if (!popup_.begin(kTitle, held, true, 0, true, view.workspace.opened)) {
		if (popup_.dismissed()) close();
		return;
	}
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	// Escape closes the modal, the text typed kept: the text box, which has the keyboard, would
	// put back the text it had when it took it.
	const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	char typed[sizeof(text_.text)];
	std::memcpy(typed, text_.text, sizeof(typed));
	ImGui::SetNextItemWidth(-FLT_MIN);
	const bool edited = ImGui::InputTextWithHint("##text", "A file, a name the files define, or an item's name", text_.text,
	                                             sizeof(text_.text));
	if (escape) {
		std::memcpy(text_.text, typed, sizeof(typed));
		close();
		popup_.close();
	} else if (edited) {
		window_requests::set_workspace(workspace, "project_find", "text", io::JsonValue::make_string(text_.sent()));
	}
	const std::string text = text_.sent();
	if (view_ != &view || key_ != cache_key(view) || searched_ != text) {
		view_ = &view;
		key_ = cache_key(view);
		searched_ = text;
		hits_ = view.findings.graph->search(searched_);
		usages_.clear();
	}
	// Somewhere to go: the modal closes as it goes.
	const auto go = [&](const ReferenceTarget &target) {
		window_requests::go_to(workspace, target);
		close();
		popup_.close();
	};
	if (text.empty()) ui_kit::empty_state("Type part of a file's name, of a name a file defines, or of the record that names a file (an item's name finds its model).");
	else if (hits_.empty()) ui_kit::empty_state("No file or name holds it.");
	else if (hits_.size() > kShownMax)
		ImGui::TextDisabled("%zu results; the first %zu listed. Type more to narrow them.", hits_.size(), kShownMax);
	ImGui::BeginChild("results", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
	for (size_t i = 0; i < hits_.size() && i < kShownMax; ++i) {
		const GraphSearchHit &hit = hits_[i];
		ImGui::PushID(static_cast<int>(i));
		// The result: its name, what it is and where, its uses; opened, the uses.
		const std::string uses = std::to_string(hit.usages) + (hit.usages == 1 ? " use" : " uses");
		// An item by its catalog's name first, its id after it; a file found by what names it says so:
		// "Dblkhwk1.3di, used by Flyable Blackhawk" (S17).
		const std::string named = hit.words.empty() ? hit.name : hit.words + " " + hit.name;
		const std::string by = hit.via.empty() ? std::string() : ", used by " + hit.via;
		const std::string title = named + by + "  (" + what_of(hit) + ", " + hit.file + ")  " + uses;
		const float go_width = ui_kit::button_width("Go to") + ImGui::GetStyle().ItemSpacing.x;
		const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
		const bool expanded = ImGui::TreeNodeEx("result", hit.usages ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_Leaf, "%s",
		                                        ui_kit::fit(title, ImGui::GetContentRegionAvail().x - go_width -
		                                                                   ImGui::GetTreeNodeToLabelSpacing())
		                                                .c_str());
		std::string tip = named + "\n" + what_of(hit) + " in " + hit.file + "\n" + uses;
		if (!hit.via.empty()) tip += "\nFound by " + hit.via + " (" + hit.via_file + "), which names it.";
		if (hit.symbol && hit.symbol->inert) tip += "\nNo lookup of the game finds it: " + hit.symbol->inert_reason + ".";
		ui_kit::tooltip(tip);
		ImGui::SameLine(right - ui_kit::button_width("Go to"));
		if (ImGui::SmallButton("Go to"))
			go(hit.symbol ? symbol_target(*view.project.scan, *hit.symbol)
						  : file_target(*view.project.scan, hit.file));
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
	if (ImGui::Button("Close")) {
		close();
		popup_.close();
	}
	ImGui::EndPopup();
}

} // namespace opennova::editor
