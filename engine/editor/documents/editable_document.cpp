#include "editable_document.h"

#include <editor/project/project_files.h>
#include <base/gameprofile/gameprofile.h>
#include <base/vfs/vfs_decode.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

namespace opennova::editor {
using namespace def;
namespace {

template<class T> T *copy(const T *data, size_t count) {
	if (!count) return nullptr;
	auto *result = static_cast<T *>(std::malloc(count * sizeof(T)));
	if (!result) throw std::bad_alloc();
	std::memcpy(result, data, count * sizeof(T));
	return result;
}
uint64_t fingerprint(const std::vector<uint8_t> &bytes) {
	uint64_t value = 14695981039346656037ull;
	for (const uint8_t byte : bytes) { value ^= byte; value *= 1099511628211ull; }
	return value;
}
bool fail(Diagnostic &error, const std::string &path, const char *code, const std::string &message,
          const std::string &field = {}) {
	error = make_diagnostic(DiagnosticSeverity::Error, code, message, path, field);
	return false;
}
void *child_record(CatalogRow &row, const CatalogAddress &address) {
	if (!address.child) return row.kind == address.kind ? row.record() : nullptr;
	const auto &ids = address.kind == DefRecordKind::Sight ? row.sights : row.children;
	const auto found = std::find(ids.begin(), ids.end(), address.child);
	if (found == ids.end()) return nullptr;
	const size_t i = size_t(found - ids.begin());
	if (row.kind == DefRecordKind::Item && address.kind == DefRecordKind::Attachment)
		return &std::get<DefItemDef>(row.data).emplacement_attachments[i];
	if (row.kind == DefRecordKind::Weapon && address.kind == DefRecordKind::Action)
		return &std::get<DefWeaponDef>(row.data).actions[i];
	if (row.kind == DefRecordKind::Weapon && address.kind == DefRecordKind::Sight)
		return &std::get<DefWeaponDef>(row.data).sights[i];
	if (row.kind == DefRecordKind::Ammo && address.kind == DefRecordKind::Effect)
		return &std::get<DefAmmoDef>(row.data).effects_table[i];
	return nullptr;
}
template<class T> bool edit_children(T *&entries, size_t &count, std::vector<CatalogId> &ids,
	const CatalogEdit &edit, CatalogId &next_id, CatalogId &added) {
	size_t index = size_t(std::find(ids.begin(), ids.end(), edit.address.child) - ids.begin());
	if (edit.operation != CatalogOperation::Add && index == count) return false;
	std::vector<T> values;
	if (count) values.assign(entries, entries + count);
	if (edit.operation == CatalogOperation::Add || edit.operation == CatalogOperation::Duplicate) {
		T row{};
		if (edit.operation == CatalogOperation::Duplicate) row = values[index];
		else def_init_record(edit.address.kind, &row);
		const size_t position = std::min(edit.position, count);
		values.insert(values.begin() + position, row);
		added = next_id++;
		ids.insert(ids.begin() + position, added);
	} else if (edit.operation == CatalogOperation::Remove) {
		values.erase(values.begin() + index);
		ids.erase(ids.begin() + index);
	} else if (edit.operation == CatalogOperation::Move) {
		const size_t to = std::min(edit.position, count - 1);
		const T row = values[index]; const CatalogId id = ids[index];
		values.erase(values.begin() + index); values.insert(values.begin() + to, row);
		ids.erase(ids.begin() + index); ids.insert(ids.begin() + to, id);
	} else return false;
	T *updated = copy(values.data(), values.size());
	std::free(entries); entries = updated; count = values.size();
	return true;
}
void update_attachment_slots(DefItemDef &item) {
	item.emplacement_g_slot = 0; item.emplacement_c_slot = 0;
	for (size_t i = 0; i < item.emplacement_attachments_count; ++i) {
		if (item.emplacement_attachments[i].kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G) item.emplacement_g_slot = int(i + 1);
		if (item.emplacement_attachments[i].kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C) item.emplacement_c_slot = int(i + 1);
	}
}
} // namespace

bool is_catalog_kind(AssetKind kind) {
	return kind == AssetKind::ItemDefs || kind == AssetKind::WeaponDefs || kind == AssetKind::AmmoDefs;
}
CatalogRow::CatalogRow(DefRecordKind k) : kind(k) {
	switch (kind) {
	case DefRecordKind::Item: data = DefItemDef{}; break;
	case DefRecordKind::Weapon: data = DefWeaponDef{}; break;
	case DefRecordKind::Ammo: data = DefAmmoDef{}; break;
	default: data = DefAmmoClassCarry{}; break;
	}
	def_init_record(kind, record());
}
CatalogRow::CatalogRow(const CatalogRow &other)
	: id(other.id), kind(other.kind), data(other.data), children(other.children), sights(other.sights) {
	if (auto *p = std::get_if<DefItemDef>(&data)) p->emplacement_attachments = copy(p->emplacement_attachments, p->emplacement_attachments_count);
	if (auto *p = std::get_if<DefWeaponDef>(&data)) {
		p->actions = copy(p->actions, p->actions_count); p->sights = copy(p->sights, p->sights_count);
	}
	if (auto *p = std::get_if<DefAmmoDef>(&data)) p->effects_table = copy(p->effects_table, p->effects_table_count);
}
CatalogRow::~CatalogRow() {
	if (auto *p = std::get_if<DefItemDef>(&data)) std::free(p->emplacement_attachments);
	if (auto *p = std::get_if<DefWeaponDef>(&data)) { std::free(p->actions); std::free(p->sights); }
	if (auto *p = std::get_if<DefAmmoDef>(&data)) std::free(p->effects_table);
}
void *CatalogRow::record() { return std::visit([](auto &r) -> void * { return &r; }, data); }
const void *CatalogRow::record() const { return std::visit([](const auto &r) -> const void * { return &r; }, data); }
std::string CatalogRow::name() const {
	const char *field = kind == DefRecordKind::Item ? "display_name" : kind == DefRecordKind::Weapon ? "weapon_name" : "name";
	return std::get<std::string>(def_get(record(), *def_field(kind, field)));
}
DefRecordKind EditableDocument::record_kind() const {
	return kind_ == AssetKind::ItemDefs ? DefRecordKind::Item : kind_ == AssetKind::WeaponDefs ? DefRecordKind::Weapon : DefRecordKind::Ammo;
}
size_t EditableDocument::row_index(CatalogId id) const {
	for (size_t i = 0; i < rows_.size(); ++i) if (rows_[i]->id == id) return i;
	return rows_.size();
}
void EditableDocument::identify_children(CatalogRow &row) {
	size_t children = 0, sights = 0;
	if (auto *p = std::get_if<DefItemDef>(&row.data)) children = p->emplacement_attachments_count;
	if (auto *p = std::get_if<DefWeaponDef>(&row.data)) { children = p->actions_count; sights = p->sights_count; }
	if (auto *p = std::get_if<DefAmmoDef>(&row.data)) children = p->effects_table_count;
	row.children.clear(); row.sights.clear();
	while (row.children.size() < children) row.children.push_back(next_id_++);
	while (row.sights.size() < sights) row.sights.push_back(next_id_++);
}
bool EditableDocument::load(const std::string &absolute, const std::string &relative, AssetKind kind,
	const std::string &game, Diagnostic &error) {
	if (!is_catalog_kind(kind)) return fail(error, relative, "document.kind", "This file has no catalog editor.");
	std::vector<uint8_t> bytes; std::string message;
	if (!read_file_bytes(absolute, bytes, message)) return fail(error, relative, "document.read", message);
	const uint64_t hash = fingerprint(bytes);
	if (!opennova::vfs_decode_payload(bytes, gameprofile::gameprofile_scr_policy_for_code(game.c_str())))
		return fail(error, relative, "document.decode", "The definition file could not be decoded.");
	// The C parsers permit an empty file, but require a non-null input pointer.
	if (bytes.empty()) bytes.push_back(0);
	rows_.clear(); spawn_ids_.clear(); parse_issues_.clear(); history_.clear();
	cursor_ = 0; next_id_ = 1; next_revision_ = 1; revision_ = saved_revision_ = 0;
	absolute_path_ = absolute; relative_path_ = relative; kind_ = kind;
	auto append = [&](DefRecordKind type, const auto &entry) {
		auto row = std::make_shared<CatalogRow>(type);
		row->data = entry; // transfer the parser-owned arrays; file release is handled below
		row->id = next_id_++; identify_children(*row); rows_.push_back(row);
	};
	if (kind == AssetKind::ItemDefs) {
		DefItemsFile file{}; def_parse_items_memory(bytes.data(), bytes.size(), &file, &parse_issues_);
		spawn_ids_.assign(file.vehicle_spawn_ids, file.vehicle_spawn_ids + file.vehicle_spawn_id_count);
		for (size_t i = 0; i < file.count; ++i) append(DefRecordKind::Item, file.entries[i]);
		std::free(file.entries);
	} else if (kind == AssetKind::WeaponDefs) {
		DefWeaponsFile file{}; def_parse_weapons_memory(bytes.data(), bytes.size(), &file, &parse_issues_);
		for (size_t i = 0; i < file.ammo_classes_count; ++i) append(DefRecordKind::Carry, file.ammo_classes[i]);
		for (size_t i = 0; i < file.count; ++i) append(DefRecordKind::Weapon, file.entries[i]);
		std::free(file.entries); std::free(file.ammo_classes);
	} else {
		DefAmmoFile file{}; def_parse_ammo_memory(bytes.data(), bytes.size(), &file, &parse_issues_);
		for (size_t i = 0; i < file.count; ++i) append(DefRecordKind::Ammo, file.entries[i]);
		std::free(file.entries);
	}
	file_fingerprint_ = hash; coalesce_key_.clear(); return true;
}
const void *EditableDocument::record(const CatalogAddress &address) const {
	const size_t i = row_index(address.row);
	return i < rows_.size() ? child_record(const_cast<CatalogRow &>(*rows_[i]), address) : nullptr;
}
DefWriteResult EditableDocument::serialize() const {
	if (!parse_issues_.empty()) return {"", parse_issues_};
	// Only the contiguous outer arrays are temporary. Nested arrays remain owned
	// by the native rows; the writer borrows them for the duration of this call.
	if (kind_ == AssetKind::ItemDefs) {
		std::vector<DefItemDef> items;
		for (const auto &row : rows_) items.push_back(std::get<DefItemDef>(row->data));
		DefItemsFile file{}; file.entries = items.data(); file.count = items.size();
		file.vehicle_spawn_id_count = int(spawn_ids_.size());
		std::copy(spawn_ids_.begin(), spawn_ids_.end(), file.vehicle_spawn_ids);
		return def_write_items(file);
	}
	if (kind_ == AssetKind::WeaponDefs) {
		std::vector<DefWeaponDef> weapons; std::vector<DefAmmoClassCarry> carries;
		for (const auto &row : rows_) {
			if (row->kind == DefRecordKind::Carry) carries.push_back(std::get<DefAmmoClassCarry>(row->data));
			else weapons.push_back(std::get<DefWeaponDef>(row->data));
		}
		DefWeaponsFile file{}; file.entries = weapons.data(); file.count = weapons.size();
		file.ammo_classes = carries.data(); file.ammo_classes_count = carries.size();
		return def_write_weapons(file);
	}
	std::vector<DefAmmoDef> ammo;
	for (const auto &row : rows_) ammo.push_back(std::get<DefAmmoDef>(row->data));
	DefAmmoFile file{}; file.entries = ammo.data(); file.count = ammo.size();
	return def_write_ammo(file);
}
bool EditableDocument::save(Diagnostic &error) {
	const auto output = serialize();
	if (!output.ok()) return fail(error, path(), "document.unserializable", output.diagnostics.front().message, output.diagnostics.front().field);
	std::vector<uint8_t> current; std::string message;
	if (!read_file_bytes(absolute_path_, current, message) || fingerprint(current) != file_fingerprint_)
		return fail(error, path(), "document.conflict", "This file changed outside the editor. Reload it before saving.");
	if (!write_file_atomic(absolute_path_, output.text, message)) return fail(error, path(), "document.write", message);
	file_fingerprint_ = fingerprint(std::vector<uint8_t>(output.text.begin(), output.text.end()));
	saved_revision_ = revision_; end_edit_group(); return true;
}
void EditableDocument::restore(const Change &c, bool forward) {
	const auto &old = forward ? c.before : c.after;
	const auto &replacement = forward ? c.after : c.before;
	if (old) {
		const size_t index = row_index(old->id);
		if (index < rows_.size()) rows_.erase(rows_.begin() + index);
	}
	if (replacement) {
		const size_t position = std::min(forward ? c.after_position : c.before_position, rows_.size());
		rows_.insert(rows_.begin() + position, replacement);
	}
	spawn_ids_ = forward ? c.after_spawn : c.before_spawn;
	revision_ = forward ? c.after_revision : c.before_revision;
}
void EditableDocument::commit(Change c, const std::string &key) {
	c.before_revision = revision_; c.after_revision = next_revision_++;
	if (cursor_ < history_.size()) history_.erase(history_.begin() + cursor_, history_.end());
	restore(c, true);
	if (!key.empty() && key == coalesce_key_ && cursor_ && history_.back().after_revision != saved_revision_) {
		history_.back().after = c.after; history_.back().after_revision = c.after_revision;
		history_.back().after_spawn = c.after_spawn;
	} else { history_.push_back(std::move(c)); ++cursor_; }
	coalesce_key_ = key;
}
bool EditableDocument::apply(const CatalogEdit &edit, Diagnostic &error) {
	if (blocked()) return fail(error, path(), "document.parse", "Fix the reported source errors and reload this document before editing.");
	Change change; change.before_spawn = change.after_spawn = spawn_ids_;
	if (edit.operation == CatalogOperation::SetSpawnId) {
		const auto *value = std::get_if<int64_t>(&edit.value);
		if (kind_ != AssetKind::ItemDefs || !value || edit.position > spawn_ids_.size() || edit.position >= 32 || *value < INT32_MIN || *value > INT32_MAX)
			return fail(error, path(), "document.value", "Invalid vehicle spawn slot.");
		if (edit.position == change.after_spawn.size()) change.after_spawn.push_back(int(*value));
		else change.after_spawn[edit.position] = int(*value);
		commit(std::move(change), {}); return true;
	}
	const size_t index = row_index(edit.address.row);
	const bool top = edit.address.kind == record_kind() ||
		(kind_ == AssetKind::WeaponDefs && edit.address.kind == DefRecordKind::Carry);
	if (edit.operation != CatalogOperation::Add || !top) {
		if (index == rows_.size()) return fail(error, path(), "document.selection", "The selected record no longer exists.");
		change.before = rows_[index]; change.before_position = index;
	}
	std::shared_ptr<CatalogRow> updated;
	if (top && edit.operation == CatalogOperation::Add) {
		updated = std::make_shared<CatalogRow>(edit.address.kind);
		updated->id = last_added_ = next_id_++;
		const char *key = edit.address.kind == DefRecordKind::Item ? "display_name" : edit.address.kind == DefRecordKind::Weapon ? "weapon_name" : "name";
		std::string ignored;
		def_set(updated->record(), *def_field(edit.address.kind, key), std::string("New_") + std::to_string(updated->id), ignored);
		if (auto *p = std::get_if<DefItemDef>(&updated->data)) {
			p->type = DEF_ITEM_TYPE_MARKER;
			p->id = 100000;
			for (;;) {
				bool used = false;
				for (const auto &row : rows_) if (std::get<DefItemDef>(row->data).id == p->id) { used = true; break; }
				if (!used) break;
				++p->id;
			}
		}
		change.after_position = std::min(edit.position, rows_.size());
	} else if (top && edit.operation == CatalogOperation::Remove) {
        // A file-wide spawn registry has no native home once the last item is removed.
        if (kind_ == AssetKind::ItemDefs && rows_.size() == 1) change.after_spawn.clear();
        change.after_position = index;
	} else {
		updated = std::make_shared<CatalogRow>(*change.before);
		change.after_position = index;
		if (edit.operation == CatalogOperation::Set) {
			void *target = child_record(*updated, edit.address);
			const auto *field = def_field(edit.address.kind, edit.field);
			std::string message;
			if (!target || !field || !def_set(target, *field, edit.value, message))
				return fail(error, path(), "document.value", message.empty() ? "Unknown field." : message, edit.field);
			def_sync_derived(edit.address.kind, target, edit.field);
		} else if (top && edit.operation == CatalogOperation::Duplicate) {
			updated->id = last_added_ = next_id_++; identify_children(*updated);
			change.before.reset(); change.after_position = std::min(edit.position, rows_.size());
		} else if (top && edit.operation == CatalogOperation::Move) {
			change.after_position = std::min(edit.position, rows_.size() - 1);
		} else {
			bool ok = false;
			if (auto *p = std::get_if<DefItemDef>(&updated->data); p && edit.address.kind == DefRecordKind::Attachment) {
				ok = edit_children(p->emplacement_attachments, p->emplacement_attachments_count, updated->children, edit, next_id_, last_added_);
			}
			if (auto *p = std::get_if<DefWeaponDef>(&updated->data)) {
				if (edit.address.kind == DefRecordKind::Action) ok = edit_children(p->actions, p->actions_count, updated->children, edit, next_id_, last_added_);
				if (edit.address.kind == DefRecordKind::Sight) ok = edit_children(p->sights, p->sights_count, updated->sights, edit, next_id_, last_added_);
			}
			if (auto *p = std::get_if<DefAmmoDef>(&updated->data); p && edit.address.kind == DefRecordKind::Effect)
				ok = edit_children(p->effects_table, p->effects_table_count, updated->children, edit, next_id_, last_added_);
			if (!ok) return fail(error, path(), "document.collection", "This collection cannot accept that edit.");
		}
	}
	if (updated) {
		if (auto *p = std::get_if<DefItemDef>(&updated->data)) update_attachment_slots(*p);
		change.after = updated;
	}
	const std::string key = edit.coalesce && edit.operation == CatalogOperation::Set
		? std::to_string(edit.address.row) + "/" + std::to_string(edit.address.child) + "/" + edit.field : "";
	commit(std::move(change), key); return true;
}
void EditableDocument::undo() { if (cursor_) { restore(history_[--cursor_], false); end_edit_group(); } }
void EditableDocument::redo() { if (cursor_ < history_.size()) { restore(history_[cursor_++], true); end_edit_group(); } }

} // namespace opennova::editor
