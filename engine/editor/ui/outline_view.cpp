#include "outline_view.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <editor/session/view/session_view.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/field_widgets.h>
#include <editor/ui/text_edit.h>
#include <editor/ui/ui_kit.h>

#include <imgui.h>

namespace opennova::editor {

namespace {

using window_requests::edit;
using window_requests::select;

// The first kind of row the file holds (RecordKindRow::top): the rows the master column lists and
// adds.
const RecordKindRow *own_kind(const Document &document) {
	for (const RecordKindRow &row : document.kinds())
		if (row.top) return &row;
	return nullptr;
}

std::string lower(std::string text) {
	for (char &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

// A record's tooltip in a tree: the token its type words where its title words it otherwise
// ("anim_walk_forward" under "walk forward"), then whether it changed since the last save.
std::string record_tip(const OutlineLine &line, Document::RecordChange change) {
	const std::string words = ui_kit::change_words(change);
	std::string tip = line.name;
	if (!words.empty()) tip += (tip.empty() ? "" : "\n") + words;
	return tip;
}

// A row's line in a list or a master column, cut to what shows of it (whole in its tooltip),
// marked when it was added or changed since the last save, selected on a click and highlighted
// while the selection is in it; `id` its item's id after its text.
void row_line(Workspace &workspace, const Document &document, const RecordReveal &reveal, const OutlineLine &line,
              const char *id) {
	const SessionView &view = workspace.view();
	ImGui::PushID(static_cast<int>(line.address.row));
	const float x = ImGui::GetCursorScreenPos().x;
	const std::string label = ui_kit::kChangeRoom + line.text;
	const std::string shown = ui_kit::fit(label, ImGui::GetContentRegionAvail().x);
	if (ImGui::Selectable((shown + id).c_str(), view.documents.selection.row == line.address.row))
		select(workspace, document, line.address);
	reveal.scroll_to(line.address, true);
	const Document::RecordChange change = document.record_change(line.address);
	ui_kit::change_dot(change, x);
	ui_kit::tooltip_lazy([&] {
		const std::string words = ui_kit::change_words(change);
		return shown != label ? line.text + (words.empty() ? "" : "\n" + words) : words;
	});
	ImGui::PopID();
}

// The selected record's Duplicate / Remove / Up / Down, as the tools pressed ask them of `address`
// at `index` in its list.
void row_tool(Workspace &workspace, const Document &document, ui_kit::RowTool tool, const NodeAddress &address,
              size_t index) {
	switch (tool) {
	case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, address, index + 1); break;
	case ui_kit::RowTool::Remove: edit(workspace, document, EditOperation::Remove, address); break;
	case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, address, index - 1); break;
	case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, address, index + 1); break;
	case ui_kit::RowTool::Add:
	case ui_kit::RowTool::None: break;
	}
}

// Whether the file adds rows of `kind` at the top level (RecordKindRow::add_label): those rows
// are a list like any collection that is not fixed; the others are the file's fixed records.
bool adds_rows_of(const Document &document, NodeKind kind) {
	const RecordKindRow *row = document.kind_row(kind);
	return row && *row->add_label;
}

} // namespace

OutlineView::OutlineView(const OutlineSpec &spec) : spec_(spec), model_(spec.mode, spec.file_values) {}

void OutlineView::rebind(const DocumentBase &) {
	// The reveal and the cell being edited name the records of the document it last drew: the next
	// draw follows anew.
	reveal_ = RecordReveal();
	editing_ = NodeAddress();
}

void OutlineView::filter_box(const char *hint, float width, const char *tip) {
	const std::string &filter = model_.filter();
	if (filter != filter_) {
		const size_t size = std::min(filter.size(), sizeof(filter_) - 1);
		std::memcpy(filter_, filter.data(), size);
		filter_[size] = '\0';
	}
	ui_kit::filter_box("##filter", filter_, sizeof(filter_), hint, width, tip, false);
	model_.set_filter(filter_);
}

void OutlineView::draw(Workspace &workspace, const DocumentBase &base) {
	// A RevealRecord taken: the selection shown again, where it was already too.
	for (const ViewEvent &event : take_events()) {
		(void)event;
		reveal_.ask();
	}
	const Document *document = records_of(base);
	if (!document) return ui_kit::empty_state("This file holds no records to list.");
	reveal_.follow(workspace.view(), *document);
	draw_document_toolbar(workspace, *document);
	switch (spec_.mode) {
	case OutlineMode::List: draw_list(workspace, *document); break;
	case OutlineMode::Tree: draw_tree(workspace, *document); break;
	case OutlineMode::MasterDetail: draw_master_detail(workspace, *document); break;
	}
}

// --- List (a catalog) ---------------------------------------------------------------------------

// The filter and the sort, each kind's Add and the selected row's tools, the rows (clipped), then
// the file-wide values.
void OutlineView::draw_list(Workspace &workspace, const Document &document) {
	const SessionView &view = workspace.view();
	{
		ui_kit::WrapRow row;
		const float filter = ImGui::GetFontSize() * 14.0f;
		row.next(filter);
		filter_box("Filter records", filter, nullptr);
		row.next(ui_kit::checkbox_width("Sort by name"));
		bool sort = model_.sort();
		if (ImGui::Checkbox("Sort by name", &sort)) model_.set_sort(sort);
		ui_kit::tooltip("Lists the records by name; Up and Down still move them in the file's order.");
	}
	// The selection moved there: its row shown (a filter hiding it cleared) and scrolled to, however
	// far down.
	const size_t revealed = reveal_.moved() ? model_.reveal(document, reveal_.path()) : SIZE_MAX;
	ImGui::BeginDisabled(document.blocked());
	const auto &rows = document.rows();
	size_t index = SIZE_MAX; // the selected record's row, its place in the file
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == view.documents.selection.row) index = i;
	{
		// Each kind's Add, then the selected record's tools, on a row that wraps.
		ui_kit::WrapRow row;
		for (const RecordKindRow &kind : document.kinds())
			if (*kind.add_label && ui_kit::tool(row, kind.add_label, true, "Adds one at the end of the file."))
				edit(workspace, document, EditOperation::Add, {0, kind.kind, 0});
		ui_kit::RowTools tools;
		tools.add = nullptr;
		tools.count = rows.size();
		tools.selected = index;
		const NodeAddress address = index < rows.size() ? NodeAddress{rows[index]->id, rows[index]->kind, 0} : NodeAddress();
		row_tool(workspace, document, ui_kit::row_tools(row, tools), address, index);
	}
	const std::vector<OutlineLine> &lines = model_.lines(document);
	if (rows.empty()) ui_kit::empty_state("The file has no records yet.", "Add one with the buttons above.");
	else if (lines.empty()) ui_kit::empty_state("No record matches the filter.");
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(lines.size()));
	if (revealed != SIZE_MAX) clipper.IncludeItemByIndex(static_cast<int>(revealed));
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			row_line(workspace, document, reveal_, lines[size_t(i)], "###record");
	ImGui::EndDisabled();
	draw_file_values(workspace, document);
}

// The file-wide values (OutlineModel::file_values), under a heading marked while they differ from
// the saved file's: each by its place, and one more added at the end while there is room.
void OutlineView::draw_file_values(Workspace &workspace, const Document &document) {
	OutlineFileValues values;
	if (!model_.file_values(document, values)) return;
	const float x = ImGui::GetCursorScreenPos().x;
	const Document::RecordChange change =
	        document.file_state_changed() ? Document::RecordChange::Changed : Document::RecordChange::Unchanged;
	// The heading open or closed as the model has it; a click on it changes the model.
	ImGui::SetNextItemOpen(model_.values_open());
	const bool open = ImGui::TreeNode("file_values", "%s%s", ui_kit::kChangeRoom, values.title.c_str());
	model_.set_values_open(open);
	ui_kit::change_dot(change, x + ImGui::GetTreeNodeToLabelSpacing());
	ui_kit::tooltip(ui_kit::change_words(change));
	if (!open) return;
	const auto set = [&](size_t place, int value) {
		Edit edit;
		edit.operation = EditOperation::SetFileValue;
		edit.position = place;
		edit.value = int64_t(value);
		workspace.request(request::edit_record(document.path(), std::move(edit)));
	};
	ImGui::BeginDisabled(document.blocked());
	for (size_t i = 0; i < values.values.size(); ++i) {
		int value = int(values.values[i]);
		ImGui::PushID(static_cast<int>(i));
		const std::string label = values.label + " " + std::to_string(i + 1);
		const float room = ImGui::GetContentRegionAvail().x - ui_kit::field_width(0.0f, label.c_str());
		ImGui::SetNextItemWidth(std::clamp(room, ImGui::GetFontSize() * 4.0f, ImGui::GetFontSize() * 10.0f));
		if (ImGui::InputInt(label.c_str(), &value)) set(i, value);
		ui_kit::tooltip(values.tip);
		ImGui::PopID();
	}
	ui_kit::WrapRow row;
	const bool room = values.values.size() < values.max;
	if (ui_kit::tool(row, values.add_label.c_str(), room, room ? values.add_tip : values.full_tip))
		set(values.values.size(), 0);
	ImGui::EndDisabled();
	ImGui::TreePop();
}

// --- Tree (a model, a clip, an animation table) -------------------------------------------------

// The filter, the rows the file adds and the selected record's tools, then the tree (clipped,
// scrolling sideways when deep).
void OutlineView::draw_tree(Workspace &workspace, const Document &document) {
	filter_box("Filter records", 0.0f, "Lists the records whose name holds the text, and what holds them.");
	// The selection moved there: the records and collections holding it open (a filter hiding it
	// cleared), its line scrolled to, however far down.
	const size_t revealed = reveal_.moved() ? model_.reveal(document, reveal_.path()) : SIZE_MAX;
	ImGui::BeginDisabled(document.blocked());
	draw_tree_tools(workspace, document);
	ImGui::Separator();
	if (ImGui::BeginChild("outline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
		const std::vector<OutlineLine> &lines = model_.lines(document);
		if (document.rows().empty()) ui_kit::empty_state("The file holds no records.");
		else if (lines.empty()) ui_kit::empty_state("No record matches the filter.");
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(lines.size()));
		if (revealed != SIZE_MAX) clipper.IncludeItemByIndex(static_cast<int>(revealed));
		while (clipper.Step())
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				draw_tree_line(workspace, document, lines[size_t(i)]);
	}
	ImGui::EndChild();
	ImGui::EndDisabled();
}

// One line of the tree at its depth: a record (a node over its collections, or a leaf; marked when
// it was added or changed since the last save; selected on a click, Ctrl joining or leaving the
// selection) or a collection (a node over its records, + adding one at its end where it takes
// one). What is open is the model's: the arrow opens or closes the line there (a line the filter
// holds open stays so).
void OutlineView::draw_tree_line(Workspace &workspace, const Document &document, const OutlineLine &line) {
	const SessionView &view = workspace.view();
	const float indent = ImGui::GetStyle().IndentSpacing * float(line.depth);
	if (indent > 0.0f) ImGui::Indent(indent);
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (line.branch) ImGui::SetNextItemOpen(line.open);
	else flags |= ImGuiTreeNodeFlags_Leaf;
	if (line.collection) {
		ImGui::PushID(document.kind_token(line.kind));
		ImGui::PushID(static_cast<int>(line.address.child ? line.address.child : line.address.row));
		ImGui::TreeNodeEx("##collection", flags | ImGuiTreeNodeFlags_AllowOverlap, "%s", line.text.c_str());
		if (line.branch && ImGui::IsItemToggledOpen()) model_.set_open(line, !line.open);
		if (line.addable) {
			ImGui::SameLine();
			if (ImGui::SmallButton("+"))
				edit(workspace, document, EditOperation::Add, {line.address.row, line.kind, 0}, SIZE_MAX, line.address.child);
			ui_kit::tooltip("Adds one at the end.");
		}
		ImGui::PopID();
		ImGui::PopID();
	} else {
		flags |= ImGuiTreeNodeFlags_OpenOnArrow;
		if (holds(view.documents.selected, line.address)) flags |= ImGuiTreeNodeFlags_Selected;
		const float x = ImGui::GetCursorScreenPos().x;
		const std::string label = ui_kit::kChangeRoom + line.text;
		const NodeId id = line.address.child ? line.address.child : line.address.row;
		ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<uintptr_t>(id)), flags, "%s", label.c_str());
		const bool toggled = ImGui::IsItemToggledOpen();
		if (line.branch && toggled) model_.set_open(line, !line.open);
		reveal_.scroll_to(line.address);
		const Document::RecordChange change = document.record_change(line.address);
		ui_kit::change_dot(change, x + ImGui::GetTreeNodeToLabelSpacing());
		ui_kit::tooltip_lazy([&] { return record_tip(line, change); });
		if (ImGui::IsItemClicked() && !toggled)
			select(workspace, document, line.address, ImGui::GetIO().KeyCtrl ? SelectMode::Toggle : SelectMode::Replace);
	}
	if (indent > 0.0f) ImGui::Unindent(indent);
}

// The rows the file adds (one tool per kind with an add_label: a row at the end of the file), the
// selected record's name and its tools, Duplicate / Remove / Up / Down, where its list allows
// them: a nested record's collection, or the rows of a kind the file adds (a collection's + adds a
// nested one).
void OutlineView::draw_tree_tools(Workspace &workspace, const Document &document) {
	const NodeAddress selection = workspace.view().documents.selection;
	Document::Placement at;
	const bool nested = selection.child != 0 && document.placement(selection, at);
	size_t row_index = SIZE_MAX;
	if (selection.child == 0 && selection.row != 0)
		for (size_t i = 0; i < document.rows().size() && row_index == SIZE_MAX; ++i)
			if (document.rows()[i]->id == selection.row) row_index = i;
	const bool placed = nested || row_index != SIZE_MAX;
	ui_kit::RowTools tools;
	tools.add = nullptr;
	tools.small = true;
	tools.pick = "Select a record to duplicate, remove or move it.";
	if (nested) {
		for (const Document::Collection &c : document.collections_of(at.owner))
			if (c.spec.kind == at.spec.kind) tools.count = c.ids.size();
		tools.selected = at.index;
		tools.max = at.spec.max;
		if (at.spec.fixed) tools.locked = "Its list is fixed: nothing in it is added, duplicated, removed or moved.";
	} else if (placed) {
		at.index = row_index;
		tools.count = document.rows().size();
		tools.selected = row_index;
		if (!adds_rows_of(document, selection.kind))
			tools.locked = "It is one of the file's own records: none is added, duplicated, removed or moved.";
	}
	ui_kit::WrapRow row;
	for (const RecordKindRow &kind : document.kinds())
		if (*kind.add_label && ui_kit::tool(row, kind.add_label, true, "Adds one at the end of the file.", true))
			edit(workspace, document, EditOperation::Add, {0, kind.kind, 0});
	if (placed) {
		const std::string title = document.record_title(selection), name = document.record_name(selection);
		const std::string shown = ui_kit::fit(title, ImGui::GetFontSize() * 12.0f);
		row.next(ui_kit::text_width(shown.c_str()));
		ImGui::TextUnformatted(shown.c_str());
		std::string tip = shown != title ? title : std::string();
		if (name != title) tip += (tip.empty() ? "" : "\n") + name;
		ui_kit::tooltip(tip);
	}
	row_tool(workspace, document, ui_kit::row_tools(row, tools), selection, at.index);
}

// --- MasterDetail (a string table) ---------------------------------------------------------------

// The filter and Every, then the rows in a column that resizes (first as wide as its widest tool
// at least) and the selected row's records beside them (or, filtered over every row, each row's
// that match).
void OutlineView::draw_master_detail(Workspace &workspace, const Document &document) {
	const SessionView &view = workspace.view();
	const RecordKindRow *rows_kind = own_kind(document);
	const std::string row_words = rows_kind ? lower(rows_kind->label) : std::string("row");
	const std::string detail_words = lower(model_.detail_label());
	const std::string every = "Every " + row_words;
	// The filter, Every beside it where both fit, else under it.
	const float room = ImGui::GetContentRegionAvail().x - ui_kit::checkbox_width(every.c_str()) -
	                   ImGui::GetStyle().ItemSpacing.x;
	const bool beside = room >= ImGui::GetFontSize() * 8.0f;
	const std::string hint = detail_words.empty() ? std::string("Filter") : "Filter " + detail_words;
	filter_box(hint.c_str(), beside ? room : 0.0f, nullptr);
	if (beside) ImGui::SameLine();
	bool ticked = model_.every();
	if (ImGui::Checkbox(every.c_str(), &ticked)) model_.set_every(ticked);
	ui_kit::tooltip(model_.every() ? "The filter lists what matches in every " + row_words + ". Untick for the selected one's."
	                               : "Tick for the filter to list what matches in every " + row_words + ".");
	ImGui::BeginDisabled(document.blocked());
	const Node *master = document.row(view.documents.selection.row);
	const NodeId master_id = master ? master->id : 0;
	// The selection moved to a record: its line shown (a filter hiding it cleared) and scrolled to.
	const size_t revealed = reveal_.moved() ? model_.reveal(document, reveal_.path(), master_id) : SIZE_MAX;
	model_.lines(document, master_id);
	const float masters = std::max(std::min(ImGui::GetFontSize() * 16.0f, ImGui::GetContentRegionAvail().x * 0.35f),
	                               ui_kit::button_width("Duplicate"));
	if (ImGui::BeginTable("master", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn(spec_.rows, ImGuiTableColumnFlags_WidthFixed, masters);
		ImGui::TableSetupColumn(model_.detail_label(), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		draw_masters(workspace, document);
		ImGui::TableNextColumn();
		if (!master && !model_.every_row())
			ui_kit::empty_state(("Select one of the " + lower(spec_.rows) + " to list what it holds.").c_str());
		else
			draw_details(workspace, document, master, revealed);
		ImGui::EndTable();
	}
	ImGui::EndDisabled();
}

// The rows: their tools (plain words: the column is narrow, its header names the list), then each
// row, marked when it was added or changed since the last save.
void OutlineView::draw_masters(Workspace &workspace, const Document &document) {
	const SessionView &view = workspace.view();
	const auto &rows = document.rows();
	size_t index = SIZE_MAX;
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == view.documents.selection.row) index = i;
	ImGui::PushID("masters");
	ui_kit::WrapRow row;
	ui_kit::RowTools tools;
	tools.count = rows.size();
	tools.selected = index;
	tools.add_tip = "Adds one at the end of the file.";
	tools.duplicate_tip = "A copy of the selected one with what it holds, right after it, under a name of its own.";
	tools.remove_tip = "Removes the selected one with what it holds.";
	const NodeAddress address = index < rows.size() ? NodeAddress{rows[index]->id, rows[index]->kind, 0} : NodeAddress();
	const ui_kit::RowTool tool = ui_kit::row_tools(row, tools);
	if (tool == ui_kit::RowTool::Add) {
		if (const RecordKindRow *kind = own_kind(document))
			edit(workspace, document, EditOperation::Add, {0, kind->kind, 0});
	}
	row_tool(workspace, document, tool, address, index);
	if (rows.empty()) ui_kit::empty_state(("No " + lower(spec_.rows) + " yet.").c_str());
	for (const OutlineLine &line : model_.masters()) row_line(workspace, document, reveal_, line, "###row");
	ImGui::PopID();
}

// The master row's records: their tools, then the table of their text fields edited in place, each
// record marked when it was added or changed since the last save (a click on its mark selects it,
// as a click into one of its cells does), a cell as tall as its text's lines (eight at most); only
// the rows in sight draw their cells (and the one being revealed, `revealed`, to scroll to). Filtered
// over every row, the table lists each row's matching records under a column of the row's name;
// the tools stay the master row's.
void OutlineView::draw_details(Workspace &workspace, const Document &document, const Node *master, size_t revealed) {
	const SessionView &view = workspace.view();
	const bool every = model_.every_row();
	const NodeKind kind = model_.detail_kind();
	if (master) {
		std::vector<NodeId> ids;
		for (const Document::Collection &collection : document.collections_of({master->id, master->kind, 0}))
			if (collection.spec.kind == kind) ids = collection.ids;
		size_t at = SIZE_MAX;
		for (size_t i = 0; i < ids.size(); ++i)
			if (view.documents.selection.row == master->id && view.documents.selection.child == ids[i]) at = i;
		ImGui::PushID("details");
		ui_kit::WrapRow row;
		ui_kit::RowTools tools;
		tools.count = ids.size();
		tools.selected = at;
		tools.pick = "Select one first (a click on its mark, or into one of its cells).";
		tools.add_tip = "Adds one at the end of the selected one's list.";
		const NodeAddress address = at < ids.size() ? NodeAddress{master->id, kind, ids[at]} : NodeAddress();
		const ui_kit::RowTool tool = ui_kit::row_tools(row, tools);
		if (tool == ui_kit::RowTool::Add) edit(workspace, document, EditOperation::Add, {master->id, kind, 0});
		row_tool(workspace, document, tool, address, at);
		ImGui::PopID();
		if (!every && ids.empty()) return ui_kit::empty_state("Nothing in it yet.");
	}
	const std::vector<OutlineLine> &lines = model_.lines(document, master ? master->id : 0);
	if (lines.empty())
		return ui_kit::empty_state(every ? "Nothing in any of them matches the filter." : "Nothing matches the filter.");
	const std::vector<const FieldSchema *> &columns = model_.columns();
	const int count = (every ? 2 : 1) + static_cast<int>(columns.size());
	if (!ImGui::BeginTable("records", count, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("##changed", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width(ui_kit::kChangeRoom));
	const RecordKindRow *rows_kind = own_kind(document);
	if (every) ImGui::TableSetupColumn(rows_kind ? rows_kind->label : "Row", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	for (const FieldSchema *field : columns)
		ImGui::TableSetupColumn(field_widgets::column_header(*field).c_str(), ImGuiTableColumnFlags_WidthStretch,
		                        field->multiline ? 3.0f : 1.0f);
	ImGui::TableHeadersRow();
	// Where the table's rows show (its own scrolling window), and how tall a row is: its tallest
	// cell's box and the table's padding.
	const ImGuiStyle &style = ImGui::GetStyle();
	const float top = ImGui::GetWindowPos().y, bottom = top + ImGui::GetWindowHeight();
	const auto height_of = [&](const OutlineLine &line) {
		return ImGui::GetTextLineHeight() * float(line.lines) + style.FramePadding.y * 2.0f + style.CellPadding.y * 2.0f;
	};
	// A row out of sight keeps its height and draws no cell, but for the row past each edge of what
	// shows (Tab and Shift+Tab move into it, and it scrolls into sight), the one being revealed (to
	// scroll to) and the one whose cell has the keyboard (it keeps it, and ends its edit, however the
	// table scrolled).
	NodeAddress editing;
	bool past_bottom = false;
	for (size_t i = 0; i < lines.size(); ++i) {
		const OutlineLine &line = lines[i];
		const float height = height_of(line);
		ImGui::TableNextRow(ImGuiTableRowFlags_None, height);
		const float y = ImGui::GetCursorScreenPos().y;
		bool drawn = y <= bottom && y + height >= top;
		if (!drawn && y > bottom && !past_bottom) drawn = past_bottom = true;
		else if (!drawn && y + height < top && i + 1 < lines.size() && y + height + height_of(lines[i + 1]) >= top)
			drawn = true; // the row just above what shows
		if (!drawn && i != revealed && !(line.address == editing_)) continue;
		ImGui::PushID(static_cast<int>(line.address.child));
		const bool on = view.documents.selection.row == line.address.row && view.documents.selection.child == line.address.child;
		if (on) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
		ImGui::TableNextColumn();
		const float x = ImGui::GetCursorScreenPos().x;
		if (ImGui::Selectable("##pick", on, ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetFrameHeight())))
			select(workspace, document, line.address);
		reveal_.scroll_to(line.address);
		const Document::RecordChange change = document.record_change(line.address);
		ui_kit::change_dot(change, x);
		ui_kit::tooltip(*ui_kit::change_words(change) ? ui_kit::change_words(change) : "Select this one.");
		if (every) {
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(ui_kit::fit(line.row_name, ImGui::GetContentRegionAvail().x).c_str());
			ui_kit::tooltip(line.row_name);
		}
		for (const FieldSchema *field : columns) {
			ImGui::TableNextColumn();
			if (!text_edit::cell(workspace, document, line.address, *field, line.lines)) continue;
			if (ImGui::IsItemActivated()) select(workspace, document, line.address);
			if (ImGui::IsItemActive()) editing = line.address;
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
	editing_ = editing;
}

} // namespace opennova::editor
