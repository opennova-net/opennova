// The def catalogs' table (def_table.h): the kinds, each record's members as labelled fields over the
// format's member inventory and line table, the lists a record holds (its C arrays, a powerup's action
// blocks), and the families with their parsers and writers.
#include "def_table.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#include <base/io/strutil.h>
#include <runtime/hud/game_text_lookup.h>

namespace opennova::editor {
using namespace def;

namespace {

// --- the kinds ---------------------------------------------------------------------------------------

// A new item is a marker with an id no item of its file has, from 100000 on (an item's id is its
// type_id, which a mission names it by).
void made_item(void *record, const std::vector<const void *> &others) {
	auto &item = *static_cast<DefItemDef *>(record);
	item.type = DEF_ITEM_TYPE_MARKER;
	item.id = 100000;
	for (;;) {
		bool used = false;
		for (const void *other : others) used = used || static_cast<const DefItemDef *>(other)->id == item.id;
		if (!used) break;
		++item.id;
	}
}

// An item's attachment slots: the 1-based place of its last G and C attachment (zero: none).
void attachment_slots(void *record) {
	auto &item = *static_cast<DefItemDef *>(record);
	item.emplacement_g_slot = 0;
	item.emplacement_c_slot = 0;
	for (size_t i = 0; i < item.emplacement_attachments_count; ++i) {
		if (item.emplacement_attachments[i].kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G) item.emplacement_g_slot = int(i + 1);
		if (item.emplacement_attachments[i].kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C) item.emplacement_c_slot = int(i + 1);
	}
}

using C = CatalogKind;
using R = DefRecordKind;
constexpr CatalogKindRow kKinds[] = {
	{C::Item, R::Item, "item", "Item", "Add record", true, "display_name", made_item, attachment_slots},
	{C::Weapon, R::Weapon, "weapon", "Weapon", "Add record", true, "weapon_name"},
	{C::Ammo, R::Ammo, "ammo", "Ammo", "Add record", true, "name"},
	{C::Action, R::Action, "action", "Action", "", false, "name"},
	{C::Sight, R::Sight, "sight", "Sight", "", false, "texture"},
	{C::Attachment, R::Attachment, "attachment", "Attachment", "", false, "userpoint"},
	{C::Effect, R::Effect, "effect", "Effect", "", false, "surface_type"},
	{C::Carry, R::Carry, "carry", "Carry limit", "Add carry limit", true, "name"},
	{C::Powerup, R::Powerup, "powerup", "Powerup", "Add record", true, "name"},
	{C::PowerupAmmo, R::PowerupAmmo, "powerup_ammo", "Ammo", "", false, "class_name"},
	{C::Pickup, R::PowerupAction, "pickup", "Pickup", "", false, ""},
	{C::Respawn, R::PowerupAction, "respawn", "Respawn", "", false, ""},
};
static_assert(std::size(kKinds) == kCatalogKindCount, "every CatalogKind has exactly one row");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) ++a, ++b;
	return *a == *b;
}
constexpr bool kinds_in_order() {
	for (size_t i = 0; i < std::size(kKinds); ++i) {
		if (size_t(kKinds[i].kind) != i) return false;
		// Every record kind at its own place, the second action block after them.
		if (i < kDefRecordKindCount && size_t(kKinds[i].record) != i) return false;
		for (size_t j = i + 1; j < std::size(kKinds); ++j)
			if (same_text(kKinds[i].token, kKinds[j].token)) return false;
	}
	return true;
}
static_assert(kinds_in_order(), "the catalog's kinds follow CatalogKind (each DefRecordKind at its own place), "
                                "each token its own");

// --- the arrays a record holds -------------------------------------------------------------------------

// A record's C array of child records: the owner's kind, the children's, and where the owner keeps
// the array and its count. The arrays are what a row owns (CatalogRecord) and what its lists edit.
struct CArray {
	R owner, element;
	size_t pointer, count;
};
constexpr CArray kArrays[] = {
	{R::Item, R::Attachment, offsetof(DefItemDef, emplacement_attachments), offsetof(DefItemDef, emplacement_attachments_count)},
	{R::Weapon, R::Action, offsetof(DefWeaponDef, actions), offsetof(DefWeaponDef, actions_count)},
	{R::Weapon, R::Sight, offsetof(DefWeaponDef, sights), offsetof(DefWeaponDef, sights_count)},
	{R::Ammo, R::Effect, offsetof(DefAmmoDef, effects_table), offsetof(DefAmmoDef, effects_table_count)},
	{R::Powerup, R::PowerupAmmo, offsetof(DefPowerupDef, ammo), offsetof(DefPowerupDef, ammo_count)},
};

void *&array_of(void *record, const CArray &array) {
	return *reinterpret_cast<void **>(static_cast<uint8_t *>(record) + array.pointer);
}
size_t &count_of(void *record, const CArray &array) {
	return *reinterpret_cast<size_t *>(static_cast<uint8_t *>(record) + array.count);
}
size_t count_of(const void *record, const CArray &array) {
	return *reinterpret_cast<const size_t *>(static_cast<const uint8_t *>(record) + array.count);
}

void *copy_array(const void *data, size_t bytes) {
	if (!bytes || !data) return nullptr;
	void *out = std::malloc(bytes);
	if (!out) throw std::bad_alloc();
	std::memcpy(out, data, bytes);
	return out;
}

// --- the labelled fields ------------------------------------------------------------------------------

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

// The symbol a record's field defines, which the other catalogs and the game name it by: a weapon's
// name, an ammo's name, an item's id.
struct Defines {
	R kind;
	const char *field;
	ReferenceKind defines;
};
constexpr Defines kDefines[] = {
	{R::Weapon, "weapon_name", ReferenceKind::Weapon},
	{R::Ammo, "name", ReferenceKind::Ammo},
	{R::Item, "id", ReferenceKind::Item},
};

// Where a field's reference resolves, when the field says: a def's game-text fields are string ids in
// the game's own table, the loadout label's in its WepDes section (menu::weapon_label), a weapon's
// attach label in its Overlays section (the runtime's weapon table attach_text_id); another text key
// is not resolved yet.
void resolve_field(const DefField &field, FieldSchema &entry) {
	const std::string game_text = strutil::to_upper(hud::kGameTextTable) + "/";
	if (field.reference == DefReference::GameText) {
		entry.scope = game_text + hud::kGameTextWepDes;
	} else if (field.reference == DefReference::OtherText && field.id == "attach_text_id") {
		entry.reference = ReferenceKind::TextId;
		entry.scope = game_text + hud::kGameTextOverlays;
	}
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

// Whether the field is a line's present flag (DefProperty::present_field): its tick is the line's.
bool is_present_flag(R kind, const std::string &id) {
	for (const DefProperty &property : def_properties(kind))
		if (property.present_field == id) return true;
	return false;
}

// What the field's line says of it (its DefMember): the key the file writes, the parser's heading and
// note; a member the line writes as a number of its own in the units the file writes it, named by the
// key (and its place on a line of several) unless the line names it; a light's colour packed
// 0xRRGGBB; a whole percent kept to the parser's 0..100; a field whose line has a present flag
// optional.
void describe(const DefMember &member, FieldSchema &entry) {
	const DefProperty *property = member.property;
	if (!property) return;
	const size_t index = member.index;
	if (!property->key.empty() && property->key != member.field->id) entry.token = property->key;
	entry.section = property->section;
	entry.description = property->note;
	const std::string label = index < property->labels.size() ? property->labels[index] : std::string();
	if (member.authored != DefAuthored::None) {
		entry.type = member.authored == DefAuthored::Integer ? FieldType::Integer : FieldType::Real;
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
	if (member.field->ranged) {
		entry.ranged = true;
		entry.min = double(member.field->min);
		entry.max = double(member.field->max);
	}
	// [orig: light_transfer atoi clamped 0..100 @ 0x4A1A12..0x4A1A50]
	if (property->encoding == DefEncoding::Percent) {
		entry.ranged = true;
		entry.min = 0.0;
		entry.max = 100.0;
	}
	if ((property->encoding == DefEncoding::LightMove || property->encoding == DefEncoding::LightImpact) && index == 1)
		entry.color = FieldColor::PackedRgb;
	if (member.present) entry.optional = true;
	// A unit a plain line states (a powerup's respawn time in seconds).
	if (member.authored == DefAuthored::None && entry.unit.empty() && property->fields.size() == 1 &&
	    property->units.size() == 1)
		entry.unit = property->units.front();
}

// A member's labelled field: its schema from the member inventory and its line, its value through the
// member (in written units where its line writes it as a number of its own, as stored otherwise), its
// presence its line's flag. The member is found once, here: a set reads nothing by its id again.
LabelledField labelled(R kind, const DefField &field) {
	const DefMember member = def_member(kind, field.id);
	LabelledField out;
	FieldSchema &entry = out.schema;
	entry.id = field.id;
	entry.type = field_type(field.type);
	entry.width = field.width;
	entry.reference = reference_kind(field.reference);
	resolve_field(field, entry);
	for (const Defines &row : kDefines)
		if (row.kind == kind && field.id == row.field) entry.defines = row.defines;
	for (const DefChoice &choice : field.choices) entry.choices.push_back({choice.name, choice.value, choice.label});
	entry.flags = field.flags;
	entry.open_choices = field.open;
	const bool flag = is_present_flag(kind, field.id);
	entry.read_only = field.read_only || flag;
	describe(member, entry);

	const auto shared = std::make_shared<const DefMember>(member);
	out.value.get = [shared](const RecordHandle &record, Value &value) {
		// In written units while the line carries the stored word, else as stored.
		if (shared->authored != DefAuthored::None && def_authored_get(*shared, record.data, value)) return true;
		value = def_get(record.data, *shared->field);
		return true;
	};
	out.value.set = [shared, flag](const RecordHandle &record, const Value &value, std::string &error) {
		if (flag) {
			error = "Whether the line is written is the field's own tick.";
			return false;
		}
		const DefValue before = def_get(record.data, *shared->field);
		// A number in the units the file writes it goes through the file's own line.
		const bool set = shared->authored != DefAuthored::None ? def_authored_set(*shared, record.data, value, error)
		                                                       : def_set(record.data, *shared->field, value, error);
		if (!set) return false;
		// A Set of another value writes an optional line (edit.h); one of the value it holds changes
		// nothing.
		if (shared->present && def_get(record.data, *shared->field) != before &&
		    !def_set(record.data, *shared->present, int64_t(1), error))
			return false;
		def_sync_derived(shared->kind, record.data, shared->field->id);
		return true;
	};
	if (member.present) {
		out.value.present = [shared](const RecordHandle &record) {
			return std::get<int64_t>(def_get(record.data, *shared->present)) != 0;
		};
		out.value.set_present = [shared](const RecordHandle &record, bool present, std::string &error) {
			return def_set(record.data, *shared->present, int64_t(present ? 1 : 0), error);
		};
	}
	return out;
}

// --- the lists ---------------------------------------------------------------------------------------

// A C array's list: its records copied in and out by their bytes (no child record owns an array of its
// own), a new one of its kind's defaults.
ListOps array_list(const CArray &array, NodeKind kind) {
	const size_t size = def_record_size(array.element);
	const R element = array.element;
	ListOps ops;
	ops.size = [array](const RecordHandle &owner) { return count_of(static_cast<const void *>(owner.data), array); };
	ops.at = [array, kind, size](const RecordHandle &owner, size_t index) {
		if (index >= count_of(static_cast<const void *>(owner.data), array)) return RecordHandle{};
		return RecordHandle{kind, static_cast<uint8_t *>(array_of(owner.data, array)) + index * size};
	};
	ops.insert = [array, kind, size, element](const RecordHandle &owner, size_t index, const DetachedRecord *record,
	                                          std::string &error) {
		if (record && (!record->data || record->kind != kind)) {
			error = "This list takes records of its own kind only.";
			return false;
		}
		void *&data = array_of(owner.data, array);
		size_t &count = count_of(owner.data, array);
		index = std::min(index, count);
		auto *grown = static_cast<uint8_t *>(std::malloc((count + 1) * size));
		if (!grown) throw std::bad_alloc();
		if (index) std::memcpy(grown, data, index * size);
		if (record) std::memcpy(grown + index * size, record->data.get(), size);
		else def_init_record(element, grown + index * size);
		if (count > index)
			std::memcpy(grown + (index + 1) * size, static_cast<const uint8_t *>(data) + index * size, (count - index) * size);
		std::free(data);
		data = grown;
		++count;
		return true;
	};
	ops.erase = [array, size](const RecordHandle &owner, size_t index) {
		void *&data = array_of(owner.data, array);
		size_t &count = count_of(owner.data, array);
		if (index >= count) return false;
		void *shrunk = nullptr;
		if (count > 1) {
			auto *kept = static_cast<uint8_t *>(std::malloc((count - 1) * size));
			if (!kept) throw std::bad_alloc();
			if (index) std::memcpy(kept, data, index * size);
			if (count > index + 1)
				std::memcpy(kept + index * size, static_cast<const uint8_t *>(data) + (index + 1) * size,
				            (count - index - 1) * size);
			shrunk = kept;
		}
		std::free(data);
		data = shrunk;
		--count;
		return true;
	};
	ops.copy = [array, kind, size](const RecordHandle &owner, size_t index) {
		DetachedRecord out;
		if (index >= count_of(static_cast<const void *>(owner.data), array)) return out;
		void *copy = copy_array(static_cast<const uint8_t *>(array_of(owner.data, array)) + index * size, size);
		out.kind = kind;
		out.data = std::shared_ptr<void>(copy, std::free);
		return out;
	};
	return ops;
}

// A powerup row's action block: written or not (its `present`), at most one in its list. A new one is
// the block the parser opens, zeroed but for its present flag [orig: PowerUpDef_ParseProperty
// @0x443056..0x4430F4, the memset and present store].
ListOps block_list(DefPowerupAction DefPowerupDef::*block, NodeKind kind, const char *label) {
	ListOps ops;
	ops.size = [block](const RecordHandle &owner) -> size_t { return (owner.as<DefPowerupDef>().*block).present ? 1 : 0; };
	ops.at = [block, kind](const RecordHandle &owner, size_t index) {
		DefPowerupAction &action = owner.as<DefPowerupDef>().*block;
		return index == 0 && action.present ? RecordHandle{kind, &action} : RecordHandle{};
	};
	ops.insert = [block, kind, label](const RecordHandle &owner, size_t, const DetachedRecord *record, std::string &error) {
		DefPowerupAction &action = owner.as<DefPowerupDef>().*block;
		if (record && (!record->data || record->kind != kind)) {
			error = "This list takes records of its own kind only.";
			return false;
		}
		if (action.present) {
			error = std::string("A powerup holds one ") + label + " block at most.";
			return false;
		}
		if (record) action = *static_cast<const DefPowerupAction *>(record->data.get());
		else std::memset(&action, 0, sizeof(action));
		action.present = 1;
		return true;
	};
	ops.erase = [block](const RecordHandle &owner, size_t index) {
		DefPowerupAction &action = owner.as<DefPowerupDef>().*block;
		if (index != 0 || !action.present) return false;
		std::memset(&action, 0, sizeof(action));
		return true;
	};
	ops.copy = [block, kind](const RecordHandle &owner, size_t index) {
		DetachedRecord out;
		const DefPowerupAction &action = owner.as<DefPowerupDef>().*block;
		if (index != 0 || !action.present) return out;
		out.kind = kind;
		out.data = std::make_shared<DefPowerupAction>(action);
		return out;
	};
	return ops;
}

Document::CollectionSpec spec(CatalogKind kind, const char *label, const char *name_field, size_t max = 0) {
	Document::CollectionSpec out;
	out.kind = node_kind(kind);
	out.label = label;
	out.name_field = name_field;
	out.max = max;
	return out;
}

constexpr const CArray *array_for(R owner, R element) {
	for (const CArray &array : kArrays)
		if (array.owner == owner && array.element == element) return &array;
	return nullptr;
}

// The lists a kind's records hold, in the order its writer emits them: a C array the record owns (its
// CArray row), or a powerup's action block, written or not, a list of one at most. A family's records
// that hold lists are rows here.
struct ListRow {
	C owner, kind;
	const char *label;
	const char *name_field;
	DefPowerupAction DefPowerupDef::*block; // null: the owner's C array of `kind`
};
constexpr ListRow kLists[] = {
	{C::Item, C::Attachment, "Attachments", "userpoint", nullptr},
	{C::Weapon, C::Action, "Actions", "name", nullptr},
	{C::Weapon, C::Sight, "Sights", "texture", nullptr},
	{C::Ammo, C::Effect, "Effects", "surface_type", nullptr},
	{C::Powerup, C::PowerupAmmo, "Ammo", "class_name", nullptr},
	{C::Powerup, C::Pickup, "Pickup", "", &DefPowerupDef::pickup},
	{C::Powerup, C::Respawn, "Respawn", "", &DefPowerupDef::respawn},
};
// Each list holds records nested in its owner, of a kind no other list holds, and a C array's list
// names an array its owner's record has.
constexpr bool lists_well_formed() {
	for (size_t i = 0; i < std::size(kLists); ++i) {
		const ListRow &list = kLists[i];
		if (kKinds[size_t(list.kind)].top || list.owner == list.kind) return false;
		if (!list.block && !array_for(kKinds[size_t(list.owner)].record, kKinds[size_t(list.kind)].record)) return false;
		for (size_t j = i + 1; j < std::size(kLists); ++j)
			if (kLists[j].kind == list.kind) return false;
	}
	return true;
}
static_assert(lists_well_formed(), "each list holds nested records of a kind of its own, an array its owner has");

RecordTable make_table() {
	std::vector<TableKind> kinds;
	for (const CatalogKindRow &row : kKinds) {
		TableKind kind(RecordKindRow{node_kind(row.kind), row.token, row.label, row.add_label, row.top});
		for (const DefField &field : def_fields(row.record)) kind.field(labelled(row.record, field));
		for (const ListRow &list : kLists) {
			if (list.owner != row.kind) continue;
			const CatalogKindRow &held = kKinds[size_t(list.kind)];
			kind.list({spec(list.kind, list.label, list.name_field, list.block ? 1 : 0),
			           list.block ? block_list(list.block, node_kind(list.kind), held.token)
			                      : array_list(*array_for(row.record, held.record), node_kind(list.kind))});
		}
		kinds.push_back(std::move(kind));
	}
	return RecordTable(std::move(kinds));
}

// --- the families ------------------------------------------------------------------------------------

constexpr uint32_t bit(CatalogKind kind) { return uint32_t(1) << unsigned(kind); }

const ItemsFileState *items_state(const FileState *state) { return dynamic_cast<const ItemsFileState *>(state); }

bool parse_items(const std::vector<uint8_t> &bytes, std::vector<CatalogRecord> &rows,
                 std::shared_ptr<const FileState> &state, DefParseReport &report) {
	DefItemsFile file{};
	def_parse_items_memory(bytes.data(), bytes.size(), &file, &report);
	auto spawn = std::make_shared<ItemsFileState>();
	spawn->spawn_ids.assign(file.vehicle_spawn_ids, file.vehicle_spawn_ids + file.vehicle_spawn_id_count);
	state = spawn;
	for (size_t i = 0; i < file.count; ++i) rows.emplace_back(R::Item, &file.entries[i]);
	std::free(file.entries); // each row took its record's arrays
	return true;
}

DefWriteResult write_items(const std::vector<const CatalogRecord *> &rows, const FileState *state) {
	// Only the contiguous outer array is temporary: the rows' arrays are borrowed for the call.
	std::vector<DefItemDef> items;
	for (const CatalogRecord *row : rows) items.push_back(row->as<DefItemDef>());
	DefItemsFile file{};
	file.entries = items.data();
	file.count = items.size();
	if (const ItemsFileState *spawn = items_state(state)) {
		file.vehicle_spawn_id_count = int(spawn->spawn_ids.size());
		std::copy(spawn->spawn_ids.begin(), spawn->spawn_ids.end(), file.vehicle_spawn_ids);
	}
	return def_write_items(file);
}

// items.def's vehicle spawn registry: an id at a slot, a new one at the end, up to its 32 slots.
bool set_spawn_slot(std::shared_ptr<const FileState> &state, const Edit &edit, std::string &error) {
	const auto *value = std::get_if<int64_t>(&edit.value);
	const ItemsFileState *current = items_state(state.get());
	const size_t count = current ? current->spawn_ids.size() : 0;
	if (!value || edit.position > count || edit.position >= size_t(DEF_VEHICLE_SPAWN_SLOTS) || *value < INT32_MIN ||
	    *value > INT32_MAX) {
		error = "Invalid vehicle spawn slot.";
		return false;
	}
	auto updated = current ? std::static_pointer_cast<ItemsFileState>(current->clone()) : std::make_shared<ItemsFileState>();
	if (edit.position == updated->spawn_ids.size()) updated->spawn_ids.push_back(int(*value));
	else updated->spawn_ids[edit.position] = int(*value);
	state = updated;
	return true;
}

// The same ids in the same slots.
bool same_spawn_registry(const FileState *a, const FileState *b) {
	static const std::vector<int> none;
	const ItemsFileState *before = items_state(a), *after = items_state(b);
	return (before ? before->spawn_ids : none) == (after ? after->spawn_ids : none);
}

// A file-wide spawn registry has no native home once the last item is removed.
std::shared_ptr<const FileState> spawn_registry_after_remove(const std::shared_ptr<const FileState> &state,
                                                             size_t remaining) {
	return remaining == 0 ? std::make_shared<ItemsFileState>() : state;
}

bool parse_weapons(const std::vector<uint8_t> &bytes, std::vector<CatalogRecord> &rows,
                   std::shared_ptr<const FileState> &, DefParseReport &report) {
	DefWeaponsFile file{};
	def_parse_weapons_memory(bytes.data(), bytes.size(), &file, &report);
	for (size_t i = 0; i < file.ammo_classes_count; ++i) rows.emplace_back(R::Carry, &file.ammo_classes[i]);
	for (size_t i = 0; i < file.count; ++i) rows.emplace_back(R::Weapon, &file.entries[i]);
	std::free(file.entries);
	std::free(file.ammo_classes);
	return true;
}

DefWriteResult write_weapons(const std::vector<const CatalogRecord *> &rows, const FileState *) {
	std::vector<DefWeaponDef> weapons;
	std::vector<DefAmmoClassCarry> carries;
	for (const CatalogRecord *row : rows) {
		if (row->kind() == R::Carry) carries.push_back(row->as<DefAmmoClassCarry>());
		else weapons.push_back(row->as<DefWeaponDef>());
	}
	DefWeaponsFile file{};
	file.entries = weapons.data();
	file.count = weapons.size();
	file.ammo_classes = carries.data();
	file.ammo_classes_count = carries.size();
	return def_write_weapons(file);
}

// A family whose file is its rows alone ({entries, count}), each a record of one kind.
template <class File, class Entry, R Kind, int (*Parse)(const uint8_t *, size_t, File *, DefParseReport *),
          DefWriteResult (*Write)(const File &)>
struct RowsFamily {
	static bool parse(const std::vector<uint8_t> &bytes, std::vector<CatalogRecord> &rows,
	                  std::shared_ptr<const FileState> &, DefParseReport &report) {
		File file{};
		Parse(bytes.data(), bytes.size(), &file, &report);
		for (size_t i = 0; i < file.count; ++i) rows.emplace_back(Kind, &file.entries[i]);
		std::free(file.entries);
		return true;
	}
	static DefWriteResult write(const std::vector<const CatalogRecord *> &rows, const FileState *) {
		std::vector<Entry> entries;
		for (const CatalogRecord *row : rows) entries.push_back(row->as<Entry>());
		File file{};
		file.entries = entries.data();
		file.count = entries.size();
		return Write(file);
	}
};
using AmmoFamily = RowsFamily<DefAmmoFile, DefAmmoDef, R::Ammo, def_parse_ammo_memory, def_write_ammo>;
using PowerupFamily =
        RowsFamily<DefPowerupFile, DefPowerupDef, R::Powerup, def_parse_powerup_memory, def_write_powerup>;

constexpr CatalogFamily kFamilies[] = {
	{AssetKind::ItemDefs, bit(C::Item) | bit(C::Attachment), C::Item, parse_items, write_items, set_spawn_slot,
	 same_spawn_registry, spawn_registry_after_remove},
	{AssetKind::WeaponDefs, bit(C::Weapon) | bit(C::Action) | bit(C::Sight) | bit(C::Carry), C::Weapon, parse_weapons,
	 write_weapons},
	{AssetKind::AmmoDefs, bit(C::Ammo) | bit(C::Effect), C::Ammo, AmmoFamily::parse, AmmoFamily::write},
	{AssetKind::PowerupDefs, bit(C::Powerup) | bit(C::PowerupAmmo) | bit(C::Pickup) | bit(C::Respawn), C::Powerup,
	 PowerupFamily::parse, PowerupFamily::write},
};

// Every family's kinds are kinds of the table, its first kind one of them and a row of its file, and
// no kind is two families': a kind is one record kind across every file the catalog opens.
constexpr bool families_well_formed() {
	uint32_t seen = 0;
	for (const CatalogFamily &family : kFamilies) {
		if (!family.kinds || (family.kinds >> kCatalogKindCount) != 0 || (seen & family.kinds) != 0) return false;
		if (!family.holds(node_kind(family.first)) || !kKinds[size_t(family.first)].top) return false;
		if (!family.parse || !family.write) return false;
		seen |= family.kinds;
	}
	return true;
}
static_assert(families_well_formed(), "each family names its own kinds of the table, its first a row of its file");

} // namespace

def::DefRecordKind def_kind(NodeKind kind) {
	return kind >= 0 && size_t(kind) < std::size(kKinds) ? kKinds[size_t(kind)].record : R::Item;
}

const CatalogKindRow &catalog_kind_row(NodeKind kind) {
	return kKinds[kind >= 0 && size_t(kind) < std::size(kKinds) ? size_t(kind) : 0];
}

const char *catalog_name_field(NodeKind kind) { return catalog_kind_row(kind).name_field; }

const RecordTable &catalog_table() {
	static const RecordTable table = make_table();
	return table;
}

const CatalogFamily *catalog_family(AssetKind kind) {
	for (const CatalogFamily &family : kFamilies)
		if (family.asset == kind) return &family;
	return nullptr;
}

// --- the native record of a row ------------------------------------------------------------------------

CatalogRecord::CatalogRecord(R kind) : kind_(kind), storage_((def_record_size(kind) + 7) / 8) {
	def_init_record(kind, data());
}

CatalogRecord::CatalogRecord(R kind, const void *parsed) : kind_(kind), storage_((def_record_size(kind) + 7) / 8) {
	std::memcpy(data(), parsed, def_record_size(kind));
}

CatalogRecord::CatalogRecord(const CatalogRecord &other) : kind_(other.kind_), storage_(other.storage_) {
	for (const CArray &array : kArrays)
		if (array.owner == kind_)
			array_of(data(), array) =
			        copy_array(array_of(const_cast<void *>(other.data()), array), count_of(data(), array) *
			                                                                          def_record_size(array.element));
}

CatalogRecord::CatalogRecord(CatalogRecord &&other) noexcept : kind_(other.kind_), storage_(std::move(other.storage_)) {
	other.storage_.clear();
}

CatalogRecord::~CatalogRecord() {
	if (storage_.empty()) return; // moved from
	for (const CArray &array : kArrays)
		if (array.owner == kind_) std::free(array_of(data(), array));
}

size_t CatalogRecord::footprint() const {
	size_t bytes = storage_.size() * sizeof(uint64_t);
	if (storage_.empty()) return bytes;
	for (const CArray &array : kArrays)
		if (array.owner == kind_) bytes += count_of(data(), array) * def_record_size(array.element);
	return bytes;
}

} // namespace opennova::editor
