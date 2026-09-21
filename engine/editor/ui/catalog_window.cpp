#include "catalog_window.h"

#include <algorithm>
#include <cstring>
#include <imgui.h>

namespace opennova::editor {
using namespace def;
namespace {
const EditableDocument *active(const SessionView &view) {
	for (const auto &document : view.documents) if (document->path() == view.active_document) return document.get();
	return nullptr;
}
void select(EditorHost &host, const EditableDocument &document, CatalogAddress address) {
	auto request = make_request(EditorRequestKind::SelectRecord, document.path());
	request.catalog_edit.address = address; host.request(std::move(request));
}
void edit(EditorHost &host, const EditableDocument &document, CatalogOperation operation,
	CatalogAddress address, size_t position = SIZE_MAX) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.catalog_edit.operation = operation; request.catalog_edit.address = address;
	request.catalog_edit.position = position; host.request(std::move(request));
}
void set(EditorHost &host, const EditableDocument &document, CatalogAddress address,
	const std::string &field, DefValue value, bool coalesce = true) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.catalog_edit.address = address; request.catalog_edit.field = field;
	request.catalog_edit.value = std::move(value); request.catalog_edit.coalesce = coalesce;
	host.request(std::move(request));
}
bool matches(const std::string &name, const char *filter) {
	return normalized_logical_name(name).find(normalized_logical_name(filter)) != std::string::npos;
}
void collection(EditorHost &host, const EditableDocument &document, const CatalogRow &row,
	DefRecordKind kind, const char *label, const std::vector<CatalogId> &ids) {
	if (!ImGui::TreeNode(label)) return;
	ImGui::PushID(label);
	if (ImGui::Button("Add")) edit(host, document, CatalogOperation::Add, {row.id, kind, 0});
	for (size_t i = 0; i < ids.size(); ++i) {
		ImGui::PushID(int(ids[i]));
		const CatalogAddress address{row.id, kind, ids[i]};
		std::string name = std::string(label) + " " + std::to_string(i + 1);
		const char *key = kind == DefRecordKind::Action ? "name" : kind == DefRecordKind::Sight ? "texture" :
			kind == DefRecordKind::Attachment ? "userpoint" : "surface_type";
		if (const void *record = document.record(address))
			name += ": " + std::get<std::string>(def_get(record, *def_field(kind, key)));
		if (ImGui::Selectable(name.c_str(), host.view().selection.child == ids[i])) select(host, document, address);
		if (ImGui::SmallButton("Duplicate")) edit(host, document, CatalogOperation::Duplicate, address, i + 1);
		ImGui::SameLine();
		if (ImGui::SmallButton("Remove")) edit(host, document, CatalogOperation::Remove, address);
		ImGui::SameLine();
		if (ImGui::SmallButton("Up") && i) edit(host, document, CatalogOperation::Move, address, i - 1);
		ImGui::SameLine();
		if (ImGui::SmallButton("Down") && i + 1 < ids.size()) edit(host, document, CatalogOperation::Move, address, i + 1);
		ImGui::PopID();
	}
	ImGui::PopID(); ImGui::TreePop();
}
bool ref_kind(DefReference reference, AssetKind kind) {
	return (reference == DefReference::Model && kind == AssetKind::Model) ||
		(reference == DefReference::AnimationMap && kind == AssetKind::AnimationMap) ||
		(reference == DefReference::Texture && kind == AssetKind::Texture) ||
		(reference == DefReference::AiProfile && kind == AssetKind::AiProfile);
}
void reference_picker(EditorHost &host, const EditableDocument &document, CatalogAddress address, const DefField &field) {
	if (field.reference == DefReference::None || field.type != DefFieldType::Text) return;
	ImGui::SameLine();
	if (ImGui::SmallButton("Pick")) ImGui::OpenPopup("references");
	if (!ImGui::BeginPopup("references")) return;
	bool any = false;
	for (const auto &asset : host.view().scan.entries) {
		if (!ref_kind(field.reference, asset.kind)) continue;
		any = true;
		if (ImGui::Selectable(asset.logical_name.c_str())) set(host, document, address, field.id, asset.logical_name, false);
	}
	for (const auto &other : host.view().documents) for (const auto &row : other->rows()) {
		if ((field.reference == DefReference::Weapon && row->kind == DefRecordKind::Weapon) ||
			(field.reference == DefReference::Ammo && row->kind == DefRecordKind::Ammo)) {
			any = true;
			if (ImGui::Selectable(row->name().c_str())) set(host, document, address, field.id, row->name(), false);
		}
	}
	if (!any) ImGui::TextDisabled("No matching symbols in the open catalogs.");
	ImGui::EndPopup();
}
void field_control(EditorHost &host, const EditableDocument &document, CatalogAddress address,
	const void *record, const DefField &field) {
	const auto value = def_get(record, field);
	ImGui::PushID(field.id.c_str());
	ImGui::BeginDisabled(field.read_only || document.blocked());
	if (field.flags) {
		if (ImGui::TreeNode(field.id.c_str())) {
			const int64_t bits = std::get<int64_t>(value);
			for (const auto &choice : field.choices) {
				bool checked = (bits & choice.value) != 0;
				if (ImGui::Checkbox(choice.name, &checked)) {
					int64_t result = checked ? bits | choice.value : bits & ~choice.value;
					if (field.type == DefFieldType::Integer) result = int32_t(uint32_t(result));
					set(host, document, address, field.id, result, false);
				}
			}
			ImGui::TreePop();
		}
	} else if (!field.choices.empty()) {
		std::string label = std::holds_alternative<std::string>(value) ? std::get<std::string>(value) : std::to_string(std::get<int64_t>(value));
		for (const auto &choice : field.choices)
			if (const auto *number = std::get_if<int64_t>(&value); number && *number == choice.value) label = choice.name;
		if (ImGui::BeginCombo(field.id.c_str(), label.c_str())) {
			for (const auto &choice : field.choices) if (ImGui::Selectable(choice.name, label == choice.name))
				set(host, document, address, field.id, field.type == DefFieldType::Text ? DefValue(std::string(choice.name)) : DefValue(choice.value), false);
			ImGui::EndCombo();
		}
	} else if (field.type == DefFieldType::Text) {
		std::vector<char> text(field.width, 0);
		const auto &current = std::get<std::string>(value);
		std::memcpy(text.data(), current.data(), std::min(current.size(), text.size() - 1));
		if (ImGui::InputText(field.id.c_str(), text.data(), text.size()))
			set(host, document, address, field.id, std::string(text.data()));
		if (ImGui::IsItemDeactivatedAfterEdit()) host.request(make_request(EditorRequestKind::EndEdit, document.path()));
		reference_picker(host, document, address, field);
	} else if (field.type == DefFieldType::Real) {
		double number = std::get<double>(value);
		if (ImGui::InputDouble(field.id.c_str(), &number, 0, 0, "%.9g")) set(host, document, address, field.id, number);
		if (ImGui::IsItemDeactivatedAfterEdit()) host.request(make_request(EditorRequestKind::EndEdit, document.path()));
	} else {
		int64_t number = std::get<int64_t>(value);
		if (ImGui::InputScalar(field.id.c_str(), ImGuiDataType_S64, &number)) set(host, document, address, field.id, number);
		if (ImGui::IsItemDeactivatedAfterEdit()) host.request(make_request(EditorRequestKind::EndEdit, document.path()));
	}
	ImGui::EndDisabled(); ImGui::PopID();
}
} // namespace

void CatalogWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const auto &view = host_.view();
	if (!view.project_open) { ImGui::TextDisabled("Open a project to edit its catalogs."); return; }
	const char *labels[] = {"Items", "Weapons", "Ammo"};
	const char *files[] = {"items.def", "weapon.def", "ammo.def"};
	for (int i = 0; i < 3; ++i) {
		if (i) ImGui::SameLine();
        const auto *asset = view.scan.find(files[i]);
        std::string label = asset ? labels[i] : std::string("Create ") + labels[i];
        for (const auto &open : view.documents)
            if (asset && open->path() == asset->relative_path && open->dirty()) label += " *";
        if (ImGui::Button(label.c_str())) host_.request(make_request(
            asset ? EditorRequestKind::OpenDocument : EditorRequestKind::CreateCatalog, files[i]));
	}
	const auto *document = active(view);
	if (!document) { ImGui::TextDisabled("Choose a catalog above, or open it from Project files."); return; }
	ImGui::Text("%s%s", document->path().c_str(), document->dirty() ? " *" : "");
	if (ImGui::Button("Save")) host_.request(make_request(EditorRequestKind::Save));
	ImGui::SameLine();
	if (ImGui::Button("Reload")) host_.request(make_request(EditorRequestKind::ReloadDocument));
	ImGui::SameLine();
	if (ImGui::Button("Close")) host_.request(make_request(EditorRequestKind::CloseDocument));
	ImGui::SameLine();
	ImGui::BeginDisabled(!document->can_undo());
	if (ImGui::Button("Undo")) host_.request(make_request(EditorRequestKind::Undo));
	ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(!document->can_redo());
	if (ImGui::Button("Redo")) host_.request(make_request(EditorRequestKind::Redo));
	ImGui::EndDisabled();
	if (document->blocked()) ImGui::TextWrapped("This file has unsupported or malformed input. See Problems, correct the source, then Reload.");
	ImGui::InputText("Filter", filter_, sizeof(filter_));
	ImGui::SameLine(); ImGui::Checkbox("Sort by name", &sort_names_);
	ImGui::BeginDisabled(document->blocked());
	if (ImGui::Button("Add record")) edit(host_, *document, CatalogOperation::Add, {0, document->record_kind(), 0});
	if (document->kind() == AssetKind::WeaponDefs) {
		ImGui::SameLine();
		if (ImGui::Button("Add carry limit")) edit(host_, *document, CatalogOperation::Add, {0, DefRecordKind::Carry, 0});
	}
	std::vector<std::shared_ptr<const CatalogRow>> visible;
	for (const auto &row : document->rows()) if (matches(row->name(), filter_)) visible.push_back(row);
	if (sort_names_) std::stable_sort(visible.begin(), visible.end(), [](const auto &a, const auto &b) { return normalized_logical_name(a->name()) < normalized_logical_name(b->name()); });
	for (const auto &row : visible) {
		ImGui::PushID(int(row->id));
		const std::string name = row->kind == DefRecordKind::Carry ? "Carry: " + row->name() : row->name();
		const CatalogAddress address{row->id, row->kind, 0};
		if (ImGui::Selectable(name.c_str(), view.selection.row == row->id)) select(host_, *document, address);
		if (view.selection.row == row->id) {
			const auto &rows = document->rows();
			const size_t index = size_t(std::find(rows.begin(), rows.end(), row) - rows.begin());
			if (ImGui::SmallButton("Duplicate")) edit(host_, *document, CatalogOperation::Duplicate, address, index + 1);
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove")) edit(host_, *document, CatalogOperation::Remove, address);
			ImGui::SameLine();
			if (ImGui::SmallButton("Up") && index) edit(host_, *document, CatalogOperation::Move, address, index - 1);
			ImGui::SameLine();
			if (ImGui::SmallButton("Down") && index + 1 < rows.size()) edit(host_, *document, CatalogOperation::Move, address, index + 1);
		}
		ImGui::PopID();
	}
	ImGui::EndDisabled();
	if (document->kind() == AssetKind::ItemDefs && ImGui::TreeNode("Vehicle spawn IDs")) {
		for (size_t i = 0; i < document->spawn_ids().size(); ++i) {
			int id = document->spawn_ids()[i]; ImGui::PushID(int(i));
			if (ImGui::InputInt("Item ID", &id)) {
				auto request = make_request(EditorRequestKind::EditRecord, document->path());
				request.catalog_edit.operation = CatalogOperation::SetSpawnId;
				request.catalog_edit.position = i; request.catalog_edit.value = int64_t(id); host_.request(std::move(request));
			}
			ImGui::PopID();
		}
		if (document->spawn_ids().size() < 32 && ImGui::Button("Add spawn slot")) {
			auto request = make_request(EditorRequestKind::EditRecord, document->path());
			request.catalog_edit.operation = CatalogOperation::SetSpawnId;
			request.catalog_edit.position = document->spawn_ids().size(); request.catalog_edit.value = int64_t(0); host_.request(std::move(request));
		}
		ImGui::TreePop();
	}
}

void CatalogInspector::draw(devtools::ImGuiPass &, uint64_t) {
	const auto &view = host_.view();
	const auto *document = active(view);
	if (!document || !view.selection.row) { ImGui::TextDisabled("Select a catalog record."); return; }
	const CatalogRow *row = nullptr;
	for (const auto &candidate : document->rows()) if (candidate->id == view.selection.row) row = candidate.get();
	if (!row) { ImGui::TextDisabled("The selected record was removed."); return; }
	ImGui::TextUnformatted(row->name().c_str());
	if (view.selection.child && ImGui::Button("Parent record")) select(host_, *document, {row->id, row->kind, 0});
	const void *record = document->record(view.selection);
	ImGui::InputText("Fields", filter_, sizeof(filter_));
	if (record) for (const auto &field : def_fields(view.selection.kind))
		if (matches(field.id, filter_)) field_control(host_, *document, view.selection, record, field);
	ImGui::Separator();
	if (row->kind == DefRecordKind::Item) collection(host_, *document, *row, DefRecordKind::Attachment, "Attachments", row->children);
	if (row->kind == DefRecordKind::Weapon) {
		collection(host_, *document, *row, DefRecordKind::Action, "Actions", row->children);
		collection(host_, *document, *row, DefRecordKind::Sight, "Sights", row->sights);
	}
	if (row->kind == DefRecordKind::Ammo) collection(host_, *document, *row, DefRecordKind::Effect, "Effects", row->children);
	for (const auto &d : view.diagnostics)
		if (d.asset == document->path() && d.row_id == row->id) ImGui::TextWrapped("%s: %s", d.field.c_str(), d.message.c_str());
}
} // namespace opennova::editor
