#include "outline_view.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_table.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/code_text_keys.h>
#include <editor/graph/display_names.h>
#include <editor/graph/jump_queries.h>
#include <editor/model/field_text.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/field_widgets.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/project_find.h>
#include <editor/ui/text_edit.h>
#include <editor/ui/texture_preview.h>
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

// A record's tooltip in a tree: the token its type words where its title words it otherwise
// ("anim_walk_forward" under "walk forward"), then whether it changed since the last save.
std::string record_tip(const OutlineLine &line, Document::RecordChange change) {
	const std::string words = ui_kit::change_words(change);
	// The whole line where the column cuts it (a mission's event, its sentence), then its name.
	std::string tip = line.text;
	if (!line.name.empty()) tip += "\n" + line.name;
	if (!words.empty()) tip += (tip.empty() ? "" : "\n") + words;
	return tip;
}

// A row's line in a list or a master column, cut to what shows of it (whole in its tooltip),
// marked when it was added or changed since the last save, highlighted while the selection is in
// it (the primary's row, or a row selected with others); `id` its item's id after its text; its
// right-click menu (record_menu, the type's `own` items after the jumps). True when it was clicked: its
// caller selects.
bool row_line(Workspace &workspace, const Document &document, const RecordReveal &reveal, const OutlineLine &line,
              const char *id, OutlineRowMenuHook own, bool middle = false) {
	const SessionView &view = workspace.view();
	ImGui::PushID(static_cast<int>(line.address.row));
	const float x = ImGui::GetCursorScreenPos().x;
	const std::string label = ui_kit::kChangeRoom + line.text;
	// A master column's names cut in their middle, so names that share a start stay apart ("HelpSc...Keys",
	// "HelpSc...Text"; the audit's 5.1).
	const std::string shown = middle ? ui_kit::kChangeRoom + ui_kit::fit_middle(line.text, ImGui::GetContentRegionAvail().x -
	                                                                                           ui_kit::text_width(ui_kit::kChangeRoom))
	                                 : ui_kit::fit(label, ImGui::GetContentRegionAvail().x);
	const bool selected = view.documents.selection.primary.row == line.address.row ||
	                      view.documents.selection.holds(line.address);
	const bool clicked = ImGui::Selectable((shown + id).c_str(), selected);
	reveal.scroll_to(line.address, true);
	const Document::RecordChange change = document.record_change(line.address);
	ui_kit::change_dot(change, x);
	ui_kit::tooltip_lazy([&] {
		const std::string words = ui_kit::change_words(change);
		return shown != label ? line.text + (words.empty() ? "" : "\n" + words) : words;
	});
	record_menu(workspace, document, line.address, own);
	ImGui::PopID();
	return clicked;
}

// A click on the record line `index` of the model's lines selects as the keys held say
// (OutlineModel::click): alone, with Ctrl joining or leaving the selection, with Shift the lines
// from the primary's to it in one selection.
void select_line(Workspace &workspace, const Document &document, const OutlineModel &model, size_t index) {
	const ImGuiIO &io = ImGui::GetIO();
	OutlineClick click = model.click(index, workspace.view().documents.selection.primary, io.KeyCtrl, io.KeyShift);
	if (!click.record.row) return;
	workspace.request(request::select_record(document.path(), click.record, click.mode, std::move(click.records)));
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

// Whether the outline's Duplicate / Remove / Up / Down could act on any record of the file: a kind
// of row the file adds, or a row's list that is not fixed. A clip has neither (its one row, its
// bones and its frame events), so its outline shows none of them. A file of many rows is taken to
// have one, unread.
bool rearranges(const Document &document) {
	for (const RecordKindRow &kind : document.kinds())
		if (*kind.add_label) return true;
	if (document.rows().size() > 64) return true;
	for (const auto &row : document.rows())
		for (const Document::Collection &collection : document.collections_of({row->id, row->kind, 0}))
			if (!collection.spec.fixed) return true;
	return false;
}

// The project's names a document's lines read (the graph's), for a type that words its records with
// them (DocumentType::record_label); none with no project open, and for any other type (its lines
// then never made again for a graph's change).
struct ViewNames {
	std::optional<GraphNameSource> graph;
	ViewNames(const SessionView &view, const Document &document) {
		const DocumentType *type = document_type_for(document.kind());
		if (view.findings.graph && type && type->record_label) graph.emplace(*view.findings.graph);
	}
	const NameSource *get() const { return graph ? &*graph : nullptr; }
};

} // namespace

// The menu over a record's line (outline_view.h).
void record_menu(Workspace &workspace, const Document &document, const NodeAddress &record, OutlineRowMenuHook own) {
	if (!ImGui::BeginPopupContextItem("record menu")) return;
	const SessionView &view = workspace.view();
	if (ImGui::IsWindowAppearing() && !view.documents.selection.holds(record))
		window_requests::select(workspace, document, record);
	std::vector<ReferenceTarget> targets;
	if (view.findings.graph && view.project.scan)
		record_definition(*view.findings.graph, *view.project.scan, document, record, targets);
	if (ImGui::MenuItem("Go to definition", "F12", false, !targets.empty()) && !targets.empty())
		window_requests::go_to(workspace, targets.front());
	ui_kit::tooltip(targets.empty() ? std::string("It names nothing the project defines or holds.")
	                                : window_requests::go_to_words(targets.front()) + " (what it names first)");
	const std::string locator = document.locator(record);
	const bool finds = view.findings.graph && !locator.empty();
	if (ImGui::MenuItem("Find usages", "Shift+F12", false, finds) && finds)
		ProjectFind::open_usages(workspace, document.path(), locator);
	ui_kit::tooltip("Who names what it defines, each a Go to.");
	if (own) {
		ImGui::Separator();
		own(workspace, document, record);
	}
	ImGui::EndPopup();
}

OutlineView::OutlineView(const OutlineSpec &spec)
    : spec_(spec), model_(spec.mode, spec.file_values, spec.row_listed, spec.groups, spec.reads_others) {}

void OutlineView::finding_mark(const SessionView &view, const Document &document, const NodeAddress &address) {
	const std::vector<size_t> found = findings_.of_record(document.path(), address.row, address.child);
	if (found.empty()) return;
	// The worst severity among them, and every message (the field's name before it) in the tooltip.
	DiagnosticSeverity worst = DiagnosticSeverity::Info;
	for (const size_t i : found) {
		const DiagnosticSeverity severity = view.findings.diagnostics[i].severity;
		if (severity == DiagnosticSeverity::Error || (severity == DiagnosticSeverity::Warning && worst == DiagnosticSeverity::Info))
			worst = severity;
	}
	if (!ui_kit::severity_mark_on_item(worst)) return;
	std::string tip;
	for (const size_t i : found) {
		const Diagnostic &d = view.findings.diagnostics[i];
		const NodeKind kind = d.record_kind ? d.record_kind : address.kind;
		tip += (tip.empty() ? "" : "\n") + (d.field.empty() ? d.message : field_title(document, kind, d.field) + ": " + d.message);
	}
	ImGui::SetTooltip("%s", tip.c_str());
}

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

OutlineView::Shown OutlineView::shown() const {
	return Shown{ model_.filter(), model_.kinds(), model_.all_rows(), model_.sort(), model_.every() };
}

void OutlineView::follow_workspace(const SessionView &view, const Document &document) {
	const WorkspaceView::DocumentView &held = view.workspace.document(document.path());
	if (!held_.follow(Shown{ held.filter, held.kinds, held.all_rows, held.sort, held.every })) return;
	model_.set_filter(held.filter);
	model_.set_kinds(held.kinds);
	model_.set_all_rows(held.all_rows);
	model_.set_sort(held.sort);
	model_.set_every(held.every);
}

void OutlineView::send_workspace(Workspace &workspace, const Document &document, const Shown &before) {
	const Shown now = shown();
	if (now == before) return;
	io::JsonValue members = io::JsonValue::make_object();
	members.set("path", io::JsonValue::make_string(document.path()));
	if (now.filter != before.filter) members.set("filter", io::JsonValue::make_string(now.filter));
	if (now.kinds != before.kinds) {
		// By the kinds' tokens, in the document's kinds() order (the model's bits).
		io::JsonValue kinds = io::JsonValue::make_array();
		const std::vector<RecordKindRow> &rows = document.kinds();
		for (size_t i = 0; i < rows.size() && i < 64; ++i)
			if (now.kinds & (uint64_t(1) << i)) kinds.push(io::JsonValue::make_string(rows[i].token));
		members.set("kinds", std::move(kinds));
	}
	if (now.all_rows != before.all_rows) members.set("all_rows", io::JsonValue::make_bool(now.all_rows));
	if (now.sort != before.sort) members.set("sort", io::JsonValue::make_bool(now.sort));
	if (now.every != before.every) members.set("every", io::JsonValue::make_bool(now.every));
	window_requests::set_workspace(workspace, "document", std::move(members));
}

void OutlineView::draw(Workspace &workspace, const DocumentBase &base) {
	// A RevealRecord taken: the selection shown again, where it was already too.
	for (const ViewEvent &event : take_events()) {
		(void)event;
		reveal_.ask();
	}
	const Document *document = records_of(base);
	if (!document) return ui_kit::empty_state("This file holds no records to list.");
	follow_workspace(workspace.view(), *document);
	const Shown before = shown();
	reveal_.follow(workspace.view(), *document);
	draw_document_toolbar(workspace, *document);
	switch (spec_.mode) {
	case OutlineMode::List: draw_list(workspace, *document); break;
	case OutlineMode::Tree: draw_tree(workspace, *document); break;
	case OutlineMode::MasterDetail: draw_master_detail(workspace, *document); break;
	}
	send_workspace(workspace, *document, before);
}

// The kinds of row the outline lists (OutlineSpec::by_kind): a chip per kind of row the file holds,
// pressed while its kind's rows are listed, a click listing them or leaving them out; then the
// switch listing the rows the type leaves out (OutlineSpec::row_listed, its words the spec's).
void OutlineView::draw_kinds(const Document &document) {
	if (!spec_.by_kind && !spec_.row_listed) return;
	ui_kit::WrapRow row;
	if (spec_.by_kind) {
		uint64_t kinds = model_.kinds();
		ImGui::PushID("kinds");
		for (const RecordKindRow &kind : document.kinds()) {
			if (!kind.top) continue;
			const uint64_t bit = OutlineModel::kind_bit(document, kind.kind);
			const bool listed = (kinds & bit) != 0;
			row.next(ui_kit::button_width(kind.label));
			// A chip left out draws as a button does at rest, dimmed; one listed as a button pressed.
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(listed ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
			if (!listed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			if (ImGui::SmallButton(kind.label)) kinds ^= bit;
			ImGui::PopStyleColor(listed ? 1 : 2);
			ui_kit::tooltip_lazy([&] {
				return std::string(listed ? "Listed: " : "Left out: ") + strutil::to_lower(kind.label) +
				       " records. Click to " + (listed ? "leave them out." : "list them.");
			});
		}
		ImGui::PopID();
		model_.set_kinds(kinds);
	}
	if (spec_.row_listed) {
		row.next(ui_kit::checkbox_width(spec_.unlisted));
		bool all = model_.all_rows();
		if (ImGui::Checkbox(spec_.unlisted, &all)) model_.set_all_rows(all);
		ui_kit::tooltip("Lists the ones that hold nothing too.");
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
	draw_kinds(document);
	// The rows by the project's names where the type words them so (a weapon by its WepDes text, the
	// plain-words lane).
	const ViewNames names(view, document);
	// The selection moved there: its row shown (a filter hiding it cleared) and scrolled to, however
	// far down.
	const size_t revealed = reveal_.moved() ? model_.reveal(document, reveal_.path(), 0, names.get()) : SIZE_MAX;
	ImGui::BeginDisabled(document.blocked());
	const auto &rows = document.rows();
	size_t index = SIZE_MAX; // the selected record's row, its place in the file
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == view.documents.selection.primary.row) index = i;
	{
		// Each kind's Add, then the selected record's tools, on a row that wraps.
		ui_kit::WrapRow row;
		for (const RecordKindRow &kind : document.kinds())
			if (*kind.add_label && ui_kit::tool(row, kind.add_label, true, "Adds one at the end of the file."))
				edit(workspace, document, EditOperation::Add, {0, kind.kind, 0});
		// The type's own Add menu (an items.def's rows the engine looks for by their ids).
		if (spec_.list_menu && spec_.list_menu_offers && spec_.list_menu_offers(document)) {
			if (ui_kit::tool(row, spec_.list_menu_label, true, spec_.list_menu_tip)) ImGui::OpenPopup("list_menu");
			if (ImGui::BeginPopup("list_menu")) {
				spec_.list_menu(workspace, document);
				ImGui::EndPopup();
			}
		}
		ui_kit::RowTools tools;
		tools.add = nullptr;
		tools.count = rows.size();
		tools.selected = index;
		const NodeAddress address = index < rows.size() ? NodeAddress{rows[index]->id, rows[index]->kind, 0} : NodeAddress();
		row_tool(workspace, document, ui_kit::row_tools(row, tools), address, index);
	}
	const std::vector<OutlineLine> &lines = model_.lines(document, 0, names.get());
	if (rows.empty()) ui_kit::empty_state("The file has no records yet.", "Add one with the buttons above.");
	else if (lines.empty()) ui_kit::empty_state("No record matches the filter.");
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(lines.size()));
	if (revealed != SIZE_MAX) clipper.IncludeItemByIndex(static_cast<int>(revealed));
	while (clipper.Step())
		for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
			if (row_line(workspace, document, reveal_, lines[size_t(i)], "###record", spec_.row_menu))
				select_line(workspace, document, model_, size_t(i));
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
	draw_kinds(document);
	const ViewNames names(workspace.view(), document);
	findings_.follow(workspace.view());
	// The selection moved there: the records, collections and headings holding it open (a filter
	// hiding it cleared), its line scrolled to, however far down.
	const size_t revealed = reveal_.moved() ? model_.reveal(document, reveal_.path(), 0, names.get()) : SIZE_MAX;
	ImGui::BeginDisabled(document.blocked());
	draw_tree_tools(workspace, document);
	ImGui::Separator();
	if (ImGui::BeginChild("outline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
		const std::vector<OutlineLine> &lines = model_.lines(document, 0, names.get());
		if (document.rows().empty()) ui_kit::empty_state("The file holds no records.");
		else if (lines.empty()) ui_kit::empty_state("No record matches the filter.");
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(lines.size()));
		if (revealed != SIZE_MAX) clipper.IncludeItemByIndex(static_cast<int>(revealed));
		while (clipper.Step())
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				draw_tree_line(workspace, document, lines[size_t(i)], size_t(i));
	}
	ImGui::EndChild();
	ImGui::EndDisabled();
}

// One line of the tree at its depth, `index` its place among the model's lines: a record (a node
// over its collections, or a leaf; marked when it was added or changed since the last save; selected
// on a click, Ctrl joining or leaving the selection, Shift selecting the lines from the primary's to
// it) or a collection (a node over its records, + adding one at its end where it takes
// one). What is open is the model's: the arrow opens or closes the line there (a line the filter
// holds open stays so).
void OutlineView::draw_tree_line(Workspace &workspace, const Document &document, const OutlineLine &line,
		size_t index) {
	const SessionView &view = workspace.view();
	const float indent = ImGui::GetStyle().IndentSpacing * float(line.depth);
	if (indent > 0.0f) ImGui::Indent(indent);
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (line.branch) ImGui::SetNextItemOpen(line.open);
	else flags |= ImGuiTreeNodeFlags_Leaf;
	if (line.heading) {
		// A heading: its words and how many rows stand under it, opened and closed as a record is; a line
		// as high as a record's (the clipper places every line at one height: a framed node, taller,
		// would put the lines after it out of place), a band drawn behind it.
		ImGui::PushID(line.key.c_str());
		const ImVec2 at = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddRectFilled(
		        at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + ImGui::GetTextLineHeight()),
		        ImGui::GetColorU32(ImGuiCol_Header, 0.55f));
		ImGui::TreeNodeEx("##heading", flags, "%s", line.text.c_str());
		if (ImGui::IsItemToggledOpen()) model_.set_open(line, !line.open);
		ui_kit::tooltip(line.forced ? "Opened while the filter keeps a record under it." : line.open ? "Click to close." : "Click to open.");
		ImGui::PopID();
	} else if (line.collection) {
		ImGui::PushID(document.kind_token(line.kind));
		ImGui::PushID(static_cast<int>(line.address.child ? line.address.child : line.address.row));
		ImGui::TreeNodeEx("##collection", flags | ImGuiTreeNodeFlags_AllowOverlap, "%s", line.text.c_str());
		if (line.branch && ImGui::IsItemToggledOpen()) model_.set_open(line, !line.open);
		// Its "+": a blank record at the end, or where the type adds its records by type (a mission's
		// triggers and actions, S15) a popup of the types by name; off, saying so, while the list holds
		// the most it holds.
		const bool by_menu = spec_.adds_by_menu && spec_.add_menu && spec_.adds_by_menu(line.kind);
		if (line.addable || line.full) {
			ImGui::SameLine();
			ImGui::BeginDisabled(!line.addable);
			if (ImGui::SmallButton("+")) {
				if (by_menu) ImGui::OpenPopup("add");
				else edit(workspace, document, EditOperation::Add, {line.address.row, line.kind, 0}, SIZE_MAX, line.address.child);
			}
			ImGui::EndDisabled();
			ui_kit::tooltip(!line.addable ? "It holds " + std::to_string(line.full) + ", the most it holds: remove one first."
			                : by_menu     ? std::string("Adds one at the end: pick its type by name.")
			                              : std::string("Adds one at the end."));
			if (by_menu && ImGui::BeginPopup("add")) {
				spec_.add_menu(workspace, document, line.address, line.kind);
				ImGui::EndPopup();
			}
		}
		ImGui::PopID();
		ImGui::PopID();
	} else {
		flags |= ImGuiTreeNodeFlags_OpenOnArrow;
		if (view.documents.selection.holds(line.address)) flags |= ImGuiTreeNodeFlags_Selected;
		const float x = ImGui::GetCursorScreenPos().x;
		// Cut to the column (a mission's event is its whole sentence, S15), room left at its end for its
		// findings' mark: the whole in its tooltip.
		const float room = std::max(ImGui::GetContentRegionAvail().x - ImGui::GetTreeNodeToLabelSpacing() -
		                                    ui_kit::text_width(ui_kit::kChangeRoom) - ImGui::GetTextLineHeight(),
		                            ImGui::GetFontSize() * 3.0f);
		// A title the column cannot hold gives way to its brief words (what tells the record apart first:
		// an event's first trigger's subject and verb, an entity's SSN), cut to the column in turn.
		const bool fits = ui_kit::text_width(line.text.c_str()) <= room;
		const std::string label = ui_kit::kChangeRoom + ui_kit::fit(fits || line.brief.empty() ? line.text : line.brief, room);
		const NodeId id = line.address.child ? line.address.child : line.address.row;
		ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<uintptr_t>(id)), flags, "%s", label.c_str());
		const bool toggled = ImGui::IsItemToggledOpen();
		if (line.branch && toggled) model_.set_open(line, !line.open);
		reveal_.scroll_to(line.address);
		const Document::RecordChange change = document.record_change(line.address);
		ui_kit::change_dot(change, x + ImGui::GetTreeNodeToLabelSpacing());
		// A record naming a texture (a model's texture row) shows it as its tooltip (S18).
		if (!(ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) &&
		      texture_preview::record_tooltip(workspace, document, line.address, record_tip(line, change))))
			ui_kit::tooltip_lazy([&] { return record_tip(line, change); });
		if (ImGui::IsItemClicked() && !toggled) select_line(workspace, document, model_, index);
		// A double click on a mission's entity or area frames it in the mission's picture (ADR 0046
		// S15: the outline and the picture are one selection; its viewport plans the camera).
		const bool placed = is_entity_kind(line.address.kind) || line.address.kind == node_kind(MissionKind::Area);
		if (!toggled && !line.address.child && placed && document.kind() == AssetKind::Mission && ImGui::IsItemHovered() &&
				ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			ViewportCommand frame;
			frame.name = "frame";
			frame.kind = ViewportKind::Mission;
			frame.ids = { line.address.row };
			workspace.request(request::edit_in_viewport(document.path(), std::move(frame)));
		}
		finding_mark(view, document, line.address);
		// Its right-click menu (DI-18), over its line (finding_mark draws no item), in its own id.
		ImGui::PushID(static_cast<int>(id));
		record_menu(workspace, document, line.address, spec_.row_menu);
		ImGui::PopID();
	}
	if (indent > 0.0f) ImGui::Unindent(indent);
}

// The rows the file adds (one tool per kind with an add_label: a row at the end of the file), the
// selected record's name and its tools, Duplicate / Remove / Up / Down, where its list allows
// them: a nested record's collection, or the rows of a kind the file adds (a collection's + adds a
// nested one).
void OutlineView::draw_tree_tools(Workspace &workspace, const Document &document) {
	const NodeAddress selection = workspace.view().documents.selection.primary;
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
	// What cannot apply is not shown: the selected record's tools where it stays as it is (its
	// tooltip on its name says why), and with none selected where nothing in the file moves.
	if (tools.locked || (!placed && !rearranges(document)))
		tools.duplicate = tools.remove = tools.up = tools.down = nullptr;
	ui_kit::WrapRow row;
	const float line = ImGui::GetContentRegionAvail().x; // the row's whole line (a narrow column's)
	for (const RecordKindRow &kind : document.kinds())
		if (*kind.add_label && ui_kit::tool(row, kind.add_label, true, "Adds one at the end of the file.", true))
			edit(workspace, document, EditOperation::Add, {0, kind.kind, 0});
	if (placed) {
		const ViewNames names(workspace.view(), document);
		const std::string title = record_display(document, selection, names.get()), name = document.record_name(selection);
		const std::string shown = ui_kit::fit(title, std::min(ImGui::GetFontSize() * 12.0f, line));
		row.next(ui_kit::text_width(shown.c_str()));
		ImGui::TextUnformatted(shown.c_str());
		std::string tip = shown != title ? title : std::string();
		if (name != title) tip += (tip.empty() ? "" : "\n") + name;
		if (tools.locked) tip += (tip.empty() ? "" : "\n") + std::string(tools.locked);
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
	const std::string row_words = rows_kind ? strutil::to_lower(rows_kind->label) : std::string("row");
	const std::string detail_words = strutil::to_lower(model_.detail_label());
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
	const Node *master = document.row(view.documents.selection.primary.row);
	const NodeId master_id = master ? master->id : 0;
	// The selection moved to a record: its line shown (a filter hiding it cleared) and scrolled to.
	const size_t revealed = reveal_.moved() ? model_.reveal(document, reveal_.path(), master_id) : SIZE_MAX;
	model_.lines(document, master_id);
	// The rows' column as wide as its widest name (two fifths of the room at most), its tools' widest at
	// least (the audit's 5.1: sections cut to "HelpScreen...").
	const float masters = std::max(std::min(masters_width(document), ImGui::GetContentRegionAvail().x * 0.4f),
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
			ui_kit::empty_state(("Select one of the " + strutil::to_lower(spec_.rows) + " to list what it holds.").c_str());
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
		if (rows[i]->id == view.documents.selection.primary.row) index = i;
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
	const RecordKindRow *adds = own_kind(document);
	if (tool == ui_kit::RowTool::Add && adds) {
		// A row named as it is added (a string table's section): its name asked first.
		if (*spec_.row_name_field) {
			add_name_[0] = '\0';
			ImGui::OpenPopup("add named");
		} else {
			edit(workspace, document, EditOperation::Add, {0, adds->kind, 0});
		}
	}
	if (adds && ImGui::BeginPopup("add named")) {
		const std::string words = strutil::to_lower(adds->label);
		ImGui::TextUnformatted(("The new " + words + "'s name:").c_str());
		if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
		const bool entered = ImGui::InputText("##name", add_name_, sizeof(add_name_), ImGuiInputTextFlags_EnterReturnsTrue);
		// One of the name already (in any case, as the game's lookup finds a name): Add selects it.
		const Node *held = nullptr;
		for (const auto &each : rows)
			if (!held && add_name_[0] && strutil::iequals(retail_text_to_utf8(each->name()), add_name_)) held = each.get();
		if (held)
			ImGui::TextDisabled("%s", (adds->label + std::string(" ") + retail_text_to_utf8(held->name()) +
			                           " is already in the file: Add selects it.").c_str());
		ImGui::BeginDisabled(!add_name_[0]);
		const bool add = ImGui::Button("Add") || (entered && add_name_[0]);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
		if (add) {
			window_requests::add_with(workspace, document, {0, adds->kind, 0}, 0, spec_.row_name_field, std::string(add_name_));
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	row_tool(workspace, document, tool, address, index);
	if (rows.empty()) ui_kit::empty_state(("No " + strutil::to_lower(spec_.rows) + " yet.").c_str());
	for (const OutlineLine &line : model_.masters())
		if (row_line(workspace, document, reveal_, line, "###row", spec_.row_menu, true)) select(workspace, document, line.address);
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
			if (view.documents.selection.primary.row == master->id && view.documents.selection.primary.child == ids[i]) at = i;
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
		// The selected record to another row (a string to another section): a Move whose destination is
		// that row, which the type makes an add there and a remove here, one undo step.
		if (spec_.details_move_between_rows) {
			const RecordKindRow *rows_kind = own_kind(document);
			const std::string to = strutil::to_lower(rows_kind ? rows_kind->label : "row");
			const std::string label = "Move to " + to + "...";
			const bool can = at < ids.size() && document.rows().size() > 1;
			if (ui_kit::tool(row, label.c_str(), can,
			                 at >= ids.size() ? std::string(tools.pick)
			                 : can           ? "Moves it to the end of another " + to + "'s list (one undo step)."
			                                 : "The file has no other " + to + ".",
			                 true)) {
				move_filter_[0] = '\0';
				ImGui::OpenPopup("move to row");
			}
			if (ImGui::BeginPopup("move to row")) {
				ui_kit::filter_box("##rows", move_filter_, sizeof(move_filter_), "Filter", ImGui::GetFontSize() * 16.0f);
				NodeId chosen = 0;
				for (const auto &each : document.rows()) {
					if (each->id == master->id) continue;
					const std::string name = retail_text_to_utf8(each->name());
					if (move_filter_[0] && !window_requests::matches(name, move_filter_)) continue;
					ImGui::PushID(static_cast<int>(each->id));
					if (ImGui::Selectable(name.empty() ? "(no name)" : name.c_str())) chosen = each->id;
					ImGui::PopID();
				}
				if (chosen) {
					edit(workspace, document, EditOperation::Move, address, SIZE_MAX, chosen);
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}
		ImGui::PopID();
		if (!every && ids.empty()) return ui_kit::empty_state("Nothing in it yet.");
	}
	const std::vector<OutlineLine> &lines = model_.lines(document, master ? master->id : 0);
	if (lines.empty())
		return ui_kit::empty_state(every ? "Nothing in any of them matches the filter." : "Nothing matches the filter.");
	const std::vector<const FieldSchema *> &columns = model_.columns();
	// The column of the name a record is found by (a string's key) as wide as its widest value, up to a
	// third of the table, and a Uses column after the others where the project's graph counts what names
	// each record (the plain-words lane, the audit's 5.1 and 5.2).
	const FieldSchema *defining = model_.defining_column();
	const AssetGraph *graph = view.findings.graph.get();
	const bool uses = defining && graph;
	const int count = (every ? 2 : 1) + static_cast<int>(columns.size()) + (uses ? 1 : 0);
	const float key_room = defining ? defining_width(document, lines, *defining) : 0.0f;
	if (!ImGui::BeginTable("records", count, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("##changed", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width(ui_kit::kChangeRoom));
	const RecordKindRow *rows_kind = own_kind(document);
	if (every) ImGui::TableSetupColumn(rows_kind ? rows_kind->label : "Row", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	const float table_width = ImGui::GetContentRegionAvail().x;
	for (const FieldSchema *field : columns) {
		if (field == defining)
			ImGui::TableSetupColumn(field_widgets::column_header(*field).c_str(), ImGuiTableColumnFlags_WidthFixed,
			                        std::min(key_room, table_width / 3.0f));
		else
			ImGui::TableSetupColumn(field_widgets::column_header(*field).c_str(), ImGuiTableColumnFlags_WidthStretch,
			                        field->multiline ? 3.0f : 1.0f);
	}
	// As narrow as its header: the text keeps the room a narrow table has.
	if (uses) ImGui::TableSetupColumn("Uses", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width("Uses"));
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
		const bool on = view.documents.selection.primary.row == line.address.row && view.documents.selection.primary.child == line.address.child;
		if (on) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderHovered, 0.35f));
		ImGui::TableNextColumn();
		const float x = ImGui::GetCursorScreenPos().x;
		if (ImGui::Selectable("##pick", on, ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetFrameHeight())))
			select(workspace, document, line.address);
		reveal_.scroll_to(line.address);
		const Document::RecordChange change = document.record_change(line.address);
		ui_kit::change_dot(change, x);
		ui_kit::tooltip(*ui_kit::change_words(change) ? ui_kit::change_words(change) : "Select this one.");
		record_menu(workspace, document, line.address, spec_.row_menu);
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
		if (uses) {
			// How many references of the project's files name it, each listed in the tooltip.
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			const size_t count = model_.uses(*graph, document, line.address);
			// A click opens its uses (DI-05): the record selected, its Referenced by in the Inspector, each a Go to.
			if (count) {
				if (ImGui::Selectable((std::to_string(count) + "###uses").c_str())) {
					select(workspace, document, line.address);
					window_requests::focus(workspace, "inspector");
				}
			} else {
				ImGui::TextDisabled("0");
			}
			ui_kit::tooltip_lazy([&] {
				const GraphSymbol *symbol = graph->symbol_at(document.path(), document.locator(line.address), defining->id);
				if (!symbol || !count) return std::string("No file of the project names it, nor does the game's code.");
				const std::vector<const CodeTextKey *> code = code_reads_of(*graph, *symbol);
				std::string tip = counted(count, "use") + " (a click lists the project's in the Inspector, each a Go to):";
				size_t listed = 0;
				for (const GraphEdge *edge : graph->users_of(*symbol)) {
					if (++listed > 12) {
						tip += "\n...";
						break;
					}
					const AssetEntry *source = view.project.scan->at_path(edge->source);
					const std::string place = edge_place_words(*edge, source ? source->kind : AssetKind::Unknown);
					tip += "\n" + (source ? source->logical_name : edge->source) + (place.empty() ? std::string() : ": " + place);
				}
				// The game's own code, which reads the string by its name: a rename or a removal changes what it shows.
				if (!code.empty())
					tip += "\nThe game itself, by name, in " + counted(code.size(), "place") + " (" + code.front()->reader +
					       (code.size() > 1 ? " and others)" : ")");
				return tip;
			});
		}
		ImGui::PopID();
	}
	ImGui::EndTable();
	editing_ = editing;
}

float OutlineView::defining_width(const Document &document, const std::vector<OutlineLine> &lines, const FieldSchema &field) {
	if (defining_.document == document.identity() && defining_.revision == document.revision() && defining_.lines == lines.size() &&
	    defining_.first == (lines.empty() ? nullptr : &lines.front()))
		return defining_.width;
	float widest = ui_kit::text_width(field_widgets::column_header(field).c_str());
	for (const OutlineLine &line : lines) {
		Value value;
		if (!document.get(line.address, field.id, value)) continue;
		if (const auto *text = std::get_if<std::string>(&value)) widest = std::max(widest, ui_kit::text_width(text->c_str()));
	}
	defining_ = {widest + ImGui::GetStyle().FramePadding.x * 4.0f + ImGui::GetStyle().CellPadding.x * 2.0f, document.identity(),
	             document.revision(), lines.size(), lines.empty() ? nullptr : &lines.front()};
	return defining_.width;
}

float OutlineView::masters_width(const Document &document) {
	const std::vector<OutlineLine> &masters = model_.masters();
	if (masters_.document == document.identity() && masters_.revision == document.revision() && masters_.lines == masters.size())
		return masters_.width;
	float widest = 0.0f;
	for (const OutlineLine &line : masters) widest = std::max(widest, ui_kit::text_width(line.text.c_str()));
	masters_ = {widest + ui_kit::text_width(ui_kit::kChangeRoom) + ImGui::GetStyle().FramePadding.x * 2.0f +
	                    ImGui::GetStyle().CellPadding.x * 2.0f + ImGui::GetStyle().ScrollbarSize,
	            document.identity(), document.revision(), masters.size(), nullptr};
	return masters_.width;
}

} // namespace opennova::editor
