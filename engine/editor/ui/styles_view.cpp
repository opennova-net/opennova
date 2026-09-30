#include "styles_view.h"

#include <editor/documents/mns_document.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/field_widgets.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/table_cells.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <string>
#include <vector>

#include <imgui.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kVariable = node_kind(StyleKind::Variable);
constexpr NodeKind kComment = node_kind(StyleKind::Comment);
constexpr NodeKind kBlank = node_kind(StyleKind::Blank);

using window_requests::edit;
using window_requests::matches;
using window_requests::select;
using window_requests::set;

// What the game makes of a stylesheet, from its Problems rows.
const char *sheet_status(const SessionView &view, const FindingsIndex &findings,
                         const MnsDocument &document) {
	if (!document.read_by_game()) return "Not read by the game: it reads menu_style.mns and brand.mns only.";
	for (const size_t i : findings.of_file(document.path())) {
		const Diagnostic &d = view.findings.diagnostics[i];
		if (d.severity == DiagnosticSeverity::Error && d.row() && d.row()->group == FindingGroup::Stylesheets)
			return "The game reads it only in part: see Problems.";
	}
	return "Read by the game.";
}

// A line's note in a cell, dimmed and cut to the cell (whole, or `tip`, in its tooltip).
void quiet_text(const std::string &text, const std::string &tip = std::string()) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ui_kit::clipped_text(text, tip);
	ImGui::PopStyleColor();
}

// A colour swatch over the value, read and written as the game's AARRGGBB word (the
// Inspector's HexArgb swatch).
void color_cell(Workspace &workspace, const Document &document, const NodeAddress &address, const std::string &value) {
	Value picked = value;
	const field_widgets::Edited change = field_widgets::swatch(FieldColor::HexArgb, picked);
	if (change.changed) set(workspace, document, address, "value", std::move(picked)); // coalesced: one undo step per drag
	if (change.finished) window_requests::end_edit(workspace, document.path());
}

// Who uses a variable: a button with the count, each use one click away.
void used_by_cell(Workspace &workspace, const std::vector<const GraphEdge *> &users) {
	if (users.empty()) {
		ImGui::TextDisabled("-");
		return;
	}
	const std::string label = std::to_string(users.size()) + (users.size() == 1 ? " use" : " uses");
	if (ImGui::SmallButton(label.c_str())) ImGui::OpenPopup("uses");
	ui_kit::tooltip_lazy([&] {
		std::string tip;
		for (size_t i = 0; i < users.size() && i < 8; ++i)
			tip += (i ? "\n" : "") + users[i]->source + ": " + (users[i]->record.empty() ? users[i]->field : users[i]->record);
		return tip;
	});
	if (!ImGui::BeginPopup("uses")) return;
	for (size_t i = 0; i < users.size(); ++i) {
		const GraphEdge &edge = *users[i];
		ImGui::PushID(int(i));
		const std::string where = edge.source + ": " + (edge.record.empty() ? std::string() : edge.record + " ") + "(" + edge.field + ")";
		if (ImGui::Selectable(where.c_str()))
			window_requests::go_to(workspace, usage_target(*workspace.view().project.scan, edge));
		ImGui::PopID();
	}
	ImGui::EndPopup();
}

} // namespace

void StylesView::draw(Workspace &workspace, const MnsDocument &document) {
	const SessionView &view = workspace.view();
	reveal_.follow(view, document);
	findings_.follow(view);
	draw_document_toolbar(workspace, document);
	ui_kit::empty_state(sheet_status(view, findings_, document));
	ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter names and values", 0.0f, nullptr, false);

	// The lines the table lists: the variables and the lines that decide what the game reads
	// (#if lines and the lines they switch off). Comment and blank lines stay in the file as
	// they are, unlisted: the shipped menu_style.mns has 38 comment lines, its first variable
	// on line 40.
	const auto &rows = document.rows();
	std::vector<size_t> listed;
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->kind != kComment && rows[i]->kind != kBlank) listed.push_back(i);
	// The selected line when the table lists it: Add goes after it, else at the end of the file
	// (a comment or a blank line a Problems row selected is no line of the table, so it places
	// nothing); its own moves, Up and Down to the place of the listed line before or after it.
	size_t index = SIZE_MAX;
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == view.documents.selection.row) index = i;
	const auto place = std::find(listed.begin(), listed.end(), index);
	const size_t at = place == listed.end() ? SIZE_MAX : size_t(place - listed.begin());
	const Node *selected = at != SIZE_MAX ? rows[index].get() : nullptr;
	const size_t insert_at = selected ? index + 1 : rows.size();
	const char *const where = selected ? "Adds one after the selected line."
	                                   : "Adds one at the end of the file: select a line of the table to add after it.";
	ImGui::BeginDisabled(document.blocked());
	ui_kit::WrapRow tools_row;
	if (ui_kit::tool(tools_row, "Add variable", true, where)) edit(workspace, document, EditOperation::Add, {0, kVariable, 0}, insert_at);
	ui_kit::RowTools tools;
	tools.add = nullptr;
	tools.count = listed.size();
	tools.selected = at;
	tools.pick = "Select a line first.";
	if (at != SIZE_MAX && document.frozen(*selected))
		tools.locked = "The selected line stays where it is: #if lines, the lines they switch off, and a value that "
		               "continues across other lines.";
	const NodeAddress address = selected ? NodeAddress{selected->id, selected->kind, 0} : NodeAddress();
	switch (ui_kit::row_tools(tools_row, tools)) {
	case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, address, index + 1); break;
	case ui_kit::RowTool::Remove: edit(workspace, document, EditOperation::Remove, address); break;
	case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, address, listed[at - 1]); break;
	case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, address, listed[at + 1]); break;
	case ui_kit::RowTool::Add:
	case ui_kit::RowTool::None: break;
	}
	ImGui::EndDisabled();
	if (tools.locked) ui_kit::empty_state(tools.locked);

	std::vector<size_t> shown;
	for (const size_t i : listed) {
		Value value;
		const std::string text = document.get({rows[i]->id, rows[i]->kind, 0}, "value", value) ? std::get<std::string>(value)
		                                                                                        : std::string();
		if (!filter_[0] || matches(rows[i]->name(), filter_) || matches(text, filter_)) shown.push_back(i);
	}
	if (listed.empty()) return ui_kit::empty_state("The stylesheet has no variables yet.");
	if (shown.empty()) return ui_kit::empty_state("No line matches the filter.");
	// The columns start at widths of the font's size and resize; narrower than they need,
	// the table scrolls sideways.
	const bool read = document.read_by_game();
	const float em = ImGui::GetFontSize();
	const ImGuiStyle &style = ImGui::GetStyle();
	const float line_width = ui_kit::text_width(ui_kit::kChangeRoom) + ui_kit::text_width("0000") + style.FramePadding.x * 2.0f;
	const float used_width = ui_kit::button_width("99 uses");
	const float needed = line_width + used_width + em * (8.0f + 8.0f + 9.0f + 8.0f + 10.0f);
	if (!ImGui::BeginTable("lines", 7,
	                       ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
	                               ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable,
	                       ImVec2(0.0f, 0.0f), std::max(needed, ImGui::GetContentRegionAvail().x)))
		return;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, line_width);
	ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.5f);
	ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.5f);
	ImGui::TableSetupColumn("##swatch", ImGuiTableColumnFlags_WidthFixed, em * 9.0f);
	ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch, 1.5f);
	ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthFixed, used_width);
	ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	ImGui::TableHeadersRow();
	for (const size_t i : shown) {
		const auto &row = rows[i];
		const NodeAddress line{row->id, row->kind, 0};
		Value value_field, number;
		const std::string value = document.get(line, "value", value_field) ? std::get<std::string>(value_field) : std::string();
		ImGui::PushID(int(row->id));
		ImGui::TableNextRow();
		if (view.documents.selection.row == row->id)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
		ImGui::TableNextColumn();
		// The line's number, marked when it was added or changed since the last save.
		const int64_t at = document.get(line, "line", number) ? std::get<int64_t>(number) : 0;
		const float x = ImGui::GetCursorScreenPos().x;
		if (ImGui::Selectable((ui_kit::kChangeRoom + std::to_string(at)).c_str(), view.documents.selection.row == row->id))
			select(workspace, document, line);
		reveal_.scroll_to(line, true);
		const Document::RecordChange change = document.record_change(line);
		ui_kit::change_dot(change, x);
		ui_kit::tooltip(ui_kit::change_words(change));
		const bool frozen = document.frozen(*row);
		ImGui::TableNextColumn();
		if (row->kind == kVariable) {
			// What the value is used as (the document's answer): a colour, or a font or a texture
			// picked with the reference picker from the project's files the kind loads, or a Files
			// row dropped on the value; its uses, the menus' uses of the definition the game reads.
			const StyleValueUse &use =
			        document.style_value_use(line, view.findings.graph.get(), cache_key(view));
			std::vector<const GraphEdge *> users;
			if (use.bound)
				users = view.findings.graph->referrers_of(ReferenceKind::StyleVar, row->name());
			text_cell(workspace, document, line, "name");
			if (ImGui::IsItemActivated()) select(workspace, document, line);
			ImGui::TableNextColumn();
			text_cell(workspace, document, line, "value");
			if (ImGui::IsItemActivated()) select(workspace, document, line);
			std::string picked;
			const bool drops = use.picks && !document.blocked();
			if (drops && ReferencePicker::accept_file(view, use.file, picked))
				set(workspace, document, line, "value", picked, false);
			ImGui::TableNextColumn();
			if (use.colour) {
				color_cell(workspace, document, line, value);
			} else if (use.picks) {
				const FieldUse &file = use.file;
				if (picker_.draw(workspace, document, line, file, Value(value), false, picked, false))
					set(workspace, document, line, "value", picked, false);
			}
			ImGui::TableNextColumn();
			text_cell(workspace, document, line, "comment");
			if (ImGui::IsItemActivated()) select(workspace, document, line);
			ImGui::TableNextColumn();
			used_by_cell(workspace, users);
			ImGui::TableNextColumn();
			if (!use.winner) quiet_text("defined again below: the game reads that one");
			else if (!read) quiet_text("not read by the game");
			else if (frozen) quiet_text("locked: continues across other lines");
		} else {
			quiet_text(row->name(), row->name() + "\nLocked: #if lines and the lines they switch off stay where they are.");
			for (int skip = 0; skip < 4; ++skip) ImGui::TableNextColumn();
			ImGui::TableNextColumn();
			quiet_text(row->kind == node_kind(StyleKind::Conditional) ? "directive" : "switched off");
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
}

} // namespace opennova::editor
