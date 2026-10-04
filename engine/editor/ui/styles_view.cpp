#include "styles_view.h"

#include <base/io/strutil.h>
#include <editor/documents/mns_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/field_widgets.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/text_edit.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <variant>
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
	// Each use by its file, record and field in words ("main.mnu: STARTUP/MAIN/BUTTONS - Text colour", the
	// plain-words lane).
	const AssetScan *scan = workspace.view().project.scan.get();
	const auto where_of = [scan](const GraphEdge &edge) {
		const AssetEntry *source = scan ? scan->at_path(edge.source) : nullptr;
		const std::string place = edge_place_words(edge, source ? source->kind : AssetKind::Unknown);
		return edge.source + (place.empty() ? std::string() : ": " + place);
	};
	ui_kit::tooltip_lazy([&] {
		std::string tip;
		for (size_t i = 0; i < users.size() && i < 8; ++i) tip += (i ? "\n" : "") + where_of(*users[i]);
		return tip;
	});
	if (!ImGui::BeginPopup("uses")) return;
	for (size_t i = 0; i < users.size(); ++i) {
		const GraphEdge &edge = *users[i];
		ImGui::PushID(int(i));
		const std::string where = where_of(edge);
		if (ImGui::Selectable(where.c_str()))
			window_requests::go_to(workspace, usage_target(*workspace.view().project.scan, edge));
		ImGui::PopID();
	}
	ImGui::EndPopup();
}

// A variable's text field by its id, from the kind's schema (null for none).
const FieldSchema *variable_field(const MnsDocument &document, const char *id) {
	for (const FieldSchema &field : document.fields(kVariable))
		if (field.id == id) return &field;
	return nullptr;
}

} // namespace

void StylesView::rebind(const DocumentBase &) {
	// The reveal names the records of the document it last drew: the next draw follows anew.
	reveal_ = RecordReveal();
}

// The lines the table lists: the variables and the lines that decide what the game reads (#if
// lines and the lines they switch off). Comment and blank lines stay in the file as they are,
// unlisted: the shipped menu_style.mns has 38 comment lines, its first variable on line 40. Those
// the filter shows: a name or a value holding it. After an edit, an undo or a redo (the revision
// alone moved), the lines its change set names changed or added are matched again, the others
// keeping their match, and the places are made again from the rows' order.
void StylesView::refresh_lines(const MnsDocument &document) {
	const bool same = lines_.made && lines_.document == document.identity() &&
	                  lines_.load == document.load_generation() && lines_.filter == filter_;
	if (same && lines_.revision == document.revision()) return;
	ChangeSet set;
	const RowChanges *changes =
	        same && document.changes_since(lines_.load, lines_.revision, set) ? std::get_if<RowChanges>(&set) : nullptr;
	if (changes) {
		for (const NodeId id : changes->removed) lines_.matched.erase(id);
	} else {
		lines_.matched.clear();
	}
	lines_.made = true;
	lines_.document = document.identity();
	lines_.load = document.load_generation();
	lines_.revision = document.revision();
	lines_.filter = filter_;
	lines_.listed.clear();
	lines_.shown.clear();
	const auto &rows = document.rows();
	for (size_t i = 0; i < rows.size(); ++i) {
		const Node &row = *rows[i];
		if (row.kind == kComment || row.kind == kBlank) continue;
		lines_.listed.push_back(i);
		if (!filter_[0]) {
			lines_.shown.push_back(i);
			continue;
		}
		auto kept = lines_.matched.find(row.id);
		if (kept == lines_.matched.end() || (changes && changes->was_changed(row.id))) {
			Value value;
			const std::string text =
			        document.get({row.id, row.kind, 0}, "value", value) ? std::get<std::string>(value) : std::string();
			kept = lines_.matched.insert_or_assign(row.id, matches(row.name(), filter_) || matches(text, filter_)).first;
			++lines_matched_;
		}
		if (kept->second) lines_.shown.push_back(i);
	}
}

bool StylesView::follow_uses(const MnsDocument &document) {
	ChangeSet set;
	if (!document.changes_since(uses_.load, uses_.revision, set)) return false;
	const RowChanges *changes = std::get_if<RowChanges>(&set);
	if (!changes || changes->reshapes() || changes->file_state) return false;
	// A changed variable's use goes, and with it the uses of the lines of the name it had and has now.
	std::vector<std::string> names;
	for (const NodeId id : changes->changed) {
		const Node *row = document.row(id);
		if (!row || row->kind != kVariable) return false;
		const auto had = uses_.names.find(id);
		if (had != uses_.names.end()) names.push_back(had->second);
		std::string now = strutil::to_upper(row->name());
		names.push_back(now);
		uses_.names[id] = std::move(now);
		uses_.rows.erase(id);
	}
	for (auto it = uses_.rows.begin(); it != uses_.rows.end();) {
		const auto name = uses_.names.find(it->first);
		const bool shares = name != uses_.names.end() &&
		                    std::find(names.begin(), names.end(), name->second) != names.end();
		it = shares ? uses_.rows.erase(it) : std::next(it);
	}
	return true;
}

const StylesView::LineUse &StylesView::use_of(const MnsDocument &document, const NodeAddress &line,
                                              const AssetGraph *graph) {
	const uint64_t generation = graph ? graph->generation() : 0;
	const bool same = uses_.document == document.identity() && uses_.load == document.load_generation() &&
	                  uses_.graph == graph && uses_.generation == generation;
	if (!same || uses_.revision != document.revision()) {
		// The graph or the load moved, or a change set the uses cannot follow: every use again.
		if (!same || !follow_uses(document)) {
			uses_.rows.clear();
			uses_.names.clear();
			for (const auto &row : document.rows())
				if (row->kind == kVariable) uses_.names.emplace(row->id, strutil::to_upper(row->name()));
			++uses_made_;
		}
		uses_.document = document.identity();
		uses_.load = document.load_generation();
		uses_.revision = document.revision();
		uses_.graph = graph;
		uses_.generation = generation;
	}
	const auto kept = uses_.rows.find(line.row);
	if (kept != uses_.rows.end()) return kept->second;
	LineUse &out = uses_.rows[line.row];
	out.use = style_value_use(document, line, graph);
	if (out.use.bound) out.users = graph->referrers_of(ReferenceKind::StyleVar, document.row(line.row)->name());
	++lines_used_;
	return out;
}

void StylesView::draw(Workspace &workspace, const DocumentBase &base) {
	// A RevealRecord taken: the selection shown again, where it was already too.
	for (const ViewEvent &event : take_events()) {
		(void)event;
		reveal_.ask();
	}
	const auto *document = dynamic_cast<const MnsDocument *>(records_of(base));
	if (!document) return ui_kit::empty_state("This file holds no stylesheet lines to list.");
	const SessionView &view = workspace.view();
	reveal_.follow(view, *document);
	findings_.follow(view);
	draw_document_toolbar(workspace, *document);
	ui_kit::empty_state(sheet_status(view, findings_, *document));
	ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter names and values", 0.0f, nullptr, false);
	refresh_lines(*document);
	const std::vector<size_t> &listed = lines_.listed;
	const std::vector<size_t> &shown = lines_.shown;

	// The selected line when the table lists it: Add goes after it, else at the end of the file
	// (a comment or a blank line a Problems row selected is no line of the table, so it places
	// nothing); its own moves, Up and Down to the place of the listed line before or after it.
	const auto &rows = document->rows();
	size_t index = SIZE_MAX;
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == view.documents.selection.primary.row) index = i;
	const auto place = std::find(listed.begin(), listed.end(), index);
	const size_t at = place == listed.end() ? SIZE_MAX : size_t(place - listed.begin());
	const Node *selected = at != SIZE_MAX ? rows[index].get() : nullptr;
	const size_t insert_at = selected ? index + 1 : rows.size();
	const char *const where = selected ? "Adds one after the selected line."
	                                   : "Adds one at the end of the file: select a line of the table to add after it.";
	ImGui::BeginDisabled(document->blocked());
	ui_kit::WrapRow tools_row;
	if (ui_kit::tool(tools_row, "Add variable", true, where)) edit(workspace, *document, EditOperation::Add, {0, kVariable, 0}, insert_at);
	ui_kit::RowTools tools;
	tools.add = nullptr;
	tools.count = listed.size();
	tools.selected = at;
	tools.pick = "Select a line first.";
	if (at != SIZE_MAX && document->frozen(*selected))
		tools.locked = "The selected line stays where it is: #if lines, the lines they switch off, and a value that "
		               "continues across other lines.";
	const NodeAddress address = selected ? NodeAddress{selected->id, selected->kind, 0} : NodeAddress();
	switch (ui_kit::row_tools(tools_row, tools)) {
	case ui_kit::RowTool::Duplicate: edit(workspace, *document, EditOperation::Duplicate, address, index + 1); break;
	case ui_kit::RowTool::Remove: edit(workspace, *document, EditOperation::Remove, address); break;
	case ui_kit::RowTool::Up: edit(workspace, *document, EditOperation::Move, address, listed[at - 1]); break;
	case ui_kit::RowTool::Down: edit(workspace, *document, EditOperation::Move, address, listed[at + 1]); break;
	case ui_kit::RowTool::Add:
	case ui_kit::RowTool::None: break;
	}
	ImGui::EndDisabled();
	if (tools.locked) ui_kit::empty_state(tools.locked);

	if (listed.empty()) return ui_kit::empty_state("The stylesheet has no variables yet.");
	if (shown.empty()) return ui_kit::empty_state("No line matches the filter.");
	// The columns start at widths of the font's size and resize; narrower than they need,
	// the table scrolls sideways.
	const bool read = document->read_by_game();
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
	const FieldSchema *name_field = variable_field(*document, "name");
	const FieldSchema *value_field = variable_field(*document, "value");
	const FieldSchema *comment_field = variable_field(*document, "comment");
	// Every row one control high, so the table clips: only the lines in sight draw (and the one
	// being revealed, to scroll to).
	const float row_height = ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f;
	size_t revealed = SIZE_MAX;
	if (reveal_.moved())
		for (size_t s = 0; s < shown.size() && revealed == SIZE_MAX; ++s)
			if (rows[shown[s]]->id == reveal_.path().front().row) revealed = s;
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(shown.size()), row_height);
	if (revealed != SIZE_MAX) clipper.IncludeItemByIndex(static_cast<int>(revealed));
	while (clipper.Step())
		for (int s = clipper.DisplayStart; s < clipper.DisplayEnd; ++s) {
			const auto &row = rows[shown[size_t(s)]];
			const NodeAddress line{row->id, row->kind, 0};
			Value value_text, number;
			const std::string value = document->get(line, "value", value_text) ? std::get<std::string>(value_text) : std::string();
			ImGui::PushID(int(row->id));
			ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
			if (view.documents.selection.primary.row == row->id)
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
			ImGui::TableNextColumn();
			// The line's number, marked when it was added or changed since the last save.
			const int64_t at_line = document->get(line, "line", number) ? std::get<int64_t>(number) : 0;
			const float x = ImGui::GetCursorScreenPos().x;
			if (ImGui::Selectable((ui_kit::kChangeRoom + std::to_string(at_line)).c_str(), view.documents.selection.primary.row == row->id))
				select(workspace, *document, line);
			reveal_.scroll_to(line, true);
			const Document::RecordChange change = document->record_change(line);
			ui_kit::change_dot(change, x);
			ui_kit::tooltip(ui_kit::change_words(change));
			const bool frozen = document->frozen(*row);
			ImGui::TableNextColumn();
			if (row->kind == kVariable) {
				// What the value is used as (graph/style_value_use, kept while the document and the
				// graph stand): a colour, or a font or a texture picked with the reference picker from
				// the project's files the kind loads, or a Files row dropped on the value; its uses,
				// the menus' uses of the definition the game reads.
				const LineUse &use = use_of(*document, line, view.findings.graph.get());
				const auto cell = [&](const FieldSchema *field) {
					if (field && text_edit::cell(workspace, *document, line, *field) && ImGui::IsItemActivated())
						select(workspace, *document, line);
				};
				cell(name_field);
				ImGui::TableNextColumn();
				cell(value_field);
				std::string picked;
				const bool drops = use.use.picks && !document->blocked();
				if (drops && ReferencePicker::accept_file(view, use.use.file, picked))
					set(workspace, *document, line, "value", picked, false);
				ImGui::TableNextColumn();
				if (use.use.colour) {
					color_cell(workspace, *document, line, value);
				} else if (use.use.picks) {
					if (picker_.draw(workspace, *document, line, use.use.file, Value(value), false, picked, false))
						set(workspace, *document, line, "value", picked, false);
				}
				ImGui::TableNextColumn();
				cell(comment_field);
				ImGui::TableNextColumn();
				used_by_cell(workspace, use.users);
				ImGui::TableNextColumn();
				if (!use.use.winner) quiet_text("defined again below: the game reads that one");
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
