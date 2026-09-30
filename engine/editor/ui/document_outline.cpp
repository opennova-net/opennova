#include "document_outline.h"

#include <algorithm>
#include <string>

#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <imgui.h>

namespace opennova::editor {
namespace {

using window_requests::edit;
using window_requests::select;

void draw_collection(Workspace &workspace, const Document &document, const RecordReveal &reveal, NodeId row,
                     const NodeAddress &owner, const Document::Collection &collection);

// A record's name as the windows show it (Document::record_title) and its tooltip: the token
// its type words ("anim_walk_forward" under "walk forward"), then whether it changed since
// the last save.
std::string record_tip(const Document &document, const NodeAddress &address, const std::string &title,
                       Document::RecordChange change) {
	const std::string name = document.record_name(address);
	const std::string words = ui_kit::change_words(change);
	std::string tip = name != title ? name : std::string();
	if (!words.empty()) tip += (tip.empty() ? "" : "\n") + words;
	return tip;
}

// One record: a leaf, or a node over the collections it holds; marked when it was added or
// changed since the last save; open while it holds the selection the outline reveals.
void draw_record(Workspace &workspace, const Document &document, const RecordReveal &reveal, const NodeAddress &address) {
	const SessionView &view = workspace.view();
	const std::vector<Document::Collection> held = document.collections_of(address);
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
	if (held.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (view.documents.selection.holds(address)) flags |= ImGuiTreeNodeFlags_Selected;
	const std::string title = document.record_title(address);
	const std::string label = ui_kit::kChangeRoom + title;
	const float x = ImGui::GetCursorScreenPos().x;
	reveal.open_record(address);
	const bool open = ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<uintptr_t>(address.child ? address.child : address.row)),
	                                    flags, "%s", label.c_str());
	reveal.scroll_to(address);
	const Document::RecordChange change = document.record_change(address);
	ui_kit::change_dot(change, x + ImGui::GetTreeNodeToLabelSpacing());
	ui_kit::tooltip_lazy([&] { return record_tip(document, address, title, change); });
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
		select(workspace, document, address, ImGui::GetIO().KeyCtrl ? SelectMode::Toggle : SelectMode::Replace);
	if (!open || held.empty()) return;
	for (const Document::Collection &collection : held)
		draw_collection(workspace, document, reveal, address.row, address, collection);
	ImGui::TreePop();
}

void draw_collection(Workspace &workspace, const Document &document, const RecordReveal &reveal, NodeId row,
                     const NodeAddress &owner, const Document::Collection &collection) {
	const Document::CollectionSpec &spec = collection.spec;
	const std::string label = std::string(spec.label) + " (" + std::to_string(collection.ids.size()) + ")";
	ImGui::PushID(document.kind_token(spec.kind));
	ImGui::PushID(static_cast<int>(owner.child ? owner.child : owner.row));
	reveal.open_collection(owner, spec.kind);
	const bool open = ImGui::TreeNodeEx("##collection", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
	                                    "%s", label.c_str());
	if (!spec.fixed && (spec.max == 0 || collection.ids.size() < spec.max)) {
		ImGui::SameLine();
		if (ImGui::SmallButton("+"))
			edit(workspace, document, EditOperation::Add, {row, spec.kind, 0}, SIZE_MAX, owner.child);
		ui_kit::tooltip("Adds one at the end.");
	}
	if (open) {
		const auto address_at = [&](size_t i) { return NodeAddress{row, spec.kind, collection.ids[i]}; };
		// A collection of leaves may run to thousands (a model's bullet faces): only what
		// shows is drawn, and the record the outline reveals.
		const bool leaves = collection.ids.empty() || document.collections_of(address_at(0)).empty();
		if (leaves && collection.ids.size() > 64) {
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(collection.ids.size()));
			if (const int revealed = reveal.index_in(collection.ids); revealed >= 0) clipper.IncludeItemByIndex(revealed);
			while (clipper.Step())
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
					draw_record(workspace, document, reveal, address_at(size_t(i)));
		} else {
			for (size_t i = 0; i < collection.ids.size(); ++i) draw_record(workspace, document, reveal, address_at(i));
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
	ImGui::PopID();
}

// Whether the file adds rows of `kind` at the top level (RecordKindRow::add_label): those rows
// are a list like any collection that is not fixed; the others are the file's fixed records.
bool adds_rows_of(const Document &document, NodeKind kind) {
	const RecordKindRow *row = document.kind_row(kind);
	return row && *row->add_label;
}

// The rows the file adds (one tool per kind with an add_label: a row at the end of the file), the
// selected record's name and its tools, Duplicate / Remove / Up / Down, where its list allows
// them: a nested record's collection, or the rows of a kind the file adds (a collection's +
// adds a nested one).
void draw_selection_tools(Workspace &workspace, const Document &document) {
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
	ui_kit::WrapRow row;
	for (const RecordKindRow &kind : document.kinds())
		if (*kind.add_label &&
		    ui_kit::tool(row, kind.add_label, true, "Adds one at the end of the file.", true))
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
	switch (ui_kit::row_tools(row, tools)) {
	case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, selection, at.index + 1); break;
	case ui_kit::RowTool::Remove: edit(workspace, document, EditOperation::Remove, selection); break;
	case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, selection, at.index - 1); break;
	case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, selection, at.index + 1); break;
	case ui_kit::RowTool::Add:
	case ui_kit::RowTool::None: break;
	}
}

} // namespace

void draw_document_outline(Workspace &workspace, const Document &document, RecordReveal &reveal) {
	reveal.follow(workspace.view(), document);
	ImGui::BeginDisabled(document.blocked());
	draw_selection_tools(workspace, document);
	ImGui::Separator();
	// A deep tree scrolls sideways rather than run past the window.
	if (ImGui::BeginChild("outline", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
		if (document.rows().empty()) ui_kit::empty_state("The file holds no records.");
		for (const auto &row : document.rows()) {
			if (!row) continue;
			ImGui::PushID(static_cast<int>(row->id));
			draw_record(workspace, document, reveal, {row->id, row->kind, 0});
			ImGui::PopID();
		}
	}
	ImGui::EndChild();
	ImGui::EndDisabled();
}

} // namespace opennova::editor
