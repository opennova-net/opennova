#include "reference_picker.h"

#include <editor/graph/reference_queries.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <set>

#include <imgui.h>

namespace opennova::editor {
namespace {

// A choice's line: its name, then where it is defined, dimmed.
std::string where_of(const ReferenceChoice &choice) {
	if (choice.record.empty()) return choice.file;
	return choice.file + ": " + choice.record;
}

// What a choice's tooltip says: the name, where, what the field would reference, why a lookup
// never finds it.
std::string choice_tip(const ReferenceChoice &choice) {
	std::string tip = choice.name + "\n" + (choice.record.empty() ? "The file " : "Defined in ") + where_of(choice);
	if (choice.status == ReferenceStatus::Missing) tip += "\nSet here, the game would not find it: Missing.";
	if (choice.inert) tip += "\nNo lookup of the game finds this definition: " + choice.reason + ".";
	return tip;
}

} // namespace

ReferencePicker::ReferencePicker() = default;

ReferencePicker::~ReferencePicker() = default;

ReferencePicker::ListKey ReferencePicker::cache_key(const SessionView &view,
		const Document &document) {
	const RevisionKey reads = revision_key(view.revisions, {ViewConcern::Graph, ViewConcern::Files,
			ViewConcern::Project, ViewConcern::Preferences});
	return {reads, document.identity(), document.revision()};
}

void ReferencePicker::refresh(Popup &popup, const SessionView &view, const Document &document, const NodeAddress &record,
                              const FieldUse &field, const Value &value, bool others) {
	const ListKey key = cache_key(view, document);
	if (popup.view == &view && popup.key == key) return;
	popup.view = &view;
	popup.key = key;
	++lists_made_;
	popup.choices = view.findings.graph ? reference_choices(*view.findings.graph, field)
										: std::vector<ReferenceChoice>();
	if (!others)
		popup.choices.erase(std::remove_if(popup.choices.begin(), popup.choices.end(),
		                                   [&](const ReferenceChoice &choice) { return choice.kind != field.reference; }),
		                    popup.choices.end());
	popup.fixes.clear();
	// The finding the graph makes of this value, as Problems shows it, for its fixes (a %NAME% the
	// stylesheets do not define: the variable's).
	Diagnostic finding;
	popup.missing = view.findings.graph &&
			missing_finding(*view.findings.graph, document, record, field, value, finding);
	if (popup.missing) popup.fixes = fixes_for(finding, view);
}

void ReferencePicker::prune(const SessionView &view) {
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::DocumentSet});
	if (pruned_view_ == &view && pruned_key_ == key) return;
	pruned_view_ = &view;
	pruned_key_ = key;
	std::set<uint64_t> open;
	for (const auto &document : view.documents.open)
		if (document) open.insert(document->identity());
	for (auto it = popups_.begin(); it != popups_.end();)
		it = open.count(it->first.document) ? std::next(it) : popups_.erase(it);
}

bool ReferencePicker::draw(Workspace &workspace, const Document &document, const NodeAddress &record, const FieldUse &field,
                           const Value &value, bool compact, std::string &picked, bool others) {
	if (ImGui::SmallButton(compact ? "..." : "Pick")) ImGui::OpenPopup("references");
	ui_kit::tooltip(reference_row(field.reference).also_offers == ReferenceKind::StyleVar
	                  ? "Pick a file of the project or a variable of the stylesheet."
	                  : "Pick a name the project has.");
	// The popup's own id, the document and the record key its state: another record's field of
	// the same place in the window has its own.
	const Key key{document.identity(), record.row, record.kind, record.child, ImGui::GetID("references")};
	if (!ImGui::BeginPopup("references")) return false;
	prune(workspace.view());
	Popup &popup = popups_[key];
	refresh(popup, workspace.view(), document, record, field, value, others);
	const bool done = draw_popup(workspace, popup, picked);
	ImGui::EndPopup();
	return done;
}

bool ReferencePicker::draw_popup(Workspace &workspace, Popup &popup, std::string &picked) {
	const float width = ImGui::GetFontSize() * 26.0f;
	if (ImGui::IsWindowAppearing()) {
		popup.cursor = 0;
		ImGui::SetKeyboardFocusHere();
	}
	// Escape closes, and the filter typed stays: the text box, which has the keyboard, would put
	// back the text it had when it took it.
	const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	char typed[sizeof(popup.filter)];
	std::memcpy(typed, popup.filter, sizeof(typed));
	if (ui_kit::filter_box("##find", popup.filter, sizeof(popup.filter), "Filter", width)) popup.cursor = 0;
	if (escape) {
		std::memcpy(popup.filter, typed, sizeof(typed));
		ImGui::CloseCurrentPopup();
	}
	size_t inert = 0;
	for (const ReferenceChoice &choice : popup.choices) inert += choice.inert ? 1 : 0;
	if (inert) {
		const std::string label = "Show unreachable (" + std::to_string(inert) + ")";
		if (ImGui::Checkbox(label.c_str(), &popup.unreachable)) popup.cursor = 0;
		ui_kit::tooltip("Names defined only where no lookup of the game finds them.");
	}
	// The rows shown: the filter's matches, the unreachable ones while shown.
	std::vector<const ReferenceChoice *> shown;
	for (const ReferenceChoice &choice : popup.choices)
		if ((!choice.inert || popup.unreachable) && (!popup.filter[0] || window_requests::matches(choice.name, popup.filter)))
			shown.push_back(&choice);
	// The keys: the arrows move the highlighted row, Enter picks it, Escape closes.
	popup.moved = false;
	if (!shown.empty() && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
		popup.cursor = std::min(popup.cursor + 1, shown.size() - 1);
		popup.moved = true;
	}
	if (!shown.empty() && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
		popup.cursor = popup.cursor ? popup.cursor - 1 : 0;
		popup.moved = true;
	}
	popup.cursor = std::min(popup.cursor, shown.empty() ? size_t(0) : shown.size() - 1);
	bool chosen = !escape && !shown.empty() &&
	              (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
	if (chosen) picked = shown[popup.cursor]->name;
	const float line = ImGui::GetTextLineHeightWithSpacing();
	ImGui::BeginChild("names", ImVec2(width, line * float(std::clamp<size_t>(shown.size(), 3, 14)) + line * 0.5f),
	                  ImGuiChildFlags_Borders);
	for (size_t i = 0; i < shown.size() && !chosen; ++i) {
		const ReferenceChoice &choice = *shown[i];
		ImGui::PushID(static_cast<int>(i));
		const float x = ImGui::GetCursorPosX();
		if (ImGui::Selectable("##choice", i == popup.cursor, ImGuiSelectableFlags_AllowOverlap)) {
			picked = choice.name;
			chosen = true;
		}
		if (i == popup.cursor && popup.moved) ImGui::SetScrollHereY(0.5f);
		ui_kit::tooltip(choice_tip(choice));
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorPosX(x);
		// The name, then where it is defined in what is left, then what it would be when not found.
		const bool found = choice.status == ReferenceStatus::Present || choice.status == ReferenceStatus::Unverified;
		const char *word = found ? "" : ui_kit::reference_word(choice.status);
		const float room = ImGui::GetContentRegionAvail().x - (found ? 0.0f : ui_kit::text_width(word));
		const std::string name = ui_kit::fit(choice.name, room * 0.6f);
		if (choice.inert) ImGui::TextDisabled("%s", name.c_str());
		else ImGui::TextUnformatted(name.c_str());
		ImGui::SameLine();
		const std::string where = ui_kit::fit(where_of(choice), room - ui_kit::text_width(name.c_str()) -
		                                                                ImGui::GetStyle().ItemSpacing.x * 2.0f);
		ImGui::TextDisabled("%s", where.c_str());
		if (!found) {
			ImGui::SameLine();
			ImGui::TextColored(ui_kit::reference_color(choice.status), "%s", word);
		}
		ImGui::PopID();
	}
	if (popup.choices.empty()) ui_kit::empty_state("The project has no names of this kind yet.");
	else if (shown.empty()) ui_kit::empty_state(popup.filter[0] ? "Nothing matches the filter." : "Every name is unreachable.");
	ImGui::EndChild();
	// The value's own fixes while it is missing, as Problems offers them.
	if (popup.missing) {
		ImGui::Separator();
		ImGui::TextColored(ui_kit::reference_color(ReferenceStatus::Missing), "%s",
		                   popup.fixes.empty() ? "The value is missing; nothing here makes it." : "The value is missing:");
		for (const ProblemFix &fix : popup.fixes) {
			ImGui::PushID(fix.label.c_str());
			if (ui_kit::fitted_button(fix.label, "fix", width)) {
				workspace.request(fix.request);
				ImGui::CloseCurrentPopup();
			}
			ui_kit::tooltip(fix.detail);
			ImGui::PopID();
		}
	}
	if (chosen) ImGui::CloseCurrentPopup();
	return chosen;
}

bool ReferencePicker::accept_file(const SessionView &view, const FieldUse &field, std::string &picked) {
	if (!ImGui::BeginDragDropTarget()) return false;
	bool dropped = false;
	// The file looked at before the drop is accepted: one that does not fit is never accepted.
	const ImGuiPayload *dragged = ImGui::GetDragDropPayload();
	if (dragged && dragged->IsDataType(kFileDragPayload) && dragged->Data) {
		const char *data = static_cast<const char *>(dragged->Data);
		const std::string path(data, strnlen(data, size_t(dragged->DataSize)));
		const AssetEntry *entry = nullptr;
		for (const AssetEntry &candidate : view.project.scan->entries)
			if (candidate.relative_path == path) entry = &candidate;
		if (entry && file_serves_reference(entry->kind, field.reference, field.loader_arg) &&
		    ImGui::AcceptDragDropPayload(kFileDragPayload)) {
			picked = entry->logical_name;
			dropped = true;
		}
	}
	ImGui::EndDragDropTarget();
	return dropped;
}

} // namespace opennova::editor
