#include "def_catalog_document.h"

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <runtime/hud/game_text_lookup.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>

namespace opennova::editor {
using namespace def;
namespace {

template <class T> T *copy(const T *data, size_t count) {
	if (!count) return nullptr;
	auto *result = static_cast<T *>(std::malloc(count * sizeof(T)));
	if (!result) throw std::bad_alloc();
	std::memcpy(result, data, count * sizeof(T));
	return result;
}

size_t collection_slots(DefRecordKind kind) {
	switch (kind) {
	case DefRecordKind::Item: case DefRecordKind::Ammo: return 1;
	case DefRecordKind::Weapon: return 2;
	default: return 0;
	}
}

size_t slot_of(DefRecordKind child_kind) { return child_kind == DefRecordKind::Sight ? 1 : 0; }

// The nested record an address names inside a row, or the row's own record.
void *child_record(CatalogRow &row, const NodeAddress &address) {
	const DefRecordKind kind = def_kind(address.kind);
	if (!address.child) return row.record_kind() == kind ? row.record() : nullptr;
	const size_t slot = slot_of(kind);
	if (slot >= row.collections.size()) return nullptr;
	const auto &ids = row.collections[slot];
	const auto found = std::find(ids.begin(), ids.end(), address.child);
	if (found == ids.end()) return nullptr;
	const size_t i = size_t(found - ids.begin());
	if (row.record_kind() == DefRecordKind::Item && kind == DefRecordKind::Attachment)
		return &std::get<DefItemDef>(row.data).emplacement_attachments[i];
	if (row.record_kind() == DefRecordKind::Weapon && kind == DefRecordKind::Action)
		return &std::get<DefWeaponDef>(row.data).actions[i];
	if (row.record_kind() == DefRecordKind::Weapon && kind == DefRecordKind::Sight)
		return &std::get<DefWeaponDef>(row.data).sights[i];
	if (row.record_kind() == DefRecordKind::Ammo && kind == DefRecordKind::Effect)
		return &std::get<DefAmmoDef>(row.data).effects_table[i];
	return nullptr;
}

template <class T>
bool edit_children(T *&entries, size_t &count, std::vector<NodeId> &ids, const Edit &edit,
                   const Document::IdAllocator &allocate, NodeId &added) {
	size_t index = size_t(std::find(ids.begin(), ids.end(), edit.address.child) - ids.begin());
	if (edit.operation != EditOperation::Add && index == count) return false;
	std::vector<T> values;
	if (count) values.assign(entries, entries + count);
	if (edit.operation == EditOperation::Add || edit.operation == EditOperation::Duplicate) {
		T row{};
		if (edit.operation == EditOperation::Duplicate) row = values[index];
		else def_init_record(def_kind(edit.address.kind), &row);
		const size_t position = std::min(edit.position, count);
		values.insert(values.begin() + static_cast<std::ptrdiff_t>(position), row);
		added = allocate();
		ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(position), added);
	} else if (edit.operation == EditOperation::Remove) {
		values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
		ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(index));
	} else if (edit.operation == EditOperation::Move) {
		const size_t to = std::min(edit.position, count - 1);
		const T row = values[index];
		const NodeId id = ids[index];
		values.erase(values.begin() + static_cast<std::ptrdiff_t>(index));
		values.insert(values.begin() + static_cast<std::ptrdiff_t>(to), row);
		ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(index));
		ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(to), id);
	} else {
		return false;
	}
	T *updated = copy(values.data(), values.size());
	std::free(entries);
	entries = updated;
	count = values.size();
	return true;
}

void update_attachment_slots(DefItemDef &item) {
	item.emplacement_g_slot = 0;
	item.emplacement_c_slot = 0;
	for (size_t i = 0; i < item.emplacement_attachments_count; ++i) {
		if (item.emplacement_attachments[i].kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G) item.emplacement_g_slot = int(i + 1);
		if (item.emplacement_attachments[i].kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C) item.emplacement_c_slot = int(i + 1);
	}
}

ReferenceKind reference_kind(DefReference reference) {
	switch (reference) {
	case DefReference::Model: return ReferenceKind::Model;
	case DefReference::AnimationMap: return ReferenceKind::AnimationMap;
	case DefReference::Ammo: return ReferenceKind::Ammo;
	case DefReference::Weapon: return ReferenceKind::Weapon;
	case DefReference::Item: return ReferenceKind::Item;
	case DefReference::Texture: return ReferenceKind::Texture;
	case DefReference::Sound: return ReferenceKind::Sound;
	case DefReference::Particle: return ReferenceKind::Particle;
	case DefReference::AiProfile: return ReferenceKind::AiProfile;
	case DefReference::GameText: return ReferenceKind::TextId;
	case DefReference::OtherText: return ReferenceKind::OtherText;
	case DefReference::UserPoint: return ReferenceKind::UserPoint;
	default: return ReferenceKind::None;
	}
}

// The symbol a record's field defines, which the other catalogs and the game name it by: a
// weapon's name, an ammo's name, an item's id.
ReferenceKind defined_by(DefRecordKind kind, const std::string &field) {
	if (kind == DefRecordKind::Weapon && field == "weapon_name") return ReferenceKind::Weapon;
	if (kind == DefRecordKind::Ammo && field == "name") return ReferenceKind::Ammo;
	if (kind == DefRecordKind::Item && field == "id") return ReferenceKind::Item;
	return ReferenceKind::None;
}

// Where a field's reference resolves, when the field says: a def's game-text fields are
// string ids in the game's own table, the loadout label's in its WepDes section
// (menu::weapon_label), a weapon's attach label in its Overlays section (the runtime's
// weapon table attach_text_id); another text key is not resolved yet.
void resolve_field(const DefField &field, FieldSchema &entry) {
	const std::string game_text = strutil::to_upper(hud::kGameTextTable) + "/";
	if (field.reference == DefReference::GameText) {
		entry.scope = game_text + hud::kGameTextWepDes;
	} else if (field.reference == DefReference::OtherText && field.id == "attach_text_id") {
		entry.reference = ReferenceKind::TextId;
		entry.scope = game_text + hud::kGameTextOverlays;
	}
}

// What the field's line says of it (def_member_property): the key the file writes, the
// parser's heading and note; a member the line writes as a number of its own (def_authored)
// in the units the file writes it, named by the key (and its place on a line of several)
// unless the line names it; a light's colour packed 0xRRGGBB; a whole percent kept to the
// parser's 0..100; a field whose line has a present flag optional, that flag read only (the
// field's tick is it).
void describe(DefRecordKind kind, const DefField &field, FieldSchema &entry) {
	size_t index = 0;
	const DefProperty *property = def_member_property(kind, field.id, &index);
	if (!property) return;
	if (!property->key.empty() && property->key != field.id) entry.token = property->key;
	entry.section = property->section;
	entry.description = property->note;
	const std::string label = index < property->labels.size() ? property->labels[index] : std::string();
	const DefAuthored authored = def_authored(kind, field.id);
	if (authored != DefAuthored::None) {
		entry.type = authored == DefAuthored::Integer ? FieldType::Integer : FieldType::Real;
		entry.unit = property->units.size() == 1 ? property->units.front()
		             : index < property->units.size() ? property->units[index]
		                                               : std::string();
		entry.label = !label.empty() ? label
		              : property->fields.size() == 1 ? property->key
		                                             : property->key + " " + std::to_string(index + 1);
	} else if (!label.empty()) {
		entry.label = label;
	}
	// The range the game's reader keeps the member to (a weapon's category and rank, an item's
	// unit_type byte).
	if (field.ranged) {
		entry.ranged = true;
		entry.min = double(field.min);
		entry.max = double(field.max);
	}
	// [orig: light_transfer atoi clamped 0..100 @ 0x4A1A12..0x4A1A50]
	if (property->encoding == DefEncoding::Percent) {
		entry.ranged = true;
		entry.min = 0.0;
		entry.max = 100.0;
	}
	if ((property->encoding == DefEncoding::LightMove || property->encoding == DefEncoding::LightImpact) && index == 1)
		entry.color = FieldColor::PackedRgb;
	if (!property->present_field.empty()) entry.optional = true;
}

// Whether the field is a line's present flag (DefProperty::present_field).
bool is_present_flag(DefRecordKind kind, const std::string &id) {
	for (const DefProperty &property : def_properties(kind))
		if (property.present_field == id) return true;
	return false;
}

FieldType field_type(DefFieldType type) {
	switch (type) {
	case DefFieldType::Unsigned: return FieldType::Unsigned;
	case DefFieldType::Byte: return FieldType::Byte;
	case DefFieldType::Count: return FieldType::Count;
	case DefFieldType::Real: return FieldType::Real;
	case DefFieldType::Text: return FieldType::Text;
	default: return FieldType::Integer;
	}
}

std::vector<SourceIssue> source_issues(const DefParseReport &report) {
	std::vector<SourceIssue> issues;
	for (const DefIssue &issue : report)
		issues.push_back({issue.blocks(), issue.line, issue.record, issue.field, issue.message});
	return issues;
}

const ItemsFileState *items_state(const FileState *state) { return dynamic_cast<const ItemsFileState *>(state); }

} // namespace

const char *catalog_name_field(DefRecordKind kind) {
	return kind == DefRecordKind::Item ? "display_name" : kind == DefRecordKind::Weapon ? "weapon_name" : "name";
}

bool is_catalog_kind(AssetKind kind) {
	return asset_kind_row(kind).document == DocumentTypeId::Catalog;
}

CatalogRow::CatalogRow(DefRecordKind k) {
	kind = node_kind(k);
	switch (k) {
	case DefRecordKind::Item: data = DefItemDef{}; break;
	case DefRecordKind::Weapon: data = DefWeaponDef{}; break;
	case DefRecordKind::Ammo: data = DefAmmoDef{}; break;
	default: data = DefAmmoClassCarry{}; break;
	}
	def_init_record(k, record());
	collections.resize(collection_slots(k));
}

CatalogRow::CatalogRow(const CatalogRow &other) : Node(other), data(other.data) {
	if (auto *p = std::get_if<DefItemDef>(&data)) p->emplacement_attachments = copy(p->emplacement_attachments, p->emplacement_attachments_count);
	if (auto *p = std::get_if<DefWeaponDef>(&data)) {
		p->actions = copy(p->actions, p->actions_count);
		p->sights = copy(p->sights, p->sights_count);
	}
	if (auto *p = std::get_if<DefAmmoDef>(&data)) p->effects_table = copy(p->effects_table, p->effects_table_count);
}

size_t CatalogRow::footprint() const {
	size_t bytes = sizeof(CatalogRow) + collections_footprint();
	if (const auto *p = std::get_if<DefItemDef>(&data))
		bytes += p->emplacement_attachments_count * sizeof(*p->emplacement_attachments);
	if (const auto *p = std::get_if<DefWeaponDef>(&data))
		bytes += p->actions_count * sizeof(*p->actions) + p->sights_count * sizeof(*p->sights);
	if (const auto *p = std::get_if<DefAmmoDef>(&data))
		bytes += p->effects_table_count * sizeof(*p->effects_table);
	return bytes;
}

CatalogRow::~CatalogRow() {
	if (auto *p = std::get_if<DefItemDef>(&data)) std::free(p->emplacement_attachments);
	if (auto *p = std::get_if<DefWeaponDef>(&data)) { std::free(p->actions); std::free(p->sights); }
	if (auto *p = std::get_if<DefAmmoDef>(&data)) std::free(p->effects_table);
}

void *CatalogRow::record() { return std::visit([](auto &r) -> void * { return &r; }, data); }
const void *CatalogRow::record() const { return std::visit([](const auto &r) -> const void * { return &r; }, data); }

std::string CatalogRow::name() const {
	return std::get<std::string>(def_get(record(), *def_field(record_kind(), catalog_name_field(record_kind()))));
}

DefRecordKind DefCatalogDocument::record_kind() const {
	return kind() == AssetKind::ItemDefs ? DefRecordKind::Item : kind() == AssetKind::WeaponDefs ? DefRecordKind::Weapon : DefRecordKind::Ammo;
}

const void *DefCatalogDocument::record(const NodeAddress &address) const {
	const Node *node = row(address.row);
	return node ? child_record(const_cast<CatalogRow &>(static_cast<const CatalogRow &>(*node)), address) : nullptr;
}

const std::vector<int> &DefCatalogDocument::spawn_ids() const {
	static const std::vector<int> none;
	const auto *state = items_state(file_state());
	return state ? state->spawn_ids : none;
}

bool DefCatalogDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                               std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &issues,
                               Diagnostic &error) {
	if (!is_catalog_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file has no catalog editor.", path());
		return false;
	}
	DefParseReport report;
	auto append = [&](DefRecordKind type, const auto &entry, size_t children, size_t sights) {
		auto row = std::make_shared<CatalogRow>(type);
		row->data = entry; // transfer the parser-owned arrays; the file's outer arrays are freed below
		if (!row->collections.empty()) row->collections[0].resize(children);
		if (row->collections.size() > 1) row->collections[1].resize(sights);
		rows.push_back(row);
	};
	if (kind() == AssetKind::ItemDefs) {
		DefItemsFile file{};
		def_parse_items_memory(bytes.data(), bytes.size(), &file, &report);
		auto spawn = std::make_shared<ItemsFileState>();
		spawn->spawn_ids.assign(file.vehicle_spawn_ids, file.vehicle_spawn_ids + file.vehicle_spawn_id_count);
		state = spawn;
		for (size_t i = 0; i < file.count; ++i) append(DefRecordKind::Item, file.entries[i], file.entries[i].emplacement_attachments_count, 0);
		std::free(file.entries);
	} else if (kind() == AssetKind::WeaponDefs) {
		DefWeaponsFile file{};
		def_parse_weapons_memory(bytes.data(), bytes.size(), &file, &report);
		for (size_t i = 0; i < file.ammo_classes_count; ++i) append(DefRecordKind::Carry, file.ammo_classes[i], 0, 0);
		for (size_t i = 0; i < file.count; ++i) append(DefRecordKind::Weapon, file.entries[i], file.entries[i].actions_count, file.entries[i].sights_count);
		std::free(file.entries);
		std::free(file.ammo_classes);
	} else {
		DefAmmoFile file{};
		def_parse_ammo_memory(bytes.data(), bytes.size(), &file, &report);
		for (size_t i = 0; i < file.count; ++i) append(DefRecordKind::Ammo, file.entries[i], file.entries[i].effects_table_count, 0);
		std::free(file.entries);
	}
	issues = source_issues(report);
	return true;
}

SerializeResult DefCatalogDocument::serialize() const {
	SerializeResult result;
	if (blocked()) {
		for (const auto &issue : issues()) if (issue.blocks) result.issues.push_back(issue);
		return result;
	}
	auto data = [](const Node &row) -> const CatalogRow & { return static_cast<const CatalogRow &>(row); };
	DefWriteResult written;
	// Only the contiguous outer arrays are temporary. Nested arrays remain owned by
	// the native rows; the writer borrows them for the duration of this call.
	if (kind() == AssetKind::ItemDefs) {
		std::vector<DefItemDef> items;
		for (const auto &row : rows()) items.push_back(std::get<DefItemDef>(data(*row).data));
		DefItemsFile file{};
		file.entries = items.data(); file.count = items.size();
		const auto &spawn = spawn_ids();
		file.vehicle_spawn_id_count = int(spawn.size());
		std::copy(spawn.begin(), spawn.end(), file.vehicle_spawn_ids);
		written = def_write_items(file);
	} else if (kind() == AssetKind::WeaponDefs) {
		std::vector<DefWeaponDef> weapons;
		std::vector<DefAmmoClassCarry> carries;
		for (const auto &row : rows()) {
			if (data(*row).record_kind() == DefRecordKind::Carry) carries.push_back(std::get<DefAmmoClassCarry>(data(*row).data));
			else weapons.push_back(std::get<DefWeaponDef>(data(*row).data));
		}
		DefWeaponsFile file{};
		file.entries = weapons.data(); file.count = weapons.size();
		file.ammo_classes = carries.data(); file.ammo_classes_count = carries.size();
		written = def_write_weapons(file);
	} else {
		std::vector<DefAmmoDef> ammo;
		for (const auto &row : rows()) ammo.push_back(std::get<DefAmmoDef>(data(*row).data));
		DefAmmoFile file{};
		file.entries = ammo.data(); file.count = ammo.size();
		written = def_write_ammo(file);
	}
	result.text = std::move(written.text);
	result.issues = source_issues(written.diagnostics);
	return result;
}

std::shared_ptr<Node> DefCatalogDocument::make_node(
		NodeKind kind, NodeId id, const std::vector<std::shared_ptr<const Node>> &rows,
		std::string &error) {
	const RecordKindRow *top = kind_row(kind);
	if (!top || !top->top) {
		error = "This catalog cannot add that kind of record.";
		return nullptr;
	}
	auto row = std::make_shared<CatalogRow>(def_kind(kind));
	std::string ignored;
	def_set(row->record(), *def_field(def_kind(kind), catalog_name_field(def_kind(kind))), std::string("New_") + std::to_string(id), ignored);
	if (auto *p = std::get_if<DefItemDef>(&row->data)) {
		p->type = DEF_ITEM_TYPE_MARKER;
		p->id = 100000;
		for (;;) {
			bool used = false;
			for (const auto &other : rows)
				if (std::get<DefItemDef>(static_cast<const CatalogRow &>(*other).data).id == p->id) { used = true; break; }
			if (!used) break;
			++p->id;
		}
	}
	return row;
}

bool DefCatalogDocument::set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
                                   std::string &error) {
	void *target = child_record(static_cast<CatalogRow &>(row), address);
	const DefRecordKind kind = def_kind(address.kind);
	const auto *description = def_field(kind, field);
	if (!target || !description) { error = "Unknown field."; return false; }
	if (is_present_flag(kind, field)) { error = "Whether the line is written is the field's own tick."; return false; }
	const DefValue before = def_get(target, *description);
	// A number in the units the file writes it goes through the file's own line.
	const bool set = def_authored(kind, field) != DefAuthored::None ? def_authored_set(kind, target, field, value, error)
	                                                                : def_set(target, *description, value, error);
	if (!set) return false;
	// A Set of another value writes an optional line (edit.h); one of the value it holds
	// changes nothing.
	const DefProperty *property = def_member_property(kind, field);
	const DefField *flag = property && !property->present_field.empty() ? def_field(kind, property->present_field) : nullptr;
	if (flag && def_get(target, *description) != before && !def_set(target, *flag, int64_t(1), error)) return false;
	def_sync_derived(kind, target, field);
	return true;
}

bool DefCatalogDocument::read_present(const Node &row, const NodeAddress &address, const std::string &field) const {
	const DefRecordKind kind = def_kind(address.kind);
	const DefProperty *property = field.empty() ? nullptr : def_member_property(kind, field);
	if (!property || property->present_field.empty()) return true;
	const void *target = child_record(const_cast<CatalogRow &>(static_cast<const CatalogRow &>(row)), address);
	const DefField *flag = def_field(kind, property->present_field);
	return target && flag && std::get<int64_t>(def_get(target, *flag)) != 0;
}

bool DefCatalogDocument::set_present(Node &row, const NodeAddress &address, const std::string &field, bool present,
                                     std::string &error) {
	const DefRecordKind kind = def_kind(address.kind);
	const DefProperty *property = def_member_property(kind, field);
	void *target = child_record(static_cast<CatalogRow &>(row), address);
	const DefField *flag = property && !property->present_field.empty() ? def_field(kind, property->present_field) : nullptr;
	if (!target || !flag) {
		error = "This line is always written.";
		return false;
	}
	return def_set(target, *flag, int64_t(present ? 1 : 0), error);
}

bool DefCatalogDocument::edit_collection(Node &node, const Edit &edit, const IdAllocator &allocate, NodeId &added,
                                         std::string &error) {
	auto &row = static_cast<CatalogRow &>(node);
	const DefRecordKind kind = def_kind(edit.address.kind);
	const size_t slot = slot_of(kind);
	if (slot >= row.collections.size()) { error = "This record has no such collection."; return false; }
	auto &ids = row.collections[slot];
	bool ok = false;
	if (auto *p = std::get_if<DefItemDef>(&row.data); p && kind == DefRecordKind::Attachment)
		ok = edit_children(p->emplacement_attachments, p->emplacement_attachments_count, ids, edit, allocate, added);
	if (auto *p = std::get_if<DefWeaponDef>(&row.data)) {
		if (kind == DefRecordKind::Action) ok = edit_children(p->actions, p->actions_count, ids, edit, allocate, added);
		if (kind == DefRecordKind::Sight) ok = edit_children(p->sights, p->sights_count, ids, edit, allocate, added);
	}
	if (auto *p = std::get_if<DefAmmoDef>(&row.data); p && kind == DefRecordKind::Effect)
		ok = edit_children(p->effects_table, p->effects_table_count, ids, edit, allocate, added);
	if (!ok) error = "This collection cannot accept that edit.";
	return ok;
}

bool DefCatalogDocument::set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) {
	const auto *value = std::get_if<int64_t>(&edit.value);
	const auto *current = items_state(state.get());
	const size_t count = current ? current->spawn_ids.size() : 0;
	if (kind() != AssetKind::ItemDefs || !value || edit.position > count ||
	    edit.position >= size_t(DEF_VEHICLE_SPAWN_SLOTS) || *value < INT32_MIN || *value > INT32_MAX) {
		error = make_finding(CoreFinding::DocumentValue, DiagnosticSeverity::Error, "Invalid vehicle spawn slot.", path());
		return false;
	}
	auto updated = current ? std::static_pointer_cast<ItemsFileState>(current->clone()) : std::make_shared<ItemsFileState>();
	if (edit.position == updated->spawn_ids.size()) updated->spawn_ids.push_back(int(*value));
	else updated->spawn_ids[edit.position] = int(*value);
	state = updated;
	return true;
}

std::shared_ptr<const FileState> DefCatalogDocument::state_after_remove(const std::shared_ptr<const FileState> &state,
                                                                        size_t remaining) const {
	// A file-wide spawn registry has no native home once the last item is removed.
	if (kind() == AssetKind::ItemDefs && remaining == 0) return std::make_shared<ItemsFileState>();
	return state;
}

void DefCatalogDocument::after_edit(Node &node) {
	if (auto *p = std::get_if<DefItemDef>(&static_cast<CatalogRow &>(node).data)) update_attachment_slots(*p);
}

const std::vector<RecordKindRow> &DefCatalogDocument::kinds() const {
	static const std::vector<RecordKindRow> items = {
	        {node_kind(DefRecordKind::Item), "item", "Item", "Add record", true},
	        {node_kind(DefRecordKind::Attachment), "attachment", "Attachment"},
	};
	static const std::vector<RecordKindRow> weapons = {
	        {node_kind(DefRecordKind::Weapon), "weapon", "Weapon", "Add record", true},
	        {node_kind(DefRecordKind::Action), "action", "Action"},
	        {node_kind(DefRecordKind::Sight), "sight", "Sight"},
	        {node_kind(DefRecordKind::Carry), "carry", "Carry limit", "Add carry limit", true},
	};
	static const std::vector<RecordKindRow> ammo = {
	        {node_kind(DefRecordKind::Ammo), "ammo", "Ammo", "Add record", true},
	        {node_kind(DefRecordKind::Effect), "effect", "Effect"},
	};
	switch (record_kind()) {
	case DefRecordKind::Item: return items;
	case DefRecordKind::Weapon: return weapons;
	default: return ammo;
	}
}

// A row holds its attachments / actions and sights / effects; nothing nests deeper.
std::vector<Document::Collection> DefCatalogDocument::collections(const Node &row, const NodeAddress &owner) const {
	if (owner.child) return {};
	std::vector<CollectionSpec> specs;
	switch (def_kind(row.kind)) {
	case DefRecordKind::Item:
		specs = {{node_kind(DefRecordKind::Attachment), "Attachments", "userpoint"}};
		break;
	case DefRecordKind::Weapon:
		specs = {{node_kind(DefRecordKind::Action), "Actions", "name"},
		         {node_kind(DefRecordKind::Sight), "Sights", "texture"}};
		break;
	case DefRecordKind::Ammo:
		specs = {{node_kind(DefRecordKind::Effect), "Effects", "surface_type"}};
		break;
	default: break;
	}
	std::vector<Collection> out;
	for (const CollectionSpec &spec : specs) {
		const size_t slot = slot_of(def_kind(spec.kind));
		out.push_back({spec, slot < row.collections.size() ? row.collections[slot] : std::vector<NodeId>()});
	}
	return out;
}

void DefCatalogDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	// A bit per id of the file-wide registry (record_choices).
	if (use.schema->id == "vehicle_spawn_mask" && def_kind(address.kind) == DefRecordKind::Item) {
		use.own_choices = true;
		return;
	}
	if (use.reference != ReferenceKind::UserPoint) return;
	// The user point is looked up on the item's graphic model [orig:
	// Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0]; with no graphic nothing is.
	const Node *record = row(address.row);
	Value graphic;
	if (!record || !get({address.row, record->kind, 0}, "graphic", graphic) || !std::holds_alternative<std::string>(graphic) ||
	    std::get<std::string>(graphic).empty()) {
		use.reference = ReferenceKind::None;
		return;
	}
	std::string model = basename_of(std::get<std::string>(graphic));
	if (std::filesystem::path(model).extension().empty()) model += ".3di";
	use.scope = strutil::to_upper(model);
}

bool DefCatalogDocument::record_choices(const NodeAddress &address, const FieldUse &use,
		std::vector<FieldChoice> &out) const {
	if (use.schema->id != "vehicle_spawn_mask" || def_kind(address.kind) != DefRecordKind::Item)
		return false;
	// A bit per id of the file-wide registry, in its slot's order [orig: @0x4A0253; @0x49DFC0].
	const std::vector<int> &ids = spawn_ids();
	for (size_t i = 0; i < ids.size() && i < size_t(DEF_VEHICLE_SPAWN_SLOTS); ++i)
		out.push_back({std::to_string(ids[i]), int64_t(uint32_t(1) << i), ""});
	return true;
}

const std::vector<FieldSchema> &DefCatalogDocument::schema(NodeKind kind) {
	// Every record kind's fields, made once for the process: a record's FieldUse points into
	// them (the language makes the one initialisation, whichever thread asks first).
	static const std::vector<std::vector<FieldSchema>> tables = [] {
		std::vector<std::vector<FieldSchema>> out;
		for (int k = 0; k <= int(DefRecordKind::Carry); ++k) {
			const DefRecordKind record = DefRecordKind(k);
			std::vector<FieldSchema> schema;
			for (const DefField &field : def_fields(record)) {
				FieldSchema entry;
				entry.id = field.id;
				entry.type = field_type(field.type);
				entry.width = field.width;
				entry.reference = reference_kind(field.reference);
				resolve_field(field, entry);
				entry.defines = defined_by(record, field.id);
				for (const DefChoice &choice : field.choices)
					entry.choices.push_back({choice.name, choice.value, choice.label});
				entry.flags = field.flags;
				entry.open_choices = field.open;
				entry.read_only = field.read_only || is_present_flag(record, field.id);
				describe(record, field, entry);
				schema.push_back(std::move(entry));
			}
			out.push_back(std::move(schema));
		}
		return out;
	}();
	static const std::vector<FieldSchema> none;
	return kind >= 0 && size_t(kind) < tables.size() ? tables[size_t(kind)] : none;
}

bool DefCatalogDocument::read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const {
	const void *target = child_record(const_cast<CatalogRow &>(static_cast<const CatalogRow &>(row)), address);
	const DefRecordKind kind = def_kind(address.kind);
	const auto *description = def_field(kind, field);
	if (!target || !description) return false;
	// In written units while the line carries the stored word, else as stored.
	if (def_authored(kind, field) != DefAuthored::None && def_authored_get(kind, target, field, out)) return true;
	out = def_get(target, *description);
	return true;
}

bool DefCatalogDocument::same_file_state(const FileState *a, const FileState *b) const {
	static const std::vector<int> none;
	const auto *before = items_state(a), *after = items_state(b);
	return (before ? before->spawn_ids : none) == (after ? after->spawn_ids : none);
}

} // namespace opennova::editor
