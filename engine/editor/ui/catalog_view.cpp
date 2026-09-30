#include "catalog_view.h"

#include <editor/documents/def_catalog_document.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <imgui.h>

namespace opennova::editor {
namespace {

using window_requests::edit;
using window_requests::matches;
using window_requests::select;

// A file-wide value of items.def's spawn registry (a slot's item id, or a new slot).
void set_spawn(Workspace &workspace, const DefCatalogDocument &document, size_t slot, int64_t id) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = EditOperation::SetFileValue;
	request.edit.position = slot;
	request.edit.value = id;
	workspace.request(std::move(request));
}

} // namespace

void CatalogView::draw(Workspace &workspace, const DefCatalogDocument &document) {
	const SessionView &view = workspace.view();
	reveal_.follow(view, document);
	draw_document_toolbar(workspace, document);
	{
		ui_kit::WrapRow row;
		const float filter = ImGui::GetFontSize() * 14.0f;
		row.next(filter);
		ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter records", filter, nullptr, false);
		row.next(ui_kit::checkbox_width("Sort by name"));
		ImGui::Checkbox("Sort by name", &sort_names_);
		ui_kit::tooltip("Lists the records by name; Up and Down still move them in the file's order.");
	}
	ImGui::BeginDisabled(document.blocked());
	const auto &rows = document.rows();
	size_t index = SIZE_MAX; // the selected record's place in the file
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i]->id == view.documents.selection.row) index = i;
	{
		// Each kind's Add, then the selected record's tools, on a row that wraps.
		ui_kit::WrapRow row;
		for (const RecordKindRow &kind : document.kinds())
			if (*kind.add_label &&
			    ui_kit::tool(row, kind.add_label, true, "Adds one at the end of the file."))
				edit(workspace, document, EditOperation::Add, {0, kind.kind, 0});
		ui_kit::RowTools tools;
		tools.add = nullptr;
		tools.count = rows.size();
		tools.selected = index;
		const NodeAddress address = index < rows.size() ? NodeAddress{rows[index]->id, rows[index]->kind, 0} : NodeAddress();
		switch (ui_kit::row_tools(row, tools)) {
		case ui_kit::RowTool::Duplicate: edit(workspace, document, EditOperation::Duplicate, address, index + 1); break;
		case ui_kit::RowTool::Remove: edit(workspace, document, EditOperation::Remove, address); break;
		case ui_kit::RowTool::Up: edit(workspace, document, EditOperation::Move, address, index - 1); break;
		case ui_kit::RowTool::Down: edit(workspace, document, EditOperation::Move, address, index + 1); break;
		case ui_kit::RowTool::Add:
		case ui_kit::RowTool::None: break;
		}
	}
	std::vector<std::shared_ptr<const Node>> visible;
	for (const auto &row : rows) if (matches(row->name(), filter_)) visible.push_back(row);
	if (sort_names_)
		std::stable_sort(visible.begin(), visible.end(), [](const auto &a, const auto &b) {
			return normalized_logical_name(a->name()) < normalized_logical_name(b->name());
		});
	if (rows.empty()) ui_kit::empty_state("The file has no records yet.", "Add one with the buttons above.");
	else if (visible.empty()) ui_kit::empty_state("No record matches the filter.");
	// Each record, marked when it was added or changed since the last save, its name cut to
	// the window (whole in its tooltip).
	for (const auto &row : visible) {
		ImGui::PushID(int(row->id));
		const bool top = def_kind(row->kind) == document.record_kind();
		const std::string name = top ? row->name() : std::string(document.kind_label(row->kind)) + ": " + row->name();
		const NodeAddress address{row->id, row->kind, 0};
		const float x = ImGui::GetCursorScreenPos().x;
		const std::string shown = ui_kit::fit(ui_kit::kChangeRoom + name, ImGui::GetContentRegionAvail().x);
		if (ImGui::Selectable((shown + "###record").c_str(), view.documents.selection.row == row->id)) select(workspace, document, address);
		reveal_.scroll_to(address, true);
		const Document::RecordChange change = document.record_change(address);
		ui_kit::change_dot(change, x);
		const std::string words = ui_kit::change_words(change);
		ui_kit::tooltip(shown != ui_kit::kChangeRoom + name ? name + (words.empty() ? "" : "\n" + words) : words);
		ImGui::PopID();
	}
	ImGui::EndDisabled();
	// items.def's vehicle spawn registry, marked while it differs from the saved file.
	if (document.kind() != AssetKind::ItemDefs) return;
	const float x = ImGui::GetCursorScreenPos().x;
	const Document::RecordChange registry =
	        document.file_state_changed() ? Document::RecordChange::Changed : Document::RecordChange::Unchanged;
	const bool open = ImGui::TreeNode("spawn", "%sVehicle spawn IDs", ui_kit::kChangeRoom);
	ui_kit::change_dot(registry, x + ImGui::GetTreeNodeToLabelSpacing());
	ui_kit::tooltip(ui_kit::change_words(registry));
	if (!open) return;
	ImGui::BeginDisabled(document.blocked());
	const std::vector<int> &spawn = document.spawn_ids();
	for (size_t i = 0; i < spawn.size(); ++i) {
		int id = spawn[i];
		ImGui::PushID(int(i));
		const std::string label = "Slot " + std::to_string(i + 1);
		const float room = ImGui::GetContentRegionAvail().x - ui_kit::field_width(0.0f, label.c_str());
		ImGui::SetNextItemWidth(std::clamp(room, ImGui::GetFontSize() * 4.0f, ImGui::GetFontSize() * 10.0f));
		if (ImGui::InputInt(label.c_str(), &id)) set_spawn(workspace, document, i, id);
		ui_kit::tooltip("The item id the game spawns in this vehicle slot.");
		ImGui::PopID();
	}
	ui_kit::WrapRow row;
	const bool room = spawn.size() < size_t(def::DEF_VEHICLE_SPAWN_SLOTS);
	if (ui_kit::tool(row, "Add spawn slot", room,
	                 room ? std::string("Adds a slot at the end (item id 0).")
	                      : "The registry holds " + std::to_string(def::DEF_VEHICLE_SPAWN_SLOTS) + " slots at most."))
		set_spawn(workspace, document, spawn.size(), 0);
	ImGui::EndDisabled();
	ImGui::TreePop();
}

} // namespace opennova::editor
