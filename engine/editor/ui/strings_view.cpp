#include "strings_view.h"

#include <editor/documents/strings_document.h>
#include <editor/session/session_view.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/table_cells.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <imgui.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kSection = node_kind(StringsKind::Section);
constexpr NodeKind kString = node_kind(StringsKind::String);

using window_requests::edit;
using window_requests::matches;
using window_requests::select;

std::string text_of(const Document &document, const NodeAddress &address, const char *field) {
	Value value;
	return document.get(address, field, value) && std::holds_alternative<std::string>(value) ? std::get<std::string>(value)
	                                                                                         : std::string();
}

// The sections: their tools (plain words: the column is narrow, its header names the list),
// then each section, marked when it was added or changed since the last save.
void draw_sections(Workspace &workspace, const StringsDocument &document, const RecordReveal &reveal, size_t index) {
	const SessionView &view = workspace.view();
	const auto &rows = document.rows();
	ImGui::PushID("sections");
	ui_kit::WrapRow row;
	ui_kit::RowTools tools;
	tools.count = rows.size();
	tools.selected = index;
	tools.pick = "Select a section first.";
	tools.add_tip = "Adds a section at the end of the table.";
	tools.duplicate_tip = "A copy of the selected section with its strings, right after it, under a name of its own.";
	tools.remove_tip = "Removes the selected section with its strings.";
	const NodeAddress address = index < rows.size() ? NodeAddress{rows[index]->id, kSection, 0} : NodeAddress();
	switch (ui_kit::row_tools(row, tools)) {
	case ui_kit::RowTool::Add: edit(workspace, document, EditOperation::Add, {0, kSection, 0}); break;
	case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, address, index + 1); break;
	case ui_kit::RowTool::Remove: edit(workspace, document, EditOperation::Remove, address); break;
	case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, address, index - 1); break;
	case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, address, index + 1); break;
	case ui_kit::RowTool::None: break;
	}
	if (rows.empty()) ui_kit::empty_state("The table has no sections yet.");
	for (const auto &section : rows) {
		ImGui::PushID(int(section->id));
		const NodeAddress at{section->id, kSection, 0};
		const float x = ImGui::GetCursorScreenPos().x;
		const std::string name = ui_kit::kChangeRoom + section->name();
		const std::string shown = ui_kit::fit(name, ImGui::GetContentRegionAvail().x);
		if (ImGui::Selectable((shown + "###section").c_str(), view.selection.row == section->id)) select(workspace, document, at);
		reveal.scroll_to(at, true);
		const Document::RecordChange change = document.record_change(at);
		ui_kit::change_dot(change, x);
		const std::string words = ui_kit::change_words(change);
		ui_kit::tooltip(shown != name ? section->name() + (words.empty() ? "" : "\n" + words)
		                              : words);
		ImGui::PopID();
	}
	ImGui::PopID();
}

} // namespace

void StringsView::draw(Workspace &workspace, const StringsDocument &document) {
	const SessionView &view = workspace.view();
	reveal_.follow(view, document);
	draw_document_toolbar(workspace, document);
	// The filter, Every section beside it where both fit, else under it.
	const float room = ImGui::GetContentRegionAvail().x - ui_kit::checkbox_width("Every section") - ImGui::GetStyle().ItemSpacing.x;
	const bool beside = room >= ImGui::GetFontSize() * 8.0f;
	ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter keys and text", beside ? room : 0.0f, nullptr, false);
	if (beside) ImGui::SameLine();
	ImGui::Checkbox("Every section", &every_section_);
	ui_kit::tooltip(every_section_ ? "The filter lists the matching strings of every section. "
	                                 "Untick for the selected section's."
	                               : "Tick to list the strings of every section the filter "
	                                 "matches.");
	ImGui::BeginDisabled(document.blocked());
	const auto &rows = document.rows();
	size_t index = SIZE_MAX;
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == view.selection.row) index = i;
	const Node *selected = index < rows.size() ? rows[index].get() : nullptr;
	// The sections down the left, in a column that resizes (first as wide as its widest tool at
	// least), the selected section's strings on the right (or, filtered over every section,
	// each section's that match).
	const float sections = std::max(std::min(ImGui::GetFontSize() * 16.0f, ImGui::GetContentRegionAvail().x * 0.35f),
	                                ui_kit::button_width("Duplicate"));
	if (ImGui::BeginTable("sections", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn("Sections", ImGuiTableColumnFlags_WidthFixed, sections);
		ImGui::TableSetupColumn("Strings", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		draw_sections(workspace, document, reveal_, index);
		ImGui::TableNextColumn();
		if (!selected && !(every_section_ && filter_[0])) {
			ui_kit::empty_state("Select a section to list its strings.");
		} else {
			draw_strings(workspace, document, selected);
		}
		ImGui::EndTable();
	}
	ImGui::EndDisabled();
}

// The selected section's strings: their tools, then the key / text table edited in place,
// each string marked when it was added or changed since the last save (a click on its mark
// selects it, as a click into its key or text does). Filtered over every section, the table
// lists each section's matching strings under a Section column; the tools stay the selected
// section's.
void StringsView::draw_strings(Workspace &workspace, const StringsDocument &document, const Node *section) {
	const SessionView &view = workspace.view();
	const bool every = every_section_ && filter_[0];
	if (section) {
		const std::vector<NodeId> &ids = section->collections.empty() ? std::vector<NodeId>() : section->collections[0];
		size_t at = SIZE_MAX;
		for (size_t i = 0; i < ids.size(); ++i)
			if (view.selection.row == section->id && view.selection.child == ids[i]) at = i;
		ImGui::PushID("strings");
		ui_kit::WrapRow row;
		ui_kit::RowTools tools;
		tools.count = ids.size();
		tools.selected = at;
		tools.pick = "Select a string first (a click on its key, its text or the mark before them).";
		tools.add_tip = "Adds a string at the end of the section.";
		const NodeAddress address = at < ids.size() ? NodeAddress{section->id, kString, ids[at]} : NodeAddress();
		switch (ui_kit::row_tools(row, tools)) {
		case ui_kit::RowTool::Add: edit(workspace, document, EditOperation::Add, {section->id, kString, 0}); break;
		case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, address, at + 1); break;
		case ui_kit::RowTool::Remove: edit(workspace, document, EditOperation::Remove, address); break;
		case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, address, at - 1); break;
		case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, address, at + 1); break;
		case ui_kit::RowTool::None: break;
		}
		ImGui::PopID();
		if (!every && ids.empty()) return ui_kit::empty_state("The section has no strings yet.");
	}
	std::vector<NodeAddress> shown;
	for (const auto &owner : document.rows()) {
		if (!every && owner.get() != section) continue;
		if (owner->collections.empty()) continue;
		for (const NodeId id : owner->collections[0]) {
			const NodeAddress string{owner->id, kString, id};
			if (!filter_[0] || matches(text_of(document, string, "key"), filter_) ||
			    matches(text_of(document, string, "text"), filter_))
				shown.push_back(string);
		}
	}
	if (shown.empty())
		return ui_kit::empty_state(every ? "No string of any section matches the filter." : "No string matches the filter.");
	if (!ImGui::BeginTable("strings", every ? 4 : 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("##changed", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width(ui_kit::kChangeRoom));
	if (every) ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch, 3.0f);
	ImGui::TableHeadersRow();
	for (const NodeAddress &string : shown) {
		ImGui::PushID(int(string.child));
		ImGui::TableNextRow();
		const bool on = view.selection.row == string.row && view.selection.child == string.child;
		if (on) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
		ImGui::TableNextColumn();
		const float x = ImGui::GetCursorScreenPos().x;
		if (ImGui::Selectable("##pick", on, ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetFrameHeight())))
			select(workspace, document, string);
		reveal_.scroll_to(string);
		const Document::RecordChange change = document.record_change(string);
		ui_kit::change_dot(change, x);
		ui_kit::tooltip(*ui_kit::change_words(change) ? ui_kit::change_words(change) : "Select this string.");
		if (every) {
			ImGui::TableNextColumn();
			const std::string name = text_of(document, {string.row, kSection, 0}, "name");
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(ui_kit::fit(name, ImGui::GetContentRegionAvail().x).c_str());
			ui_kit::tooltip(name);
		}
		ImGui::TableNextColumn();
		text_cell(workspace, document, string, "key");
		if (ImGui::IsItemActivated()) select(workspace, document, string);
		ImGui::TableNextColumn();
		text_cell(workspace, document, string, "text");
		if (ImGui::IsItemActivated()) select(workspace, document, string);
		ImGui::PopID();
	}
	ImGui::EndTable();
}

} // namespace opennova::editor
